import time
from datetime import datetime, timezone

import pytest
from fastapi.testclient import TestClient

TOKEN = "test-token"
AUTH = {"Authorization": f"Bearer {TOKEN}"}


def iso(epoch: int) -> str:
    return datetime.fromtimestamp(epoch, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def reading(epoch: int, base: float = 24.0, temps=None):
    temps = temps or [base + i * 0.1 for i in range(8)]
    return {
        "ts": iso(epoch),
        "channels": [
            {"ch": i + 1, "type": "PT1000" if i == 7 else "PT100", "temp_c": temps[i]}
            for i in range(8)
        ],
    }


def payload(readings, device="rtd-test"):
    return {"device_id": device, "fw_version": "0.1.0", "readings": readings}


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("RTD_DB", str(tmp_path / "t.db"))
    monkeypatch.setenv("RTD_API_TOKEN", TOKEN)
    monkeypatch.delenv("RTD_DASHBOARD_PASSWORD", raising=False)
    from app.main import app
    with TestClient(app) as c:
        yield c


def test_ingest_requires_valid_token(client):
    now = int(time.time())
    body = payload([reading(now)])
    assert client.post("/ingest", json=body).status_code == 401
    assert client.post("/ingest", json=body, headers={"Authorization": "Bearer nope"}).status_code == 401
    assert client.post("/ingest", json=body, headers=AUTH).status_code == 200


def test_ingest_disabled_without_server_token(client, monkeypatch):
    monkeypatch.delenv("RTD_API_TOKEN")
    r = client.post("/ingest", json=payload([reading(int(time.time()))]), headers=AUTH)
    assert r.status_code == 503


def test_ingest_dedupes_resent_rows(client):
    now = int(time.time())
    body = payload([reading(now - 10), reading(now)])
    first = client.post("/ingest", json=body, headers=AUTH).json()
    again = client.post("/ingest", json=body, headers=AUTH).json()
    assert first == {"accepted": 2, "duplicates": 0, "rejected": 0}
    assert again == {"accepted": 0, "duplicates": 2, "rejected": 0}


def test_rows_with_unset_clock_or_far_future_are_rejected_not_errors(client):
    now = int(time.time())
    body = payload([reading(100), reading(now + 3 * 86400), reading(now)])
    r = client.post("/ingest", json=body, headers=AUTH)
    assert r.status_code == 200
    assert r.json() == {"accepted": 1, "duplicates": 0, "rejected": 2}


def test_malformed_payload_is_422(client):
    bad = payload([reading(int(time.time()))])
    bad["readings"][0]["channels"].pop()
    assert client.post("/ingest", json=bad, headers=AUTH).status_code == 422
    bad = payload([reading(int(time.time()))])
    bad["readings"][0]["channels"][1]["type"] = "PT500"
    assert client.post("/ingest", json=bad, headers=AUTH).status_code == 422


def test_fault_and_out_of_range_values_stored_as_null(client):
    now = int(time.time())
    temps = [24.0, None, 24.2, 1200.0, 24.4, 24.5, 24.6, 24.7]
    client.post("/ingest", json=payload([reading(now, temps=temps)]), headers=AUTH)
    row = client.get("/api/recent", params={"device_id": "rtd-test", "n": 1}).json()["rows"][0]
    assert row["values"][1] is None
    assert row["values"][3] is None
    assert row["values"][0] == 24.0


def test_latest_and_recent_order(client):
    now = int(time.time())
    client.post("/ingest", json=payload([reading(now - 20, 20.0), reading(now - 10, 21.0), reading(now, 22.0)]),
                headers=AUTH)
    latest = client.get("/api/latest", params={"device_id": "rtd-test"}).json()
    assert latest["latest"]["values"][0] == 22.0
    assert latest["previous"]["values"][0] == 21.0
    assert latest["device"]["types"] == ["PT100"] * 7 + ["PT1000"]
    recent = client.get("/api/recent", params={"device_id": "rtd-test", "n": 2}).json()["rows"]
    assert [r["values"][0] for r in recent] == [22.0, 21.0]


def test_unknown_device_404(client):
    assert client.get("/api/latest", params={"device_id": "ghost"}).status_code == 404


def test_readings_buckets_long_ranges_and_reports_stats(client):
    now = int(time.time())
    start = now - 10_000
    rows = [reading(start + i * 10, base=20.0 + (i % 10)) for i in range(1000)]
    for i in range(0, 1000, 50):
        client.post("/ingest", json=payload(rows[i:i + 50]), headers=AUTH)

    raw = client.get("/api/readings", params={"device_id": "rtd-test", "since": start - 1,
                                              "until": now, "max_points": 5000}).json()
    assert raw["bucket"] == 1
    assert len(raw["ts"]) == 1000
    assert raw["count"] == 1000

    coarse = client.get("/api/readings", params={"device_id": "rtd-test", "since": start - 1,
                                                 "until": now, "max_points": 100}).json()
    assert coarse["bucket"] >= 100
    assert len(coarse["ts"]) <= 101
    assert coarse["stats"]["min"][0] == 20.0
    assert coarse["stats"]["max"][0] == 29.0
    assert len(coarse["ch"]) == 8 and len(coarse["ch"][0]) == len(coarse["ts"])


def test_readings_rejects_bad_ranges(client):
    now = int(time.time())
    client.post("/ingest", json=payload([reading(now)]), headers=AUTH)
    r = client.get("/api/readings", params={"device_id": "rtd-test", "since": now, "until": now - 5})
    assert r.status_code == 400
    r = client.get("/api/readings", params={"device_id": "rtd-test", "since": 0, "until": now})
    assert r.status_code == 400


def test_export_csv(client):
    now = int(time.time())
    client.post("/ingest", json=payload([reading(now, temps=[24.0, None] + [25.0] * 6)]), headers=AUTH)
    r = client.get("/api/export.csv", params={"device_id": "rtd-test", "since": now - 60})
    assert r.status_code == 200
    lines = r.text.strip().split("\n")
    assert lines[0] == "timestamp_utc,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8"
    assert lines[1].startswith(iso(now) + ",24.00,,25.00")


def test_dashboard_password_protects_reads_but_not_ingest(client, monkeypatch):
    monkeypatch.setenv("RTD_DASHBOARD_PASSWORD", "s3cret")
    assert client.get("/api/devices").status_code == 401
    assert client.get("/").status_code == 401
    assert client.get("/healthz").status_code == 200
    assert client.post("/ingest", json=payload([reading(int(time.time()))]), headers=AUTH).status_code == 200
    assert client.get("/api/devices", auth=("admin", "s3cret")).status_code == 200
    assert client.get("/api/devices", auth=("admin", "wrong")).status_code == 401
