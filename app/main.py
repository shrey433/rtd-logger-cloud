import asyncio
import base64
import csv
import hmac
import io
import json
import os
import time
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from typing import Optional

from fastapi import FastAPI, Header, HTTPException, Query, Request
from fastapi.responses import Response, StreamingResponse
from fastapi.staticfiles import StaticFiles

from . import backup, db
from .ingest import Payload, store_readings
from .live import hub
from .mqtt_ingest import ingest as mqtt_ingest

MAX_RANGE_S = 400 * 86400
OPEN_PATHS = {"/ingest", "/healthz"}


@asynccontextmanager
async def lifespan(_: FastAPI):
    db.init_db()
    hub.bind(asyncio.get_running_loop())
    if mqtt_ingest.configured():
        mqtt_ingest.start()
    scheduler = asyncio.create_task(backup.scheduler()) if backup.config()["repo"] else None
    yield
    if scheduler:
        scheduler.cancel()
    mqtt_ingest.stop()


app = FastAPI(title="RTD Logger cloud", lifespan=lifespan)


@app.middleware("http")
async def dashboard_auth(request: Request, call_next):
    password = os.environ.get("RTD_DASHBOARD_PASSWORD", "")
    if password and request.url.path not in OPEN_PATHS:
        user = os.environ.get("RTD_DASHBOARD_USER", "admin")
        ok = False
        header = request.headers.get("authorization", "")
        if header.lower().startswith("basic "):
            try:
                decoded = base64.b64decode(header[6:]).decode("utf-8")
                given_user, _, given_pw = decoded.partition(":")
                ok = (hmac.compare_digest(given_user.encode(), user.encode())
                      and hmac.compare_digest(given_pw.encode(), password.encode()))
            except Exception:
                ok = False
        if not ok:
            return Response(status_code=401,
                            headers={"WWW-Authenticate": 'Basic realm="RTD Logger"'})
    return await call_next(request)


def _check_device_token(authorization: Optional[str]) -> None:
    expected = os.environ.get("RTD_API_TOKEN", "")
    if not expected:
        raise HTTPException(503, "ingest disabled: RTD_API_TOKEN is not set on the server")
    scheme, _, token = (authorization or "").partition(" ")
    if scheme.lower() != "bearer" or not hmac.compare_digest(token.encode(), expected.encode()):
        raise HTTPException(401, "invalid device token")


@app.get("/healthz")
def healthz() -> dict:
    mqtt_state = "off"
    if mqtt_ingest.configured():
        mqtt_state = ("subscribed" if mqtt_ingest.subscribed
                      else "connecting" if mqtt_ingest.connected else "disconnected")
    return {"ok": True, "mqtt": mqtt_state}


@app.post("/ingest")
def ingest(payload: Payload, authorization: Optional[str] = Header(None)) -> dict:
    _check_device_token(authorization)
    return store_readings(payload.device_id, payload.fw_version, payload.backlog, payload.readings)


def _device_or_404(device_id: str) -> dict:
    device = db.get_device(device_id)
    if device is None:
        raise HTTPException(404, f"unknown device '{device_id}'")
    return device


@app.get("/api/devices")
def devices() -> dict:
    return {"server_time": int(time.time()), "devices": db.list_devices()}


@app.get("/api/latest")
def latest(device_id: str) -> dict:
    device = _device_or_404(device_id)
    rows = db.recent_rows(device_id, 2)
    return {
        "server_time": int(time.time()),
        "device": device,
        "latest": rows[0] if rows else None,
        "previous": rows[1] if len(rows) > 1 else None,
    }


@app.get("/api/recent")
def recent(device_id: str, n: int = Query(20, ge=1, le=500)) -> dict:
    _device_or_404(device_id)
    return {"rows": db.recent_rows(device_id, n)}


@app.get("/api/readings")
def readings(device_id: str, since: int, until: Optional[int] = None,
             max_points: int = Query(1200, ge=50, le=5000)) -> dict:
    device = _device_or_404(device_id)
    until = until or int(time.time())
    if since >= until:
        raise HTTPException(400, "since must be earlier than until")
    if until - since > MAX_RANGE_S:
        raise HTTPException(400, "range too large (max 400 days)")
    span = until - since
    bucket = max(1, -(-span // max_points))
    if bucket <= 10:  # devices sample every 10 s, so averaging below that adds nothing
        bucket = 1
    data = db.bucketed(device_id, since, until, bucket)
    return {"device_id": device_id, "types": device["types"], "since": since,
            "until": until, "bucket": bucket, **data}


@app.get("/api/backup")
def backup_status() -> dict:
    return backup.info()


@app.get("/api/stream")
async def stream(device_id: Optional[str] = None) -> StreamingResponse:
    """Server-sent events: one `data:` line per new reading, so open dashboards update at once."""
    queue = hub.subscribe()

    async def events():
        try:
            yield "retry: 3000\n\n"
            while True:
                try:
                    event = await asyncio.wait_for(queue.get(), timeout=15)
                except asyncio.TimeoutError:
                    yield ": keep-alive\n\n"
                    continue
                if device_id and event["device_id"] != device_id:
                    continue
                yield f"data: {json.dumps(event)}\n\n"
        finally:
            hub.unsubscribe(queue)

    return StreamingResponse(events(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.get("/api/export.csv")
def export_csv(device_id: str, since: int, until: Optional[int] = None) -> StreamingResponse:
    _device_or_404(device_id)
    until = until or int(time.time())

    def generate():
        buf = io.StringIO()
        writer = csv.writer(buf, lineterminator="\n")
        writer.writerow(["timestamp_utc"] + [f"ch{i}" for i in range(1, db.CHANNELS + 1)])
        yield buf.getvalue()
        for row in db.iter_export(device_id, since, until):
            buf.seek(0)
            buf.truncate()
            stamp = datetime.fromtimestamp(row["ts"], tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            writer.writerow([stamp] + ["" if row[c] is None else f"{row[c]:.2f}" for c in db.COLS])
            yield buf.getvalue()

    filename = f"{device_id}_{since}_{until}.csv"
    return StreamingResponse(generate(), media_type="text/csv",
                             headers={"Content-Disposition": f'attachment; filename="{filename}"'})


app.mount("/", StaticFiles(directory=os.path.join(os.path.dirname(__file__), "static"), html=True),
          name="static")
