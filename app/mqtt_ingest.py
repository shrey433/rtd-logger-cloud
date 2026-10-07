"""MQTT subscriber: devices publish one reading per message on rtd/<device_id>/telemetry."""
import logging
import os
from typing import Optional

import paho.mqtt.client as mqtt
from pydantic import ValidationError

from .ingest import DEVICE_ID_RE, Telemetry, store_readings

log = logging.getLogger("rtd.mqtt")
TOPIC_FILTER = "rtd/+/telemetry"


def handle(topic: str, payload: bytes) -> str:
    """Store one telemetry message. Returns stored | duplicate | rejected | bad | ignored.

    Anything but an exception means the message is finished with and can be acknowledged; a bad
    message will never become valid, so acknowledging it keeps it from being redelivered forever.
    """
    parts = topic.split("/")
    if len(parts) != 3 or parts[0] != "rtd" or parts[2] != "telemetry":
        return "ignored"
    device_id = parts[1]
    if not DEVICE_ID_RE.match(device_id):
        return "bad"
    try:
        msg = Telemetry.model_validate_json(payload)
    except ValidationError as e:
        log.warning("bad telemetry from %s: %s", device_id, e.errors()[:1])
        return "bad"
    result = store_readings(device_id, msg.fw_version, msg.backlog, [msg])
    if result["accepted"]:
        return "stored"
    return "duplicate" if result["duplicates"] else "rejected"


class MqttIngest:
    def __init__(self) -> None:
        self.client: Optional[mqtt.Client] = None
        self.connected = False   # TCP/TLS session to the broker is up
        self.subscribed = False  # broker has confirmed our subscription, so messages will arrive

    @staticmethod
    def configured() -> bool:
        return bool(os.environ.get("MQTT_HOST"))

    def start(self) -> None:
        env = os.environ.get
        port = int(env("MQTT_PORT", "8883"))
        client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=env("MQTT_CLIENT_ID", "rtd-dashboard"),
            clean_session=False,  # the broker keeps QoS 1 messages for us while the app restarts
        )
        client.manual_ack_set(True)  # acknowledge only after the row is safely stored
        if env("MQTT_USERNAME"):
            client.username_pw_set(env("MQTT_USERNAME"), env("MQTT_PASSWORD", ""))
        if env("MQTT_TLS", "1" if port == 8883 else "0") == "1":
            client.tls_set(ca_certs=env("MQTT_CA") or None, certfile=env("MQTT_CERT") or None,
                           keyfile=env("MQTT_KEY") or None)
        client.reconnect_delay_set(min_delay=1, max_delay=30)
        client.on_connect = self._on_connect
        client.on_subscribe = self._on_subscribe
        client.on_disconnect = self._on_disconnect
        client.on_message = self._on_message
        client.connect_async(env("MQTT_HOST"), port, keepalive=60)
        client.loop_start()
        self.client = client
        log.info("MQTT: connecting to %s:%s", env("MQTT_HOST"), port)

    def stop(self) -> None:
        if self.client:
            self.client.loop_stop()
            self.client.disconnect()
            self.client = None
        self.connected = False
        self.subscribed = False

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            log.error("MQTT: connection refused (%s)", reason_code)
            return
        self.connected = True
        client.subscribe(TOPIC_FILTER, qos=1)

    def _on_subscribe(self, client, userdata, mid, reason_codes, properties):
        self.subscribed = all(not rc.is_failure for rc in reason_codes)
        if self.subscribed:
            log.info("MQTT: subscribed to %s", TOPIC_FILTER)
        else:
            log.error("MQTT: broker refused the subscription to %s (check the IoT policy)", TOPIC_FILTER)

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        self.connected = False
        self.subscribed = False
        log.warning("MQTT: disconnected (%s), will retry", reason_code)

    def _on_message(self, client, userdata, msg):
        try:
            handle(msg.topic, msg.payload)
        except Exception:
            # Storage failed: leave it unacknowledged so the broker sends it again after a reconnect.
            log.exception("MQTT: could not store message on %s", msg.topic)
            return
        client.ack(msg.mid, msg.qos)


ingest = MqttIngest()
