import base64
import hashlib
import hmac
import json
import random
import struct
import tempfile
import unittest
from pathlib import Path

from tlv_telemetry_simulator import (
    CredentialBundle,
    DeviceCredential,
    DeviceState,
    GatewayCredential,
    build_packet,
    build_wrapper,
    load_credentials,
    resolve_gateway_auth,
)


class TlvSimulatorTests(unittest.TestCase):
    def test_gateway_auth_resolves_from_bundle_and_allows_explicit_override(self):
        bundle = CredentialBundle(
            devices=(DeviceCredential(1001, "d" * 48, bytes(32)),),
            gateways=(GatewayCredential("0016", "g" * 48, "Test Hub"),),
        )
        self.assertEqual(resolve_gateway_auth(bundle, None, None), ("0016", "g" * 48))
        self.assertEqual(
            resolve_gateway_auth(bundle, "0016", "x" * 48), ("0016", "x" * 48)
        )

    def test_gateway_auth_requires_selection_when_bundle_has_multiple_hubs(self):
        bundle = CredentialBundle(
            devices=(DeviceCredential(1001, "d" * 48, bytes(32)),),
            gateways=(
                GatewayCredential("0016", "g" * 48),
                GatewayCredential("0017", "h" * 48),
            ),
        )
        with self.assertRaisesRegex(ValueError, "multiple gateways"):
            resolve_gateway_auth(bundle, None, None)

    def test_load_credentials_requires_a_32_byte_hmac_key(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "devices.json"
            path.write_text(
                json.dumps(
                    [
                        {
                            "device_id": 1001,
                            "token": "a" * 32,
                            "hmac_key_b64": base64.b64encode(bytes(31)).decode(),
                        }
                    ]
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "32 bytes"):
                load_credentials(path)

    def test_packet_matches_locked_layout_and_authenticates(self):
        key = bytes(range(32))
        state = DeviceState(
            DeviceCredential(1001, "a" * 32, key),
            latitude=51.5,
            longitude=-0.1,
            battery_mv=3900,
            message_sequence=41,
        )
        packet = build_packet(state, random.Random(1), 0, timestamp=1_700_000_000)
        tlv_length = packet[31]
        self.assertEqual(len(packet), 32 + tlv_length + 8)
        self.assertEqual(struct.unpack_from("<H", packet, 1)[0], 1001)
        self.assertEqual(struct.unpack_from("<H", packet, 3)[0], 42)
        self.assertEqual(struct.unpack_from("<i", packet, 12)[0], 515_000_000)
        self.assertEqual(packet[27:31], bytes(4))
        expected = hmac.new(key, packet[:-8], hashlib.sha256).digest()[:8]
        self.assertEqual(packet[-8:], expected)

    def test_duplicate_packet_keeps_the_same_payload(self):
        state = DeviceState(
            DeviceCredential(1001, "a" * 32, bytes(32)),
            latitude=51.5,
            longitude=-0.1,
            battery_mv=3900,
            message_sequence=10,
        )
        packet = build_packet(state, random.Random(1), 0, timestamp=1_700_000_000)
        first = build_wrapper(packet, "cellular_direct", random.Random(2), None)
        second = build_wrapper(packet, "cellular_direct", random.Random(3), None)
        self.assertEqual(first["payload_b64"], second["payload_b64"])

    def test_lora_wrapper_preserves_packet_and_route_metadata(self):
        packet_data = bytearray(40)
        packet_data[31] = 0
        packet = bytes(packet_data)
        wrapper = build_wrapper(packet, "lora_hub", random.Random(1), "0016")
        self.assertEqual(wrapper["ingest_path"], "lora_hub")
        self.assertEqual(wrapper["gateway_guid16"], "0016")
        self.assertEqual(base64.b64decode(wrapper["payload_b64"]), packet)


if __name__ == "__main__":
    unittest.main()
