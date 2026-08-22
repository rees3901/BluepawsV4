#!/usr/bin/env python3
"""Send authenticated Bluepaws v1.1 TLV telemetry through HTTPS wrappers."""

from __future__ import annotations

import argparse
import base64
import json
import os
import random
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from tlv_packet_codec import (
    DEFAULT_URL,
    CredentialBundle,
    DeviceCredential,
    GatewayCredential,
    PacketFields,
    build_tlv_packet,
    build_transport_wrapper,
    known_tlv,
    load_credential_bundle,
    load_credentials,
    normalize_gateway_guid16,
    post_wrapper,
    validate_bearer_token,
)
DEFAULT_INTERVAL_SECONDS = 10.0
DEFAULT_BASE_LATITUDE = 51.907055
DEFAULT_BASE_LONGITUDE = -2.256660


@dataclass
class DeviceState:
    credential: DeviceCredential
    latitude: float
    longitude: float
    battery_mv: int
    message_sequence: int
    uptime_seconds: int = 0
    last_packet: bytes | None = None


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

    packet = build_tlv_packet(
        PacketFields(
            device_id=state.credential.device_id,
            message_sequence=state.message_sequence,
            timestamp=int(time.time()) if timestamp is None else timestamp,
            status=1,
            power_profile=1,
            flags=0x03,
            tx_reason=0,
            latitude=state.latitude,
            longitude=state.longitude,
            battery_mv=state.battery_mv,
            accuracy_m=8,
            fix_age_s=0,
            satellite_count=9,
        ),
        (
            known_tlv(0x04, 0x0101),
            known_tlv(0x10, state.uptime_seconds),
            known_tlv(0x13, rng.randint(0, 100)),
        ),
        state.credential.hmac_key,
    ).packet
    state.last_packet = packet
    return packet


def build_wrapper(
    packet: bytes,
    transport: str,
    rng: random.Random,
    gateway_guid16: str | None,
) -> dict[str, Any]:
    if transport in ("lora_hub", "lora_gateway"):
        if gateway_guid16 is None:
            raise ValueError("gateway_guid16 is required for lora_gateway")
        return build_transport_wrapper(
            base64.b64encode(packet).decode("ascii"),
            transport,
            gateway_guid16=gateway_guid16,
            gateway_rx_time_unix=int(time.time()),
            link_rssi_dbm=round(rng.uniform(-110, -65), 1),
            link_snr_db=round(rng.uniform(-8, 10), 2),
        )
    return build_transport_wrapper(
        base64.b64encode(packet).decode("ascii"),
        transport,
        link_rssi_dbm=round(rng.uniform(-115, -75), 1),
        link_snr_db=round(rng.uniform(-5, 18), 2),
        cell_rsrp_dbm=round(rng.uniform(-115, -75), 1),
        cell_rsrq_db=round(rng.uniform(-20, -5), 1),
        cell_sinr_db=round(rng.uniform(-5, 18), 2),
    )


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
        default=Path(os.getenv("BLUEPAWS_DEVICE_FILE", "tools/devices.json")),
    )
    parser.add_argument("--device-count", type=int, default=0, help="0 uses every device")
    parser.add_argument("--interval", type=float, default=DEFAULT_INTERVAL_SECONDS)
    parser.add_argument("--iterations", type=int, default=0, help="0 runs until stopped")
    parser.add_argument("--duplicate-rate", type=float, default=0.05)
    parser.add_argument(
        "--transport", choices=("cellular_direct", "lora_hub", "lora_gateway"), default="cellular_direct"
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
    return args


def resolve_gateway_auth(
    bundle: CredentialBundle,
    gateway_guid16: str | None,
    gateway_token: str | None,
) -> tuple[str, str]:
    normalized_guid = (
        normalize_gateway_guid16(gateway_guid16) if gateway_guid16 is not None else None
    )
    selected = None
    if normalized_guid is not None:
        selected = next(
            (
                gateway
                for gateway in bundle.gateways
                if gateway.gateway_guid16 == normalized_guid
            ),
            None,
        )
    elif len(bundle.gateways) == 1:
        selected = bundle.gateways[0]
        normalized_guid = selected.gateway_guid16
    elif len(bundle.gateways) > 1:
        raise ValueError(
            "multiple gateways are available; select one with --gateway-guid16"
        )

    resolved_token = gateway_token or (selected.token if selected is not None else None)
    if normalized_guid is None:
        raise ValueError(
            "LoRa requires a gateway in the credentials bundle or --gateway-guid16"
        )
    if resolved_token is None:
        raise ValueError(
            f"gateway {normalized_guid} requires a bundle credential or --gateway-token"
        )
    return normalized_guid, validate_bearer_token(resolved_token, "gateway bearer token")


def main() -> int:
    args = parse_args()
    rng = random.Random(args.seed)
    try:
        bundle = load_credential_bundle(args.devices_file)
        credentials = list(bundle.devices)
        if args.transport in ("lora_hub", "lora_gateway"):
            args.gateway_guid16, args.gateway_token = resolve_gateway_auth(
                bundle, args.gateway_guid16, args.gateway_token
            )
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
                    if args.transport in ("lora_hub", "lora_gateway")
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
