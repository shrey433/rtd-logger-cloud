"""Validation and storage shared by the HTTP /ingest endpoint and the MQTT subscriber."""
import re
import time
from datetime import datetime, timezone
from typing import List, Literal, Optional

from pydantic import BaseModel, Field, field_validator

from . import db
from .live import hub

MIN_VALID_TS = 1704067200  # 2024-01-01: anything older means the device clock was never set
MAX_FUTURE_S = 86400
PT_MIN_C, PT_MAX_C = -200.0, 850.0
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


class Channel(BaseModel):
    ch: int = Field(ge=1, le=db.CHANNELS)
    type: Literal["PT100", "PT1000"]
    temp_c: Optional[float] = None  # null when the MAX31865 reports a fault


class Reading(BaseModel):
    ts: datetime
    channels: List[Channel] = Field(min_length=db.CHANNELS, max_length=db.CHANNELS)

    @field_validator("channels")
    @classmethod
    def channels_unique(cls, v: List[Channel]) -> List[Channel]:
        if sorted(c.ch for c in v) != list(range(1, db.CHANNELS + 1)):
            raise ValueError("channels must contain ch 1..8 exactly once each")
        return v


class Payload(BaseModel):
    """HTTP body: several readings from one device."""
    device_id: str = Field(pattern=DEVICE_ID_RE.pattern)
    fw_version: str = Field(max_length=32)
    backlog: Optional[int] = Field(default=None, ge=0, le=10_000_000)  # rows still queued on the device
    readings: List[Reading] = Field(min_length=1, max_length=50)


class Telemetry(Reading):
    """MQTT message: one reading; the device id comes from the topic (rtd/<device_id>/telemetry)."""
    fw_version: str = Field(max_length=32)
    backlog: Optional[int] = Field(default=None, ge=0, le=10_000_000)


def store_readings(device_id: str, fw_version: str, backlog: Optional[int],
                   readings: List[Reading]) -> dict:
    now = int(time.time())
    rows = []
    rejected = 0
    types: List[str] = []
    for reading in readings:
        ts = reading.ts if reading.ts.tzinfo else reading.ts.replace(tzinfo=timezone.utc)
        epoch = int(ts.timestamp())
        if epoch < MIN_VALID_TS or epoch > now + MAX_FUTURE_S:
            rejected += 1
            continue
        by_ch = {c.ch: c for c in reading.channels}
        values = []
        for ch in range(1, db.CHANNELS + 1):
            v = by_ch[ch].temp_c
            values.append(v if v is not None and PT_MIN_C <= v <= PT_MAX_C else None)
        rows.append((epoch, values))
        types = [by_ch[ch].type for ch in range(1, db.CHANNELS + 1)]

    new_rows = db.insert_readings(device_id, fw_version, types, rows, backlog) if rows else []
    for epoch, values in new_rows:
        hub.publish({"device_id": device_id, "ts": epoch, "values": values, "backlog": backlog,
                     "fw_version": fw_version, "server_time": int(time.time())})
    return {"accepted": len(new_rows), "duplicates": len(rows) - len(new_rows), "rejected": rejected}
