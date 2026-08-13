import base64
import hashlib
import hmac
import struct
import unittest

from tlv_packet_codec import (
    PacketFields,
    TlvEntry,
    build_tlv_packet,
    build_transport_wrapper,
    custom_tlv,
    firmware_tlv,
    known_tlv,
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
        self.assertEqual(lora["link_type"], "lora")
        self.assertEqual(lora["gateway_guid16"], "0016")
        self.assertNotIn("cell_rsrp_dbm", lora)

    def test_rejects_backend_reserved_values_and_invalid_cross_transport_fields(self):
        key = bytes(32)
        with self.assertRaisesRegex(ValueError, "TX reason"):
            build_tlv_packet(packet_fields(tx_reason=7), [], key)
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
