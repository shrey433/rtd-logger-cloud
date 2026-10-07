import json
import os
import sqlite3
import time
from contextlib import contextmanager
from typing import Iterator, List, Optional

CHANNELS = 8
COLS = [f"c{i}" for i in range(1, CHANNELS + 1)]

SCHEMA = f"""
CREATE TABLE IF NOT EXISTS devices (
    device_id  TEXT PRIMARY KEY,
    fw_version TEXT,
    types      TEXT,
    first_seen INTEGER NOT NULL,
    last_seen  INTEGER NOT NULL,
    backlog    INTEGER
);
CREATE TABLE IF NOT EXISTS readings (
    device_id   TEXT    NOT NULL,
    ts          INTEGER NOT NULL,
    {", ".join(f"{c} REAL" for c in COLS)},
    received_at INTEGER NOT NULL,
    PRIMARY KEY (device_id, ts)
) WITHOUT ROWID;
"""


def db_path() -> str:
    return os.environ.get("RTD_DB", os.path.join("data", "rtd.db"))


def init_db() -> None:
    path = db_path()
    folder = os.path.dirname(os.path.abspath(path))
    os.makedirs(folder, exist_ok=True)
    with connect() as conn:
        conn.executescript(SCHEMA)
        columns = [row["name"] for row in conn.execute("PRAGMA table_info(devices)")]
        if "backlog" not in columns:  # database created before the device reported its queue
            conn.execute("ALTER TABLE devices ADD COLUMN backlog INTEGER")


@contextmanager
def connect() -> Iterator[sqlite3.Connection]:
    conn = sqlite3.connect(db_path(), timeout=15)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def insert_readings(device_id: str, fw_version: str, types: List[str],
                    rows: List[tuple], backlog: Optional[int] = None) -> int:
    """rows: (ts, [8 floats or None]). Returns how many were new."""
    now = int(time.time())
    inserted = 0
    placeholders = ",".join("?" * (CHANNELS + 3))
    sql = (f"INSERT OR IGNORE INTO readings (device_id, ts, {', '.join(COLS)}, received_at) "
           f"VALUES ({placeholders})")
    with connect() as conn:
        for ts, values in rows:
            cur = conn.execute(sql, (device_id, ts, *values, now))
            inserted += cur.rowcount
        conn.execute(
            """INSERT INTO devices (device_id, fw_version, types, first_seen, last_seen, backlog)
               VALUES (?, ?, ?, ?, ?, ?)
               ON CONFLICT(device_id) DO UPDATE SET
                 fw_version = excluded.fw_version,
                 types      = excluded.types,
                 last_seen  = excluded.last_seen,
                 backlog    = excluded.backlog""",
            (device_id, fw_version, json.dumps(types), now, now, backlog),
        )
    return inserted


def list_devices() -> List[dict]:
    with connect() as conn:
        rows = conn.execute(
            "SELECT device_id, fw_version, types, first_seen, last_seen, backlog FROM devices "
            "ORDER BY device_id").fetchall()
    return [
        {
            "device_id": r["device_id"],
            "fw_version": r["fw_version"],
            "types": json.loads(r["types"]) if r["types"] else [],
            "first_seen": r["first_seen"],
            "last_seen": r["last_seen"],
            "backlog": r["backlog"],
        }
        for r in rows
    ]


def get_device(device_id: str) -> Optional[dict]:
    for d in list_devices():
        if d["device_id"] == device_id:
            return d
    return None


def recent_rows(device_id: str, n: int) -> List[dict]:
    with connect() as conn:
        rows = conn.execute(
            f"SELECT ts, {', '.join(COLS)} FROM readings WHERE device_id=? "
            "ORDER BY ts DESC LIMIT ?", (device_id, n)).fetchall()
    return [{"ts": r["ts"], "values": [r[c] for c in COLS]} for r in rows]


def bucketed(device_id: str, since: int, until: int, bucket: int) -> dict:
    avg_cols = ", ".join(f"AVG({c}) AS {c}" for c in COLS)
    with connect() as conn:
        rows = conn.execute(
            f"SELECT (ts / ?) * ? AS t, {avg_cols} FROM readings "
            "WHERE device_id=? AND ts BETWEEN ? AND ? GROUP BY t ORDER BY t",
            (bucket, bucket, device_id, since, until)).fetchall()
        agg = ", ".join(f"MIN({c}), MAX({c}), AVG({c})" for c in COLS)
        stats_row = conn.execute(
            f"SELECT COUNT(*), {agg} FROM readings "
            "WHERE device_id=? AND ts BETWEEN ? AND ?",
            (device_id, since, until)).fetchone()
    count = stats_row[0]
    mins, maxs, avgs = [], [], []
    for i in range(CHANNELS):
        mins.append(stats_row[1 + i * 3])
        maxs.append(stats_row[2 + i * 3])
        avgs.append(stats_row[3 + i * 3])
    return {
        "ts": [r["t"] for r in rows],
        "ch": [[r[c] for r in rows] for c in COLS],
        "count": count,
        "stats": {"min": mins, "max": maxs, "avg": avgs},
    }


def iter_export(device_id: str, since: int, until: int) -> Iterator[sqlite3.Row]:
    with connect() as conn:
        cur = conn.execute(
            f"SELECT ts, {', '.join(COLS)} FROM readings "
            "WHERE device_id=? AND ts BETWEEN ? AND ? ORDER BY ts",
            (device_id, since, until))
        while True:
            chunk = cur.fetchmany(2000)
            if not chunk:
                break
            yield from chunk
