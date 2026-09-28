#!/usr/bin/env python3
"""Hold a native-API connection the way Home Assistant does: entity list, state
subscription and log subscription, for as long as asked.

Nothing in the api component creates a task, so this work lands on the ESPHome loop task —
which is the task the HEAPCSV trace reports as loop_stack_left.
"""
from __future__ import annotations

import argparse
import asyncio
import sys

from aioesphomeapi import APIClient


async def run(host: str, port: int, seconds: float) -> None:
    client = APIClient(host, port, None)
    await client.connect(login=True)
    info = await client.device_info()
    entities, services = await client.list_entities_services()
    n = sum(len(v) if isinstance(v, list) else 1 for v in entities)
    print(f"connected to {info.name}: {n} entities, {len(services)} services", flush=True)

    states = 0
    logs = 0

    def on_state(state) -> None:  # noqa: ANN001
        nonlocal states
        states += 1

    def on_log(msg) -> None:  # noqa: ANN001
        nonlocal logs
        logs += 1

    client.subscribe_states(on_state)
    client.subscribe_logs(on_log)
    await asyncio.sleep(seconds)
    print(f"held {seconds:.0f}s: {states} state messages, {logs} log messages", flush=True)
    await client.disconnect()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6064)
    ap.add_argument("--seconds", type=float, default=600)
    args = ap.parse_args()
    asyncio.run(run(args.host, args.port, args.seconds))


if __name__ == "__main__":
    sys.exit(main())
