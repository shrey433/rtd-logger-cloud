#!/usr/bin/env python3
"""A throwaway local MQTT broker (no TLS, no auth) for developing without AWS.

  python tools/dev_broker.py --port 1883
  MQTT_HOST=127.0.0.1 MQTT_PORT=1883 MQTT_TLS=0 python -m uvicorn app.main:app
"""
import argparse
import asyncio
import logging

from amqtt.broker import Broker


async def run(host: str, port: int) -> None:
    broker = Broker({
        "listeners": {"default": {"type": "tcp", "bind": f"{host}:{port}"}},
        "sys_interval": 0,
        "auth": {"allow-anonymous": True, "plugins": ["auth_anonymous"]},
        "topic-check": {"enabled": True, "plugins": ["topic_taboo"]},
    })
    await broker.start()
    print(f"dev broker listening on {host}:{port}", flush=True)
    try:
        await asyncio.Event().wait()
    finally:
        await broker.shutdown()


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=1883)
    args = p.parse_args()
    logging.disable(logging.CRITICAL)
    try:
        asyncio.run(run(args.host, args.port))
    except KeyboardInterrupt:
        pass
