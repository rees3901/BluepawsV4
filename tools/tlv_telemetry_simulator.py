#!/usr/bin/env python3
"""Send authenticated Bluepaws v1.1 TLV telemetry through HTTPS wrappers."""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import hmac
import json
import os
import random
import struct
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_URL = (
    "https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/ingest-position"
)
DEFAULT_INTERVAL_SECONDS = 10.0
DEFAULT_BASE_LATITUDE = 51.907055
DEFAULT_BASE_LONGITUDE = -2.256660
HEADER_SIZE = 32
AUTH_TAG_SIZE = 8
MAX_TLV_SIZE = 24


@dataclass(frozen=True)
class DeviceCredential:
    device_id: int
    token: str
    hmac_key: bytes


@dataclass
class DeviceState:
    credential: DeviceCredential
    latitude: float
    longitude: float
    battery_mv: int
    message_sequence: int
    uptime_seconds: int = 0
    last_packet: bytes | None = None


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
        key_text = item.get("hmac_key_b64")
        if not isinstance(device_id, int) or isinstance(device_id, bool):
            raise ValueError("device_id must be an integer")
        if not 1 <= device_id <= 65_535:
            raise ValueError(f"device_id {device_id} is outside 1..65535")
        if device_id in seen:
            raise ValueError(f"device_id {device_id} is duplicated")
        if not isinstance(token, str) or len(token) < 32:
            raise ValueError(f"device_id {device_id} has an invalid bearer token")
        if not isinstance(key_text, str):
            raise ValueError(f"device_id {device_id} is missing hmac_key_b64")
        try:
            hmac_key = base64.b64decode(key_text, validate=True)
        except (ValueError, binascii.Error) as error:
            raise ValueError(
                f"device_id {device_id} has invalid Base64 HMAC key"
            ) from error
        if len(hmac_key) != 32:
            raise ValueError(f"device_id {device_id} HMAC key must decode to 32 bytes")
        seen.add(device_id)
        credentials.append(DeviceCredential(device_id, token, hmac_key))
    return credentials


def encode_tlv(tlv_type: int, value: bytes) -> bytes:
    if not 0 <= tlv_type <= 255 or not 1 <= len(value) <= 255:
        raise ValueError("TLV type and length must fit in one byte")
    return bytes((tlv_type, len(value))) + value


def build_packet(
    state: DeviceState,
    rng: random.Random,
    step_degrees: float,
    *,
    timestamp: int | None = None,
    elapsed_seconds: float = DEFAULT_INTERVAL_SECONDS,
) -> bytes:
    state.latitude = max(
        -90.0, min(90.0, state.latitude + rng.uniform(-step_degrees, step_degrees))
    )
    state.longitude = max(
        -180.0,
        min(180.0, state.longitude + rng.uniform(-step_degrees, step_degrees)),
    )
    if rng.random() < 0.01:
        state.battery_mv = max(3_000, state.battery_mv - 1)
    state.message_sequence = (state.message_sequence + 1) & 0xFFFF
    state.uptime_seconds += max(1, round(elapsed_seconds))

    tlvs = b"".join(
        (
            encode_tlv(0x04, struct.pack("<H", 0x0101)),
            encode_tlv(0x10, struct.pack("<I", state.uptime_seconds)),
            encode_tlv(0x13, bytes((rng.randint(0, 100),))),
        )
    )
    if len(tlvs) > MAX_TLV_SIZE:
        raise AssertionError("simulator TLVs exceed the protocol limit")

    header = bytearray(HEADER_SIZE)
    header[0] = 1
    struct.pack_into("<H", header, 1, state.credential.device_id)
    struct.pack_into("<H", header, 3, state.message_sequence)
    struct.pack_into("<I", header, 5, int(time.time()) if timestamp is None else timestamp)
    header[9] = 0x11  # NORMAL power profile and OUT status.
    header[10] = 0x03  # GNSS_VALID and FIX_3D.
    header[11] = 0  # TELEMETRY.
    struct.pack_into("<i", header, 12, round(state.latitude * 10_000_000))
    struct.pack_into("<i", header, 16, round(state.longitude * 10_000_000))
    struct.pack_into("<H", header, 20, state.battery_mv)
    struct.pack_into("<H", header, 22, 8)
    struct.pack_into("<H", header, 24, 0)
    header[26] = 9
    header[31] = len(tlvs)

    body = bytes(header) + tlvs
    tag = hmac.new(state.credential.hmac_key, body, hashlib.sha256).digest()[:8]
    packet = body + tag
    state.last_packet = packet
    return packet


def build_wrapper(
    packet: bytes,
    transport: str,
    rng: random.Random,
    gateway_guid16: str | None,
) -> dict[str, Any]:
    payload_b64 = base64.b64encode(packet).decode("ascii")
    if transport == "lora_hub":
        if gateway_guid16 is None:
            raise ValueError("gateway_guid16 is required for lora_hub")
        return {
            "ingest_path": "lora_hub",
            "link_type": "lora",
            "gateway_guid16": gateway_guid16,
            "gateway_rx_time_unix": int(time.time()),
            "link_rssi_dbm": round(rng.uniform(-110, -65), 1),
            "link_snr_db": round(rng.uniform(-8, 10), 2),
            "payload_b64": payload_b64,
        }
    return {
        "ingest_path": "cellular_direct",
        "link_type": "lte",
        "link_rssi_dbm": round(rng.uniform(-115, -75), 1),
        "link_snr_db": round(rng.uniform(-5, 18), 2),
        "cell_rsrp_dbm": round(rng.uniform(-115, -75), 1),
        "cell_rsrq_db": round(rng.uniform(-20, -5), 1),
        "cell_sinr_db": round(rng.uniform(-5, 18), 2),
        "payload_b64": payload_b64,
    }


def post_wrapper(
    url: str, bearer_token: str, wrapper: dict[str, Any], timeout: float
) -> tuple[int, dict[str, Any]]:
    request = urllib.request.Request(
        url,
        data=json.dumps(wrapper, separators=(",", ":")).encode("utf-8"),
        method="POST",
        headers={
            "Authorization": f"Bearer {bearer_token}",
            "Content-Type": "application/json",
            "User-Agent": "bluepaws-tlv-vps-simulator/1",
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
    start_sequence = int(time.time()) & 0xFFFF
    return [
        DeviceState(
            credential=credential,
            latitude=base_latitude + rng.uniform(-0.01, 0.01),
            longitude=base_longitude + rng.uniform(-0.01, 0.01),
            battery_mv=rng.randint(3_650, 4_150),
            message_sequence=(start_sequence + index) & 0xFFFF,
        )
        for index, credential in enumerate(credentials)
    ]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Simulate authenticated Bluepaws v1.1 TLV collars."
    )
    parser.add_argument("--url", default=os.getenv("BLUEPAWS_INGEST_URL", DEFAULT_URL))
    parser.add_argument(
        "--devices-file",
        type=Path,
        default=Path(os.getenv("BLUEPAWS_DEVICE_FILE", "tools/vps_devices.json")),
    )
    parser.add_argument("--device-count", type=int, default=0, help="0 uses every device")
    parser.add_argument("--interval", type=float, default=DEFAULT_INTERVAL_SECONDS)
    parser.add_argument("--iterations", type=int, default=0, help="0 runs until stopped")
    parser.add_argument("--duplicate-rate", type=float, default=0.05)
    parser.add_argument(
        "--transport", choices=("cellular_direct", "lora_hub"), default="cellular_direct"
    )
    parser.add_argument(
        "--gateway-guid16", default=os.getenv("BLUEPAWS_GATEWAY_GUID16")
    )
    parser.add_argument("--gateway-token", default=os.getenv("BLUEPAWS_GATEWAY_TOKEN"))
    parser.add_argument("--base-latitude", type=float, default=DEFAULT_BASE_LATITUDE)
    parser.add_argument("--base-longitude", type=float, default=DEFAULT_BASE_LONGITUDE)
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
    if args.transport == "lora_hub":
        if not args.gateway_token:
            parser.error("--gateway-token or BLUEPAWS_GATEWAY_TOKEN is required for LoRa")
        if not args.gateway_guid16:
            parser.error("--gateway-guid16 or BLUEPAWS_GATEWAY_GUID16 is required for LoRa")
        try:
            gateway_id = int(args.gateway_guid16, 16)
        except ValueError:
            parser.error("gateway-guid16 must be exactly four hexadecimal characters")
        if len(args.gateway_guid16) != 4 or not 1 <= gateway_id <= 0xFFFF:
            parser.error("gateway-guid16 must be 0001..FFFF")
        args.gateway_guid16 = args.gateway_guid16.upper()
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
    print(
        f"Sending {len(states)} simulated TLV devices via {args.transport} to {args.url}"
    )

    cycle = 0
    try:
        while args.iterations == 0 or cycle < args.iterations:
            cycle += 1
            for state in states:
                duplicate = state.last_packet is not None and rng.random() < args.duplicate_rate
                packet = (
                    state.last_packet
                    if duplicate
                    else build_packet(
                        state,
                        rng,
                        args.step_degrees,
                        elapsed_seconds=args.interval,
                    )
                )
                assert packet is not None
                wrapper = build_wrapper(packet, args.transport, rng, args.gateway_guid16)
                bearer = (
                    args.gateway_token
                    if args.transport == "lora_hub"
                    else state.credential.token
                )
                try:
                    status, response = post_wrapper(args.url, bearer, wrapper, args.timeout)
                    print(
                        json.dumps(
                            {
                                "status": status,
                                "device_id": state.credential.device_id,
                                "message_sequence": state.message_sequence,
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
                                "message_sequence": state.message_sequence,
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
