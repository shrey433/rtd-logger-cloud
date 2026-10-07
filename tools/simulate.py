#!/usr/bin/env python3
"""Pretend to be an RTD logger: POST the same payload the firmware sends.

  python tools/simulate.py --token dev-token --backfill-hours 24 --outage 3:10-3:40 --live
"""
import argparse
import json
import math
import random
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

INTERVAL_S = 10
BATCH = 50  # server accepts up to 50 readings per request


def iso(epoch: int) -> str:
    return datetime.fromtimestamp(epoch, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def temps_at(epoch: int, offsets, rng):
    swing = 3.0 * math.sin(epoch / 5400.0)  # ~9 h period process swing
    out = []
    for ch in range(8):
        v = 24.0 + swing + offsets[ch] + rng.gauss(0, 0.03)
        if ch == 3:  # one hot spot that steps up for 20 min each hour
            v += 6.0 if (epoch // 60) % 60 < 20 else 0.0
        out.append(round(v, 2))
    return out


def reading(epoch: int, offsets, rng, fault_ch=None):
    temps = temps_at(epoch, offsets, rng)
    return {
        "ts": iso(epoch),
        "channels": [
            {"ch": i + 1, "type": "PT1000" if i == 7 else "PT100",
             "temp_c": None if i == fault_ch else temps[i]}
            for i in range(8)
        ],
    }


def post(url: str, token: str, device: str, readings, backlog=None) -> dict:
    payload = {"device_id": device, "fw_version": "sim-0.1.0", "readings": readings}
    if backlog is not None:
        payload["backlog"] = backlog
    body = json.dumps(payload).encode()
    req = urllib.request.Request(
        url.rstrip("/") + "/ingest", data=body, method="POST",
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read())


def parse_outage(spec: str):
    """'3:10-3:40' -> a window from 3 h 10 min ago to 3 h 40 min ago, as minutes-ago (low, high)."""
    a, b = spec.split("-")

    def minutes(x):
        h, m = x.split(":")
        return int(h) * 60 + int(m)

    return sorted((minutes(a), minutes(b)))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", default="http://127.0.0.1:8000")
    p.add_argument("--token", default="dev-token")
    p.add_argument("--device", default="rtd-logger-01")
    p.add_argument("--backfill-hours", type=float, default=6.0, help="history to generate first")
    p.add_argument("--outage", action="append", default=[], metavar="H:MM-H:MM",
                   help="leave a gap in the history, expressed as time ago (repeatable)")
    p.add_argument("--live", action="store_true", help="keep posting one reading every 10 s")
    p.add_argument("--demo-backlog", type=int, default=0, metavar="N",
                   help="in --live mode, report N rows queued on the device and count down one per post")
    p.add_argument("--mqtt-host", help="in --live mode, publish over MQTT (a local dev broker) instead of HTTP")
    p.add_argument("--mqtt-port", type=int, default=1883)
    p.add_argument("--seed", type=int, default=7)
    args = p.parse_args()

    rng = random.Random(args.seed)
    offsets = [rng.uniform(-0.6, 0.6) for _ in range(8)]
    gaps = [parse_outage(s) for s in args.outage]

    now = int(time.time()) // INTERVAL_S * INTERVAL_S
    start = now - int(args.backfill_hours * 3600)
    batch, sent, skipped = [], 0, 0
    for t in range(start, now + 1, INTERVAL_S):
        mins_ago = (now - t) / 60
        if any(lo <= mins_ago <= hi for lo, hi in gaps):
            skipped += 1
            continue
        fault = 5 if (t // INTERVAL_S) % 997 == 0 else None  # an occasional sensor fault on ch6
        batch.append(reading(t, offsets, rng, fault))
        if len(batch) == BATCH:
            sent += post(args.url, args.token, args.device, batch)["accepted"]
            batch = []
    if batch:
        sent += post(args.url, args.token, args.device, batch)["accepted"]
    print(f"backfill: {sent} readings stored, {skipped} skipped for outages")

    mqtt_client = None
    if args.live and args.mqtt_host:
        import paho.mqtt.client as mqtt
        mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=args.device)
        mqtt_client.connect(args.mqtt_host, args.mqtt_port)
        mqtt_client.loop_start()
        print(f"live: publishing to rtd/{args.device}/telemetry on {args.mqtt_host}:{args.mqtt_port}")

    queued = args.demo_backlog
    while args.live:
        t = int(time.time())
        row = reading(t, offsets, rng)
        try:
            if mqtt_client:
                message = {"fw_version": "sim-0.1.0", "backlog": queued, **row}
                mqtt_client.publish(f"rtd/{args.device}/telemetry", json.dumps(message), qos=1)
                print(f"{iso(t)} -> published")
            else:
                print(f"{iso(t)} -> {post(args.url, args.token, args.device, [row], backlog=queued)}")
            queued = max(0, queued - 1)
        except (urllib.error.URLError, TimeoutError) as e:
            print(f"{iso(t)} upload failed: {e}")
        time.sleep(INTERVAL_S)


if __name__ == "__main__":
    main()
