import asyncio
import json
import os
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
import pytest
from fastapi.testclient import TestClient

from app.live import Hub

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def iso(epoch):
    return datetime.fromtimestamp(epoch, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def message(epoch, base=24.0, backlog=0):
    return {
        "fw_version": "0.2.0",
        "backlog": backlog,
        "ts": iso(epoch),
        "channels": [
            {"ch": i + 1, "type": "PT1000" if i == 7 else "PT100", "temp_c": base + i * 0.1}
            for i in range(8)
        ],
    }


@pytest.fixture()
def api(tmp_path, monkeypatch):
    monkeypatch.setenv("RTD_DB", str(tmp_path / "m.db"))
    monkeypatch.delenv("MQTT_HOST", raising=False)
    from app.main import app
    with TestClient(app) as c:
        yield c


def test_handle_stores_a_valid_message(api):
    from app.mqtt_ingest import handle
    now = int(time.time())
    body = json.dumps(message(now)).encode()
    assert handle("rtd/rtd-logger-01/telemetry", body) == "stored"
    assert handle("rtd/rtd-logger-01/telemetry", body) == "duplicate"
    device = api.get("/api/devices").json()["devices"][0]
    assert device["device_id"] == "rtd-logger-01" and device["fw_version"] == "0.2.0"
    row = api.get("/api/latest", params={"device_id": "rtd-logger-01"}).json()["latest"]
    assert row["ts"] == now and row["values"][7] == pytest.approx(24.7)


def test_handle_takes_the_device_id_from_the_topic_only(api):
    from app.mqtt_ingest import handle
    body = message(int(time.time()))
    body["device_id"] = "someone-else"  # a device cannot speak for another one
    assert handle("rtd/rtd-logger-02/telemetry", json.dumps(body).encode()) == "stored"
    ids = [d["device_id"] for d in api.get("/api/devices").json()["devices"]]
    assert ids == ["rtd-logger-02"]


@pytest.mark.parametrize("topic,payload,expected", [
    ("rtd/rtd-logger-01/status", b"{}", "ignored"),
    ("other/rtd-logger-01/telemetry", b"{}", "ignored"),
    ("rtd/bad id!/telemetry", b"{}", "bad"),
    ("rtd/rtd-logger-01/telemetry", b"not json", "bad"),
    ("rtd/rtd-logger-01/telemetry", b'{"ts": "2026-01-01T00:00:00Z"}', "bad"),
])
def test_handle_ignores_or_discards_what_it_cannot_use(api, topic, payload, expected):
    from app.mqtt_ingest import handle
    assert handle(topic, payload) == expected


def test_handle_counts_a_row_with_an_unset_clock_as_rejected(api):
    from app.mqtt_ingest import handle
    assert handle("rtd/rtd-logger-01/telemetry", json.dumps(message(100)).encode()) == "rejected"
    assert api.get("/api/devices").json()["devices"] == []


def test_hub_delivers_events_published_from_another_thread():
    async def scenario():
        hub = Hub()
        hub.bind(asyncio.get_running_loop())
        q = hub.subscribe()
        threading.Thread(target=hub.publish, args=({"n": 1},)).start()
        assert await asyncio.wait_for(q.get(), timeout=2) == {"n": 1}
        hub.unsubscribe(q)
        hub.publish({"n": 2})
        await asyncio.sleep(0.05)
        assert q.empty()

    asyncio.run(scenario())


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(predicate, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        value = predicate()
        if value:
            return value
        time.sleep(0.2)
    return None


def test_end_to_end_through_a_real_broker(tmp_path, monkeypatch):
    port = free_port()
    broker = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "dev_broker.py"),
                               "--port", str(port)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        assert wait_for(lambda: socket.socket().connect_ex(("127.0.0.1", port)) == 0), "broker did not start"
        monkeypatch.setenv("RTD_DB", str(tmp_path / "e2e.db"))
        monkeypatch.setenv("MQTT_HOST", "127.0.0.1")
        monkeypatch.setenv("MQTT_PORT", str(port))
        monkeypatch.setenv("MQTT_TLS", "0")
        from app.main import app
        with TestClient(app) as api:
            assert wait_for(lambda: api.get("/healthz").json()["mqtt"] == "subscribed"), "app never subscribed"

            device = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="rtd-logger-01")
            device.connect("127.0.0.1", port)
            device.loop_start()
            now = int(time.time())
            for i in range(3):
                info = device.publish("rtd/rtd-logger-01/telemetry",
                                      json.dumps(message(now - 20 + i * 10, backlog=2 - i)), qos=1)
                info.wait_for_publish(5)
            device.loop_stop()
            device.disconnect()

            rows = wait_for(lambda: len(api.get("/api/recent", params={"device_id": "rtd-logger-01", "n": 10}
                                              ).json().get("rows", [])) == 3)
            assert rows, "readings published over MQTT never reached the database"
            assert api.get("/api/devices").json()["devices"][0]["backlog"] == 0
    finally:
        broker.terminate()
        broker.wait(timeout=10)
