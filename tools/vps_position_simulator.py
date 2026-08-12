#!/usr/bin/env python3
"""Send synthetic multi-device position telemetry to Bluepaws ingestion."""

from __future__ import annotations

import argparse
import json
import os
import random
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DEFAULT_URL = (
    "https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/ingest-position"
)
DEFAULT_INTERVAL_SECONDS = 10.0
DEFAULT_BASE_LATITUDE = 51.907055
DEFAULT_BASE_LONGITUDE = -2.256660


@dataclass(frozen=True)
class DeviceCredential:
    device_id: int
    token: str


@dataclass
class DeviceState:
    credential: DeviceCredential
    latitude: float
    longitude: float
    battery: int
    message_id: int
    last_payload: dict[str, Any] | None = None


def load_credentials(path: Path) -> list[DeviceCredential]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, list) or not raw:
        raise ValueError("credentials file must contain a non-empty JSON array")

    credentials: list[DeviceCredential] = []
    seen: set[int] = set()
    for item in raw:
        if not isinstance(item, dict):
            raise ValueError("each credential must be a JSON object")
        device_id = item.get("device_id")
        token = item.get("token")
        if not isinstance(device_id, int) or isinstance(device_id, bool):
            raise ValueError("device_id must be an integer")
        if not 1 <= device_id <= 65_534:
            raise ValueError(f"device_id {device_id} is outside 1..65534")
        if device_id in seen:
            raise ValueError(f"device_id {device_id} is duplicated")
        if not isinstance(token, str) or len(token) < 32:
            raise ValueError(f"device_id {device_id} has an invalid token")
        seen.add(device_id)
        credentials.append(DeviceCredential(device_id, token))
    return credentials


def build_payload(state: DeviceState, rng: random.Random, step: float) -> dict[str, Any]:
    state.latitude = max(-90.0, min(90.0, state.latitude + rng.uniform(-step, step)))
    state.longitude = max(-180.0, min(180.0, state.longitude + rng.uniform(-step, step)))
    state.battery = max(0, state.battery - (1 if rng.random() < 0.01 else 0))
    state.message_id += 1
    payload: dict[str, Any] = {
        "schema_version": 1,
        "device_id": state.credential.device_id,
        "message_id": state.message_id,
        "latitude": round(state.latitude, 7),
        "longitude": round(state.longitude, 7),
        "battery": state.battery,
        "recorded_at": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
            "+00:00", "Z"
        ),
    }
    state.last_payload = payload
    return payload


def post_position(
    url: str,
    credential: DeviceCredential,
    payload: dict[str, Any],
    timeout: float,
) -> tuple[int, dict[str, Any]]:
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {credential.token}",
            "Content-Type": "application/json",
            "User-Agent": "bluepaws-vps-simulator/1",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        raw = error.read().decode("utf-8", errors="replace")
        try:
            response_body = json.loads(raw)
        except json.JSONDecodeError:
            response_body = {"error": raw or error.reason}
        return error.code, response_body


def initialise_states(
    credentials: list[DeviceCredential],
    base_latitude: float,
    base_longitude: float,
    rng: random.Random,
) -> list[DeviceState]:
    start_message_id = int(time.time())
    return [
        DeviceState(
            credential=credential,
            latitude=base_latitude + rng.uniform(-0.01, 0.01),
            longitude=base_longitude + rng.uniform(-0.01, 0.01),
            battery=rng.randint(65, 100),
            message_id=start_message_id,
        )
        for credential in credentials
    ]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Simulate several Bluepaws collars posting through the public internet."
    )
    parser.add_argument(
        "--url", default=os.getenv("BLUEPAWS_INGEST_URL", DEFAULT_URL)
    )
    parser.add_argument(
        "--devices-file",
        type=Path,
        default=Path(os.getenv("BLUEPAWS_DEVICE_FILE", "tools/vps_devices.json")),
    )
    parser.add_argument("--device-count", type=int, default=0, help="0 uses every device")
    parser.add_argument(
        "--interval",
        type=float,
        default=DEFAULT_INTERVAL_SECONDS,
        help="seconds between fleet update cycles (default: 10)",
    )
    parser.add_argument(
        "--iterations", type=int, default=0, help="cycles to send; 0 runs until stopped"
    )
    parser.add_argument("--duplicate-rate", type=float, default=0.05)
    parser.add_argument(
        "--base-latitude",
        type=float,
        default=DEFAULT_BASE_LATITUDE,
        help="simulation centre latitude (default: Sandhurst, Gloucestershire)",
    )
    parser.add_argument(
        "--base-longitude",
        type=float,
        default=DEFAULT_BASE_LONGITUDE,
        help="simulation centre longitude (default: Sandhurst, Gloucestershire)",
    )
    parser.add_argument("--step-degrees", type=float, default=0.0002)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--seed", type=int)
    args = parser.parse_args()
    if args.device_count < 0 or args.iterations < 0:
        parser.error("device-count and iterations cannot be negative")
    if args.interval < 0 or args.timeout <= 0 or args.step_degrees < 0:
        parser.error("interval/step cannot be negative and timeout must be positive")
    if not 0 <= args.duplicate_rate <= 1:
        parser.error("duplicate-rate must be from 0 to 1")
    return args


def main() -> int:
    args = parse_args()
    rng = random.Random(args.seed)
    try:
        credentials = load_credentials(args.devices_file)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Unable to load credentials: {error}", file=sys.stderr)
        return 2

    if args.device_count:
        credentials = credentials[: args.device_count]
    states = initialise_states(
        credentials, args.base_latitude, args.base_longitude, rng
    )
    cycle = 0
    print(f"Sending {len(states)} simulated devices to {args.url}")

    try:
        while args.iterations == 0 or cycle < args.iterations:
            cycle += 1
            for state in states:
                duplicate = (
                    state.last_payload is not None and rng.random() < args.duplicate_rate
                )
                payload = (
                    state.last_payload
                    if duplicate
                    else build_payload(state, rng, args.step_degrees)
                )
                assert payload is not None
                try:
                    status, response = post_position(
                        args.url, state.credential, payload, args.timeout
                    )
                    print(
                        json.dumps(
                            {
                                "status": status,
                                "device_id": state.credential.device_id,
                                "message_id": payload["message_id"],
                                "retry": duplicate,
                                "response": response,
                            },
                            separators=(",", ":"),
                        )
                    )
                except (OSError, TimeoutError) as error:
                    print(
                        json.dumps(
                            {
                                "status": 0,
                                "device_id": state.credential.device_id,
                                "message_id": payload["message_id"],
                                "error": str(error),
                            },
                            separators=(",", ":"),
                        ),
                        file=sys.stderr,
                    )
            if args.iterations == 0 or cycle < args.iterations:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print("Stopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
