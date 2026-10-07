# RTD Logger cloud

Ingest API and dashboard for the 8-channel RTD temperature logger (ESP32-S3 + MAX31865).
The device firmware lives in a separate repo; this one receives its uploads, stores them, and shows them.

- `POST /ingest` takes the JSON payload from the spec (device token required)
- `GET /` is the dashboard: live channel tiles, zoomable chart with gaps for outages, latest readings, CSV export
- SQLite storage, one wide row per device per timestamp, so a backlog re-sent after a Wi-Fi outage is de-duplicated

## Run locally

```bash
pip install -r requirements-dev.txt
export RTD_API_TOKEN=dev-token          # PowerShell: $env:RTD_API_TOKEN="dev-token"
python -m uvicorn app.main:app --port 8000
python tools/simulate.py --token dev-token --backfill-hours 24 --outage 3:10-3:40 --live
```

Open http://127.0.0.1:8000. Run the tests with `python -m pytest`.

## Configuration

| Variable | Purpose |
|---|---|
| `RTD_API_TOKEN` | Bearer token the loggers send. Unset means `/ingest` answers 503. |
| `RTD_DASHBOARD_PASSWORD` | Turns on HTTP Basic auth for the dashboard and `/api/*` (`/ingest` and `/healthz` stay open). |
| `RTD_DASHBOARD_USER` | Basic auth user, default `admin`. |
| `RTD_DB` | SQLite path, default `data/rtd.db` (`/data/rtd.db` in the Docker image). |

See `.env.example`.

## Deploying

The `Dockerfile` runs on any container host (Fly.io, Railway, Render, a VPS). Two things matter:

1. **Mount a persistent volume on `/data`.** SQLite is a file; without a volume every redeploy wipes the history.
2. **Serve it over HTTPS** and use that `https://` URL as `SERVER_URL` in the firmware. The device token and dashboard password travel in headers.

At 10 s sampling a device adds about 260k rows a month, which SQLite handles comfortably. If you later want a managed database or several writers, the storage code is confined to `app/db.py`.

## Payload

```json
{
  "device_id": "rtd-logger-01",
  "fw_version": "0.1.0",
  "readings": [
    { "ts": "2026-10-07T12:19:20Z",
      "channels": [ { "ch": 1, "type": "PT100", "temp_c": 21.70 }, "... ch 2 to 8 ..." ] }
  ]
}
```

- `readings` holds 1 to 50 entries; the firmware sends the live row plus at most one backlog row.
- `temp_c` is `null` when the MAX31865 reports a fault, and values outside -200 to 850 °C are stored as null.
- A reading with a timestamp before 2024 or more than a day ahead is counted under `rejected` rather than failing the request, so one bad row can never block the device's backlog.
- Response: `{"accepted": n, "duplicates": n, "rejected": n}`. Anything 2xx, or a 400/422, tells the firmware to drop the rows it sent; 401, 5xx and timeouts make it keep and retry them.
