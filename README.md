# RTD Logger cloud

Firmware, ingest API and dashboard for the 8-channel RTD temperature logger (ESP32-S3 + MAX31865).

- `firmware/` is the ESP32-S3 PlatformIO project (see `firmware/README.md`)
- `app/` is the cloud server that receives the uploads, stores them, and serves the dashboard
- `docs/rtd-logger-spec.html` is the design spec: architecture, BOM, wiring and payload format

The sections below cover the cloud server.

- **MQTT in:** loggers publish one reading per message to `rtd/<device_id>/telemetry`; the app subscribes with QoS 1 and acknowledges a message only after it is stored
- `POST /ingest` takes the same readings over HTTPS (bearer token required); the firmware now uses MQTT, so this is kept for testing
- `GET /` is the dashboard: tiles and chart update the moment a reading arrives (server-sent events from `/api/stream`), with gaps for outages, latest readings, and CSV export
- SQLite storage, one wide row per device per timestamp, so a backlog re-sent after a Wi-Fi outage is de-duplicated

## Run locally

```bash
pip install -r requirements-dev.txt
export RTD_API_TOKEN=dev-token          # PowerShell: $env:RTD_API_TOKEN="dev-token"
python tools/dev_broker.py --port 1883 &          # throwaway local MQTT broker, no AWS needed
export MQTT_HOST=127.0.0.1 MQTT_PORT=1883 MQTT_TLS=0
python -m uvicorn app.main:app --port 8000
python tools/simulate.py --token dev-token --backfill-hours 24 --outage 3:10-3:40 --live --mqtt-host 127.0.0.1
```

Open http://127.0.0.1:8000. Without `MQTT_HOST` the app still runs and takes readings over HTTP only. Run the tests with `python -m pytest` (one test starts a real local broker). `GET /healthz` reports `mqtt: subscribed` once the app is receiving.

## Configuration

| Variable | Purpose |
|---|---|
| `RTD_API_TOKEN` | Bearer token the loggers send. Unset means `/ingest` answers 503. |
| `RTD_DASHBOARD_PASSWORD` | Turns on HTTP Basic auth for the dashboard and `/api/*` (`/ingest` and `/healthz` stay open). |
| `RTD_DASHBOARD_USER` | Basic auth user, default `admin`. |
| `RTD_DB` | SQLite path, default `data/rtd.db` (`/data/rtd.db` in the Docker image). |
| `MQTT_HOST` | Broker to subscribe to. Unset means MQTT is off. For AWS IoT Core, the account's `iot:Data-ATS` endpoint. |
| `MQTT_PORT`, `MQTT_TLS` | Default `8883` with TLS on; set `MQTT_TLS=0` for a local broker. |
| `MQTT_CA`, `MQTT_CERT`, `MQTT_KEY` | Root CA and the app's client certificate and key (AWS IoT Core authenticates with these). |
| `MQTT_USERNAME`, `MQTT_PASSWORD` | For brokers that use passwords instead of certificates. |
| `MQTT_CLIENT_ID` | Default `rtd-dashboard`. Fixed, so the broker keeps QoS 1 messages for the app while it restarts. |

See `.env.example` and `deploy/.env.example`.

## Deploying

`deploy/` holds a Docker Compose setup (the app plus Caddy for automatic HTTPS) and `aws_iot_setup.py`, which creates the IoT Core things, certificates and policies. `deploy/AWS_SETUP.md` has the steps for AWS IoT Core plus one EC2 server. Whatever host you use, two things matter:

1. **Keep the `/data` volume.** SQLite is a file; without a persistent volume every redeploy wipes the history.
2. **Serve it over HTTPS.** The dashboard password travels in a header, and the live stream needs a proxy that doesn't buffer (Caddy's `flush_interval -1`, as configured).

At 10 s sampling a device adds about 260k rows a month, which SQLite handles comfortably. If you later want a managed database or several writers, the storage code is confined to `app/db.py`.

## Payload

An MQTT message on `rtd/<device_id>/telemetry` (QoS 1), one reading per message. The device id comes from the topic, and the IoT policy only lets a certificate publish to its own topic.

```json
{
  "fw_version": "0.1.0",
  "backlog": 0,
  "ts": "2026-10-07T12:19:20Z",
  "channels": [ { "ch": 1, "type": "PT100", "temp_c": 21.70 }, "... ch 2 to 8 ..." ]
}
```

- `backlog` (optional) is how many rows are still queued on the device. The dashboard shows it as "Device queue" with the time left to catch up.
- `temp_c` is `null` when the MAX31865 reports a fault, and values outside -200 to 850 °C are stored as null.
- A reading with a timestamp before 2024 or more than a day ahead is discarded rather than stored, so one bad row can never block the device's backlog. Malformed messages are acknowledged and dropped for the same reason; a message is left unacknowledged only if storing it failed, so the broker sends it again.
- The broker's PUBACK is what the firmware treats as delivered. Re-sent rows are de-duplicated by device and timestamp.

The HTTPS `POST /ingest` endpoint takes the same readings wrapped as `{"device_id", "fw_version", "backlog", "readings": [ ... up to 50 ... ]}` with `Authorization: Bearer <RTD_API_TOKEN>`, and answers `{"accepted", "duplicates", "rejected"}`.
