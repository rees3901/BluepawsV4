import base64
import hashlib
import hmac
import json
import struct
import tempfile
import unittest
from pathlib import Path

from tlv_packet_codec import (
    PacketFields,
    TlvEntry,
    build_tlv_packet,
    build_transport_wrapper,
    custom_tlv,
    decode_tlv_payload,
    firmware_tlv,
    known_tlv,
    load_credential_bundle,
    load_credentials,
    validate_payload_b64,
)


def packet_fields(**overrides):
    values = {
        "device_id": 1001,
        "message_sequence": 42,
        "timestamp": 1_700_000_000,
        "status": 1,
        "power_profile": 1,
        "flags": 0x03,
        "tx_reason": 0,
        "latitude": 51.5,
        "longitude": -0.1,
        "battery_mv": 3900,
        "accuracy_m": 8,
        "fix_age_s": 0,
        "satellite_count": 9,
    }
    values.update(overrides)
    return PacketFields(**values)


class TlvPacketCodecTests(unittest.TestCase):
    def test_loads_typed_bundle_and_preserves_legacy_device_arrays(self):
        key = base64.b64encode(bytes(32)).decode()
        with tempfile.TemporaryDirectory() as directory:
            bundle_path = Path(directory) / "credentials.json"
            bundle_path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "devices": [
                            {
                                "device_id": 1001,
                                "bearer_token": "d" * 48,
                                "hmac_key_b64": key,
                            }
                        ],
                        "gateways": [
                            {
                                "gateway_guid16": "0016",
                                "bearer_token": "g" * 48,
                                "display_name": "Test Hub",
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            bundle = load_credential_bundle(bundle_path)
            self.assertEqual(bundle.devices[0].device_id, 1001)
            self.assertEqual(bundle.gateways[0].gateway_guid16, "0016")
            self.assertEqual(bundle.gateways[0].display_name, "Test Hub")

            legacy_path = Path(directory) / "legacy.json"
            legacy_path.write_text(
                json.dumps(
                    [
                        {
                            "device_id": 1002,
                            "token": "l" * 48,
                            "hmac_key_b64": key,
                        }
                    ]
                ),
                encoding="utf-8",
            )
            self.assertEqual(load_credentials(legacy_path)[0].device_id, 1002)

    def test_rejects_duplicate_or_zero_gateway_identifiers(self):
        key = base64.b64encode(bytes(32)).decode()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "credentials.json"
            body = {
                "devices": [
                    {
                        "device_id": 1001,
                        "bearer_token": "d" * 48,
                        "hmac_key_b64": key,
                    }
                ],
                "gateways": [
                    {"gateway_guid16": "0000", "bearer_token": "g" * 48}
                ],
            }
            path.write_text(json.dumps(body), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "0001..FFFF"):
                load_credential_bundle(path)

    def test_builds_locked_layout_and_valid_hmac(self):
        key = bytes(range(32))
        built = build_tlv_packet(
            packet_fields(),
            [firmware_tlv("1.1"), known_tlv(0x10, 60), known_tlv(0x13, 42)],
            key,
        )
        packet = built.packet
        self.assertEqual(packet[0], 1)
        self.assertEqual(struct.unpack_from("<H", packet, 1)[0], 1001)
        self.assertEqual(struct.unpack_from("<i", packet, 12)[0], 515_000_000)
        self.assertEqual(packet[27:31], bytes(4))
        self.assertEqual(len(packet), 32 + packet[31] + 8)
        self.assertEqual(packet[-8:], hmac.new(key, packet[:-8], hashlib.sha256).digest()[:8])
        self.assertEqual(validate_payload_b64(built.payload_b64), packet)

    def test_decodes_transmitted_payload_into_human_readable_json(self):
        key = bytes(range(32))
        built = build_tlv_packet(
            packet_fields(),
            [
                firmware_tlv("1.1"),
                known_tlv(0x10, 60),
                TlvEntry(0x7E, bytes.fromhex("A1B2")),
            ],
            key,
        )

        decoded = decode_tlv_payload(built.payload_b64, key)

        self.assertEqual(decoded["packet"]["size_bytes"], len(built.packet))
        self.assertEqual(decoded["header"]["device_id"], 1001)
        self.assertEqual(decoded["header"]["status"], {"code": 1, "name": "OUT"})
        self.assertEqual(
            decoded["header"]["flags"]["set"], ["GNSS_VALID", "FIX_3D"]
        )
        self.assertEqual(decoded["header"]["position"]["latitude"], 51.5)
        self.assertEqual(decoded["tlvs"][0]["value"], "1.1")
        self.assertEqual(decoded["tlvs"][1]["value"], 60)
        self.assertEqual(decoded["tlvs"][2]["name"], "unknown")
        self.assertEqual(decoded["tlvs"][2]["value"], "A1B2")
        self.assertTrue(decoded["authentication"]["valid"])

        corrupt = build_tlv_packet(packet_fields(), [], key, tag_mode="corrupt")
        self.assertFalse(
            decode_tlv_payload(corrupt.payload_b64, key)["authentication"]["valid"]
        )

    def test_corrupt_and_custom_tag_modes_are_explicit(self):
        key = bytes(32)
        valid = build_tlv_packet(packet_fields(), [], key)
        corrupt = build_tlv_packet(packet_fields(), [], key, tag_mode="corrupt")
        custom = build_tlv_packet(
            packet_fields(), [], key, tag_mode="custom", custom_tag_hex="0102030405060708"
        )
        self.assertEqual(valid.body, corrupt.body)
        self.assertNotEqual(valid.transmitted_tag, corrupt.transmitted_tag)
        self.assertEqual(custom.transmitted_tag, bytes.fromhex("0102030405060708"))

    def test_enforces_known_tlv_lengths_duplicates_and_budget(self):
        key = bytes(32)
        with self.assertRaisesRegex(ValueError, "appears more than once"):
            build_tlv_packet(
                packet_fields(), [known_tlv(0x13, 1), known_tlv(0x13, 2)], key
            )
        with self.assertRaisesRegex(ValueError, "must contain 2 bytes"):
            build_tlv_packet(packet_fields(), [TlvEntry(0x04, b"x")], key)
        with self.assertRaisesRegex(ValueError, "at most 24"):
            build_tlv_packet(
                packet_fields(),
                [custom_tlv("7E", "AA" * 11), custom_tlv("7F", "BB" * 11)],
                key,
            )

    def test_wrapper_contract_keeps_transport_metadata_separate(self):
        payload = build_tlv_packet(packet_fields(), [], bytes(32)).payload_b64
        lte = build_transport_wrapper(
            payload,
            "cellular_direct",
            link_rssi_dbm=-104,
            cell_rsrp_dbm=-104,
            cell_rsrq_db=-9.5,
            cell_sinr_db=7,
        )
        self.assertEqual(lte["format"], "tlv")
        self.assertEqual(lte["ingest_path"], "cellular_direct")
        self.assertNotIn("gateway_guid16", lte)
        self.assertEqual(base64.b64decode(lte["payload_b64"]), base64.b64decode(payload))

        lora = build_transport_wrapper(
            payload,
            "lora_hub",
            gateway_guid16="0016",
            gateway_rx_time_unix=1_700_000_001,
            link_rssi_dbm=-92,
            link_snr_db=6.25,
        )
        self.assertEqual(lora["format"], "tlv")
        self.assertEqual(lora["ingest_path"], "lora_gateway")
        self.assertEqual(lora["link_type"], "lora")
        self.assertEqual(lora["gateway_guid16"], "0016")
        self.assertNotIn("cell_rsrp_dbm", lora)

    def test_rejects_backend_reserved_values_and_invalid_cross_transport_fields(self):
        key = bytes(32)
        wake_checkin = build_tlv_packet(packet_fields(tx_reason=7, flags=0x08), [], key)
        self.assertEqual(wake_checkin.packet[11], 7)
        with self.assertRaisesRegex(ValueError, "TX reason"):
            build_tlv_packet(packet_fields(tx_reason=8), [], key)
        payload = build_tlv_packet(packet_fields(), [], key).payload_b64
        with self.assertRaisesRegex(ValueError, "cellular RF"):
            build_transport_wrapper(
                payload,
                "lora_hub",
                gateway_guid16="0016",
                gateway_rx_time_unix=1,
                cell_rsrp_dbm=-100,
            )


if __name__ == "__main__":
    unittest.main()
