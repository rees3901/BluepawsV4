#!/usr/bin/env python3
"""Reusable Bluepaws v1.1 TLV packet and HTTPS-wrapper primitives.

This module deliberately contains no GUI code.  Both the command-line fleet
simulator and the desktop test console import it so they cannot silently drift
onto different packet contracts.
"""

from __future__ import annotations

import base64
import binascii
import hashlib
import hmac
import json
import re
import struct
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


DEFAULT_URL = (
    "https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/ingest-position"
)
HEADER_SIZE = 32
AUTH_TAG_SIZE = 8
MAX_TLV_SIZE = 24
MIN_PACKET_SIZE = HEADER_SIZE + AUTH_TAG_SIZE
MAX_PACKET_SIZE = HEADER_SIZE + MAX_TLV_SIZE + AUTH_TAG_SIZE

STATUS_CODES = {
    "HOME": 0,
    "OUT": 1,
    "LOST": 2,
    "ERROR": 3,
}
POWER_PROFILE_CODES = {
    "POWER_SAVE": 0,
    "NORMAL": 1,
    "ACTIVE": 2,
    "LOST_ALERT": 3,
}
TX_REASON_CODES = {
    "TELEMETRY": 0,
    "ACK": 1,
    "PING": 2,
    "INTERRUPT": 3,
    "BOOT": 4,
    "ALERT": 5,
    "CONFIG": 6,
    "WAKE_CHECKIN": 7,
}
FLAG_MASKS = {
    "GNSS_VALID": 0x01,
    "FIX_3D": 0x02,
    "LOW_BATTERY": 0x04,
    "HOME_BEACON_SEEN": 0x08,
    "GEOFENCE_BREACHED": 0x10,
    "CHARGING": 0x20,
    "STALE_FIX": 0x40,
    "ERROR_PRESENT": 0x80,
}
KNOWN_TLV_LENGTHS = {
    0x04: ("fw_ver", 2),
    0x06: ("reset_reason", 1),
    0x10: ("uptime_s", 4),
    0x13: ("activity_score", 1),
    0x20: ("acked_msg_seq_id", 2),
}


@dataclass(frozen=True)
class DeviceCredential:
    device_id: int
    token: str
    hmac_key: bytes


@dataclass(frozen=True)
class GatewayCredential:
    gateway_guid16: str
    token: str
    display_name: str | None = None


@dataclass(frozen=True)
class CredentialBundle:
    devices: tuple[DeviceCredential, ...]
    gateways: tuple[GatewayCredential, ...]


@dataclass(frozen=True)
class PacketFields:
    device_id: int
    message_sequence: int
    timestamp: int
    status: int
    power_profile: int
    flags: int
    tx_reason: int
    latitude: float
    longitude: float
    battery_mv: int
    accuracy_m: int
    fix_age_s: int
    satellite_count: int
    protocol_version: int = 1


@dataclass(frozen=True)
class TlvEntry:
    tlv_type: int
    value: bytes
    name: str = "custom"


@dataclass(frozen=True)
class BuiltPacket:
    body: bytes
    expected_tag: bytes
    transmitted_tag: bytes
    packet: bytes
    tlv_length: int
    payload_hash: str

    @property
    def packet_hex(self) -> str:
        return self.packet.hex().upper()

    @property
    def payload_b64(self) -> str:
        return base64.b64encode(self.packet).decode("ascii")


def load_credential_bundle(path: Path) -> CredentialBundle:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(raw, list):
        device_items = raw
        gateway_items: list[Any] = []
    elif isinstance(raw, dict):
        unknown = set(raw) - {"schema_version", "devices", "gateways"}
        if unknown:
            raise ValueError(
                "credentials bundle contains unknown field(s): " + ", ".join(sorted(unknown))
            )
        if raw.get("schema_version", 1) != 1:
            raise ValueError("credentials bundle schema_version must be 1")
        device_items = raw.get("devices")
        gateway_items = raw.get("gateways", [])
        if not isinstance(gateway_items, list):
            raise ValueError("credentials bundle gateways must be a JSON array")
    else:
        raise ValueError("credentials file must contain a JSON array or credentials bundle")

    if not isinstance(device_items, list) or not device_items:
        raise ValueError("credentials bundle devices must be a non-empty JSON array")

    credentials: list[DeviceCredential] = []
    seen: set[int] = set()
    for item in device_items:
        if not isinstance(item, dict):
            raise ValueError("each device credential must be a JSON object")
        device_id = item.get("device_id")
        token = _bearer_token(item, f"device_id {device_id}")
        key_text = item.get("hmac_key_b64")
        if not isinstance(device_id, int) or isinstance(device_id, bool):
            raise ValueError("device_id must be an integer")
        _range(device_id, 1, 65_535, "device_id")
        if device_id in seen:
            raise ValueError(f"device_id {device_id} is duplicated")
        hmac_key = decode_hmac_key(key_text, f"device_id {device_id} HMAC key")
        seen.add(device_id)
        credentials.append(DeviceCredential(device_id, token, hmac_key))

    gateways: list[GatewayCredential] = []
    seen_gateways: set[str] = set()
    for item in gateway_items:
        if not isinstance(item, dict):
            raise ValueError("each gateway credential must be a JSON object")
        gateway_guid16 = normalize_gateway_guid16(item.get("gateway_guid16"))
        if gateway_guid16 in seen_gateways:
            raise ValueError(f"gateway_guid16 {gateway_guid16} is duplicated")
        token = _bearer_token(item, f"gateway_guid16 {gateway_guid16}")
        display_name = item.get("display_name")
        if display_name is not None and (
            not isinstance(display_name, str) or not 1 <= len(display_name.strip()) <= 80
        ):
            raise ValueError(
                f"gateway_guid16 {gateway_guid16} display_name must contain 1..80 characters"
            )
        seen_gateways.add(gateway_guid16)
        gateways.append(
            GatewayCredential(
                gateway_guid16,
                token,
                display_name.strip() if display_name is not None else None,
            )
        )
    return CredentialBundle(tuple(credentials), tuple(gateways))


def load_credentials(path: Path) -> list[DeviceCredential]:
    """Load device credentials from either the legacy array or typed bundle."""
    return list(load_credential_bundle(path).devices)


def normalize_gateway_guid16(value: Any) -> str:
    if isinstance(value, int) and not isinstance(value, bool):
        _range(value, 1, 0xFFFF, "gateway_guid16")
        return f"{value:04X}"
    if not isinstance(value, str) or re.fullmatch(r"[0-9A-Fa-f]{4}", value.strip()) is None:
        raise ValueError("gateway_guid16 must be four hexadecimal characters from 0001..FFFF")
    normalized = value.strip().upper()
    if int(normalized, 16) == 0:
        raise ValueError("gateway_guid16 must be four hexadecimal characters from 0001..FFFF")
    return normalized


def _bearer_token(item: dict[str, Any], owner: str) -> str:
    legacy = item.get("token")
    canonical = item.get("bearer_token")
    if legacy is not None and canonical is not None and legacy != canonical:
        raise ValueError(f"{owner} token and bearer_token values do not match")
    token = canonical if canonical is not None else legacy
    return validate_bearer_token(token, f"{owner} bearer token")


def validate_bearer_token(value: Any, field: str = "bearer token") -> str:
    if not isinstance(value, str) or re.fullmatch(r"[A-Za-z0-9_-]{32,256}", value) is None:
        raise ValueError(f"{field} must contain 32..256 URL-safe characters")
    return value


def decode_hmac_key(value: Any, field: str = "HMAC key") -> bytes:
    if not isinstance(value, str):
        raise ValueError(f"{field} must be Base64 text")
    try:
        decoded = base64.b64decode(value.strip(), validate=True)
    except (ValueError, binascii.Error) as error:
        raise ValueError(f"{field} is not valid Base64") from error
    if len(decoded) != 32:
        raise ValueError(f"{field} must decode to 32 bytes")
    return decoded


def encode_tlv(tlv_type: int, value: bytes) -> bytes:
    _range(tlv_type, 0, 255, "TLV type")
    if not isinstance(value, bytes) or not 1 <= len(value) <= 255:
        raise ValueError("TLV value must contain 1..255 bytes")
    return bytes((tlv_type, len(value))) + value


def known_tlv(tlv_type: int, value: int) -> TlvEntry:
    spec = KNOWN_TLV_LENGTHS.get(tlv_type)
    if spec is None:
        raise ValueError(f"0x{tlv_type:02X} is not a selected v1.1 TLV")
    name, length = spec
    maximum = (1 << (length * 8)) - 1
    _range(value, 0, maximum, name)
    return TlvEntry(tlv_type, value.to_bytes(length, "little"), name)


def firmware_tlv(value: str) -> TlvEntry:
    parts = value.strip().split(".")
    if len(parts) != 2:
        raise ValueError("firmware version must use major.minor, for example 1.1")
    try:
        major, minor = (int(part, 10) for part in parts)
    except ValueError as error:
        raise ValueError("firmware major and minor must be decimal integers") from error
    _range(major, 0, 255, "firmware major")
    _range(minor, 0, 255, "firmware minor")
    return known_tlv(0x04, (major << 8) | minor)


def custom_tlv(type_text: str, value_hex: str) -> TlvEntry:
    cleaned_type = type_text.strip().lower()
    try:
        tlv_type = int(cleaned_type, 16) if cleaned_type.startswith("0x") else int(cleaned_type, 16)
    except ValueError as error:
        raise ValueError("custom TLV type must be hexadecimal, such as 7E or 0x7E") from error
    _range(tlv_type, 0, 255, "custom TLV type")
    cleaned_value = "".join(value_hex.split())
    if cleaned_value.lower().startswith("0x"):
        cleaned_value = cleaned_value[2:]
    if not cleaned_value or len(cleaned_value) % 2:
        raise ValueError("custom TLV value must contain complete hexadecimal bytes")
    try:
        value = bytes.fromhex(cleaned_value)
    except ValueError as error:
        raise ValueError("custom TLV value contains non-hexadecimal characters") from error
    if not 1 <= len(value) <= 22:
        raise ValueError("custom TLV value must contain 1..22 bytes")
    return TlvEntry(tlv_type, value, "custom")


def build_tlv_packet(
    fields: PacketFields,
    tlvs: Iterable[TlvEntry],
    hmac_key: bytes,
    *,
    tag_mode: str = "valid",
    custom_tag_hex: str = "",
) -> BuiltPacket:
    _validate_packet_fields(fields)
    if not isinstance(hmac_key, bytes) or len(hmac_key) != 32:
        raise ValueError("HMAC key must contain exactly 32 bytes")

    encoded_tlvs: list[bytes] = []
    seen_known: set[int] = set()
    for entry in tlvs:
        if not isinstance(entry, TlvEntry):
            raise ValueError("TLV entries must be TlvEntry values")
        selected = KNOWN_TLV_LENGTHS.get(entry.tlv_type)
        if selected:
            if entry.tlv_type in seen_known:
                raise ValueError(f"known TLV 0x{entry.tlv_type:02X} appears more than once")
            expected_length = selected[1]
            if len(entry.value) != expected_length:
                raise ValueError(
                    f"known TLV 0x{entry.tlv_type:02X} must contain {expected_length} bytes"
                )
            seen_known.add(entry.tlv_type)
        encoded_tlvs.append(encode_tlv(entry.tlv_type, entry.value))
    tlv_bytes = b"".join(encoded_tlvs)
    if len(tlv_bytes) > MAX_TLV_SIZE:
        raise ValueError(f"TLV section uses {len(tlv_bytes)} bytes; v1.1 allows at most 24")

    header = bytearray(HEADER_SIZE)
    header[0] = fields.protocol_version
    struct.pack_into("<H", header, 1, fields.device_id)
    struct.pack_into("<H", header, 3, fields.message_sequence)
    struct.pack_into("<I", header, 5, fields.timestamp)
    header[9] = (fields.power_profile << 4) | fields.status
    header[10] = fields.flags
    header[11] = fields.tx_reason
    struct.pack_into("<i", header, 12, round(fields.latitude * 10_000_000))
    struct.pack_into("<i", header, 16, round(fields.longitude * 10_000_000))
    struct.pack_into("<H", header, 20, fields.battery_mv)
    struct.pack_into("<H", header, 22, fields.accuracy_m)
    struct.pack_into("<H", header, 24, fields.fix_age_s)
    header[26] = fields.satellite_count
    # Bytes 27..30 stay zero by construction.
    header[31] = len(tlv_bytes)

    body = bytes(header) + tlv_bytes
    expected_tag = hmac.new(hmac_key, body, hashlib.sha256).digest()[:AUTH_TAG_SIZE]
    if tag_mode == "valid":
        transmitted_tag = expected_tag
    elif tag_mode == "corrupt":
        transmitted_tag = bytes((expected_tag[0] ^ 0x01,)) + expected_tag[1:]
    elif tag_mode == "custom":
        cleaned = "".join(custom_tag_hex.split())
        try:
            transmitted_tag = bytes.fromhex(cleaned)
        except ValueError as error:
            raise ValueError("custom HMAC tag must be hexadecimal") from error
        if len(transmitted_tag) != AUTH_TAG_SIZE:
            raise ValueError("custom HMAC tag must contain exactly 8 bytes (16 hex characters)")
    else:
        raise ValueError("HMAC mode must be valid, corrupt, or custom")

    packet = body + transmitted_tag
    return BuiltPacket(
        body=body,
        expected_tag=expected_tag,
        transmitted_tag=transmitted_tag,
        packet=packet,
        tlv_length=len(tlv_bytes),
        payload_hash=hashlib.sha256(packet).hexdigest(),
    )


def build_transport_wrapper(
    payload_b64: str,
    transport: str,
    *,
    gateway_guid16: str | None = None,
    gateway_rx_time_unix: int | None = None,
    link_rssi_dbm: float | None = None,
    link_snr_db: float | None = None,
    cell_rsrp_dbm: float | None = None,
    cell_rsrq_db: float | None = None,
    cell_sinr_db: float | None = None,
) -> dict[str, Any]:
    validate_payload_b64(payload_b64)
    wrapper: dict[str, Any]
    if transport in ("lora_hub", "lora_gateway"):
        gateway = (gateway_guid16 or "").strip().upper()
        if len(gateway) != 4:
            raise ValueError("gateway GUID must be exactly four hexadecimal characters")
        try:
            gateway_number = int(gateway, 16)
        except ValueError as error:
            raise ValueError("gateway GUID must be exactly four hexadecimal characters") from error
        _range(gateway_number, 1, 0xFFFF, "gateway GUID")
        if gateway_rx_time_unix is None:
            raise ValueError("gateway receive timestamp is required for LoRa")
        _range(gateway_rx_time_unix, 0, 0xFFFF_FFFF, "gateway receive timestamp")
        if any(value is not None for value in (cell_rsrp_dbm, cell_rsrq_db, cell_sinr_db)):
            raise ValueError("cellular RF fields are not valid for a LoRa wrapper")
        wrapper = {
            "format": "tlv",
            "ingest_path": "lora_gateway",
            "link_type": "lora",
            "gateway_guid16": gateway,
            "gateway_rx_time_unix": gateway_rx_time_unix,
        }
    elif transport == "cellular_direct":
        if gateway_guid16 is not None or gateway_rx_time_unix is not None:
            raise ValueError("gateway fields are not valid for a cellular wrapper")
        wrapper = {"format": "tlv", "ingest_path": "cellular_direct", "link_type": "lte"}
    else:
        raise ValueError("transport must be cellular_direct, lora_hub, or lora_gateway")

    optional_values = {
        "link_rssi_dbm": _optional_number(link_rssi_dbm, -200, 0, "link RSSI"),
        "link_snr_db": _optional_number(link_snr_db, -100, 100, "link SNR"),
        "cell_rsrp_dbm": _optional_number(cell_rsrp_dbm, -200, 0, "cell RSRP"),
        "cell_rsrq_db": _optional_number(cell_rsrq_db, -100, 0, "cell RSRQ"),
        "cell_sinr_db": _optional_number(cell_sinr_db, -100, 100, "cell SINR"),
    }
    wrapper.update({key: value for key, value in optional_values.items() if value is not None})
    wrapper["payload_b64"] = payload_b64
    return wrapper


def validate_payload_b64(value: str) -> bytes:
    if not isinstance(value, str) or not value or len(value) % 4:
        raise ValueError("payload must be non-empty canonical Base64")
    try:
        decoded = base64.b64decode(value, validate=True)
    except (ValueError, binascii.Error) as error:
        raise ValueError("payload is not canonical Base64") from error
    if not MIN_PACKET_SIZE <= len(decoded) <= MAX_PACKET_SIZE:
        raise ValueError(f"decoded TLV packet must contain {MIN_PACKET_SIZE}..{MAX_PACKET_SIZE} bytes")
    if len(decoded) != HEADER_SIZE + decoded[31] + AUTH_TAG_SIZE:
        raise ValueError("decoded packet length does not match its tlv_len header byte")
    return decoded


def decode_tlv_payload(
    payload_b64: str, hmac_key: bytes | None = None
) -> dict[str, Any]:
    """Decode a v1.1 payload into a human-readable JSON-safe structure."""
    packet = validate_payload_b64(payload_b64)
    if hmac_key is not None and (
        not isinstance(hmac_key, bytes) or len(hmac_key) != 32
    ):
        raise ValueError("HMAC key must contain exactly 32 bytes")

    status_profile = packet[9]
    status = status_profile & 0x0F
    power_profile = status_profile >> 4
    flags = packet[10]
    tx_reason = packet[11]
    timestamp = struct.unpack_from("<I", packet, 5)[0]
    tlv_length = packet[31]
    tlv_bytes = packet[HEADER_SIZE : HEADER_SIZE + tlv_length]

    decoded_tlvs: list[dict[str, Any]] = []
    seen_known: set[int] = set()
    offset = 0
    while offset < len(tlv_bytes):
        if len(tlv_bytes) - offset < 2:
            raise ValueError("TLV section ends before a complete type/length header")
        tlv_type = tlv_bytes[offset]
        value_length = tlv_bytes[offset + 1]
        value_start = offset + 2
        value_end = value_start + value_length
        if value_length == 0 or value_end > len(tlv_bytes):
            raise ValueError(f"TLV 0x{tlv_type:02X} has an invalid value length")
        value = tlv_bytes[value_start:value_end]
        spec = KNOWN_TLV_LENGTHS.get(tlv_type)
        entry: dict[str, Any] = {
            "type": f"0x{tlv_type:02X}",
            "length_bytes": value_length,
            "raw_value_hex": value.hex().upper(),
        }
        if spec is None:
            entry.update({"name": "unknown", "value": value.hex().upper()})
        else:
            name, expected_length = spec
            if tlv_type in seen_known:
                raise ValueError(f"known TLV 0x{tlv_type:02X} appears more than once")
            if value_length != expected_length:
                raise ValueError(
                    f"known TLV 0x{tlv_type:02X} must contain {expected_length} bytes"
                )
            seen_known.add(tlv_type)
            number = int.from_bytes(value, "little")
            human_value: int | str
            if tlv_type == 0x04:
                human_value = f"{(number >> 8) & 0xFF}.{number & 0xFF}"
            else:
                human_value = number
            entry.update({"name": name, "value": human_value})
        decoded_tlvs.append(entry)
        offset = value_end

    body = packet[:-AUTH_TAG_SIZE]
    transmitted_tag = packet[-AUTH_TAG_SIZE:]
    expected_tag = (
        hmac.new(hmac_key, body, hashlib.sha256).digest()[:AUTH_TAG_SIZE]
        if hmac_key is not None
        else None
    )
    status_name = _name_for_code(STATUS_CODES, status)
    profile_name = _name_for_code(POWER_PROFILE_CODES, power_profile)
    reason_name = _name_for_code(TX_REASON_CODES, tx_reason)
    fix_age_s = struct.unpack_from("<H", packet, 24)[0]
    satellite_count = packet[26]

    return {
        "packet": {
            "size_bytes": len(packet),
            "header_size_bytes": HEADER_SIZE,
            "tlv_length_bytes": tlv_length,
            "authentication_tag_size_bytes": AUTH_TAG_SIZE,
            "sha256": hashlib.sha256(packet).hexdigest(),
        },
        "header": {
            "protocol_version": packet[0],
            "device_id": struct.unpack_from("<H", packet, 1)[0],
            "message_sequence": struct.unpack_from("<H", packet, 3)[0],
            "timestamp_unix": timestamp,
            "timestamp_utc": datetime.fromtimestamp(timestamp, timezone.utc)
            .isoformat()
            .replace("+00:00", "Z"),
            "status": {"code": status, "name": status_name},
            "power_profile": {"code": power_profile, "name": profile_name},
            "flags": {
                "raw": flags,
                "hex": f"0x{flags:02X}",
                "set": [name for name, mask in FLAG_MASKS.items() if flags & mask],
            },
            "tx_reason": {"code": tx_reason, "name": reason_name},
            "position": {
                "latitude": struct.unpack_from("<i", packet, 12)[0] / 10_000_000,
                "longitude": struct.unpack_from("<i", packet, 16)[0] / 10_000_000,
                "battery_mv": struct.unpack_from("<H", packet, 20)[0],
                "accuracy_m": struct.unpack_from("<H", packet, 22)[0],
                "fix_age_s": None if fix_age_s == 65_535 else fix_age_s,
                "satellite_count": None if satellite_count == 255 else satellite_count,
            },
            "reserved_bytes_hex": packet[27:31].hex().upper(),
        },
        "tlvs": decoded_tlvs,
        "authentication": {
            "algorithm": "HMAC-SHA256-64",
            "tag_hex": transmitted_tag.hex().upper(),
            "expected_tag_hex": (
                expected_tag.hex().upper() if expected_tag is not None else None
            ),
            "valid": (
                hmac.compare_digest(transmitted_tag, expected_tag)
                if expected_tag is not None
                else None
            ),
        },
    }


def _name_for_code(codes: dict[str, int], value: int) -> str:
    return next((name for name, code in codes.items() if code == value), "UNKNOWN")


def post_wrapper(
    url: str,
    bearer_token: str,
    wrapper: dict[str, Any],
    timeout: float,
    *,
    user_agent: str = "bluepaws-tlv-simulator/2",
) -> tuple[int, dict[str, Any]]:
    if not url.lower().startswith("https://"):
        raise ValueError("ingestion endpoint must use HTTPS")
    validate_bearer_token(bearer_token)
    if timeout <= 0:
        raise ValueError("HTTP timeout must be positive")
    request = urllib.request.Request(
        url,
        data=json.dumps(wrapper, separators=(",", ":")).encode("utf-8"),
        method="POST",
        headers={
            "Authorization": f"Bearer {bearer_token}",
            "Content-Type": "application/json",
            "User-Agent": user_agent,
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, _decode_response(response.read())
    except urllib.error.HTTPError as error:
        return error.code, _decode_response(error.read(), fallback=str(error.reason))


def _validate_packet_fields(fields: PacketFields) -> None:
    if fields.protocol_version != 1:
        raise ValueError("the deployed protocol version must be 1")
    _range(fields.device_id, 1, 65_535, "device ID")
    _range(fields.message_sequence, 0, 65_535, "message sequence")
    _range(fields.timestamp, 0, 0xFFFF_FFFF, "timestamp")
    _range(fields.status, 0, 3, "status")
    _range(fields.power_profile, 0, 3, "power profile")
    _range(fields.flags, 0, 255, "flags")
    _range(fields.tx_reason, 0, 7, "TX reason")
    if not -90 <= fields.latitude <= 90:
        raise ValueError("latitude must be from -90 to 90")
    if not -180 <= fields.longitude <= 180:
        raise ValueError("longitude must be from -180 to 180")
    _range(fields.battery_mv, 0, 65_535, "battery millivolts")
    _range(fields.accuracy_m, 0, 65_535, "accuracy metres")
    _range(fields.fix_age_s, 0, 65_535, "fix age seconds")
    _range(fields.satellite_count, 0, 255, "satellite count")


def _range(value: Any, minimum: int, maximum: int, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValueError(f"{field} must be an integer from {minimum} to {maximum}")
    return value


def _optional_number(
    value: float | None, minimum: float, maximum: float, field: str
) -> float | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not minimum <= value <= maximum:
        raise ValueError(f"{field} must be a number from {minimum} to {maximum}")
    return value


def _decode_response(raw: bytes, fallback: str = "") -> dict[str, Any]:
    text = raw.decode("utf-8", errors="replace")
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        return {"error": text or fallback}
    return value if isinstance(value, dict) else {"response": value}
