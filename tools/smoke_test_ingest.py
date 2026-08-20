#!/usr/bin/env python3
"""Exercise the public ingestion contract without exposing device secrets."""

from __future__ import annotations

import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from vps_position_simulator import DEFAULT_URL, load_credentials


def request(
    method: str,
    payload: dict[str, Any] | None,
    token: str | None,
) -> tuple[int, dict[str, Any]]:
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    outgoing = urllib.request.Request(
        DEFAULT_URL, data=body, method=method, headers=headers
    )
    try:
        with urllib.request.urlopen(outgoing, timeout=15) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read().decode("utf-8"))


def check(name: str, actual: int, expected: int, body: dict[str, Any]) -> None:
    if actual != expected:
        raise AssertionError(f"{name}: expected HTTP {expected}, got {actual}: {body}")
    print(f"PASS {name}: HTTP {actual}")


def main() -> int:
    credentials = load_credentials(Path("tools/devices.json"))
    credential = credentials[0]
    timestamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )
    payload = {
        "schema_version": 1,
        "device_id": credential.device_id,
        "message_id": int(time.time()),
        "latitude": 51.5074,
        "longitude": -0.1278,
        "battery": 87,
        "recorded_at": timestamp,
    }

    status, body = request("GET", None, None)
    check("method restriction", status, 405, body)

    status, body = request("POST", payload, None)
    check("missing credential", status, 401, body)

    invalid = dict(payload, latitude=91)
    status, body = request("POST", invalid, credential.token)
    check("payload validation", status, 400, body)

    status, body = request("POST", payload, credential.token)
    check("new position", status, 201, body)
    if body.get("duplicate") is not False:
        raise AssertionError(f"new position was not marked new: {body}")

    status, body = request("POST", payload, credential.token)
    check("idempotent retry", status, 200, body)
    if body.get("duplicate") is not True:
        raise AssertionError(f"retry was not marked duplicate: {body}")

    conflicting = dict(payload, latitude=51.5075)
    status, body = request("POST", conflicting, credential.token)
    check("message conflict", status, 409, body)

    unknown = dict(payload, device_id=65_534, message_id=payload["message_id"] + 1)
    status, body = request("POST", unknown, credential.token)
    check("unprovisioned device", status, 401, body)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
