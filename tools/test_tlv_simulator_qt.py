"""Feature-parity smoke tests for the PySide6 TLV console."""

from __future__ import annotations

import base64
import importlib.util
import os
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch


PYSIDE_AVAILABLE = importlib.util.find_spec("PySide6") is not None

if PYSIDE_AVAILABLE:
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from PySide6.QtWidgets import QApplication

    from tlv_simulator_qt import (
        STYLESHEET,
        TAG_MODES,
        TRANSPORTS,
        BluepawsTlvConsole,
    )
    from tlv_packet_codec import DeviceCredential


@unittest.skipUnless(PYSIDE_AVAILABLE, "PySide6 GUI dependency is not installed")
class QtConsoleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])
        cls.app.setStyle("Fusion")
        cls.app.setStyleSheet(STYLESHEET)

    def setUp(self) -> None:
        self.window = BluepawsTlvConsole()
        self.window.hmac.setText(base64.b64encode(bytes(range(32))).decode("ascii"))
        self.window.bearer.setText("t" * 48)
        self.window.build_packet()

    def tearDown(self) -> None:
        self.window.close()

    def test_builds_valid_packet_and_lte_wrapper(self) -> None:
        built = self.window.build_packet()
        self.assertIsNotNone(built)
        assert built is not None
        self.assertEqual(built.transmitted_tag, built.expected_tag)
        self.assertEqual(self.window.packet_b64.toPlainText(), built.payload_b64)

        wrapper = self.window.preview_wrapper()
        self.assertIsNotNone(wrapper)
        assert wrapper is not None
        self.assertEqual(wrapper["ingest_path"], "cellular_direct")
        self.assertEqual(wrapper["payload_b64"], built.payload_b64)
        self.assertNotIn("authorization", json_keys_lower(wrapper))

    def test_custom_tlv_and_corrupt_hmac_negative_packet(self) -> None:
        self.window.custom_type.setText("7E")
        self.window.custom_value.setText("010203")
        self.window._add_custom_tlv()
        self.window.tag_mode.setCurrentText("Corrupt one HMAC bit (negative test)")
        built = self.window.build_packet()

        self.assertEqual(self.window.custom_table.rowCount(), 1)
        self.assertIsNotNone(built)
        assert built is not None
        self.assertNotEqual(built.transmitted_tag, built.expected_tag)

    def test_advance_packets_increments_sequence_and_timestamp(self) -> None:
        self.window.sequence.setText("65535")
        self.window.timestamp.setText("1000")
        self.window.advance_packets.setChecked(True)
        requests = self.window._prepare_requests(3, 2.0)

        self.assertEqual([sequence for sequence, _ in requests], [65535, 0, 1])
        self.assertEqual(len({wrapper["payload_b64"] for _, wrapper in requests}), 3)

    def test_transport_switches_relevant_metadata_controls(self) -> None:
        self.window.transport.setCurrentText(next(iter(TRANSPORTS)))
        self.assertFalse(self.window.gateway_guid.isEnabled())
        self.assertTrue(self.window.cell_rsrp.isEnabled())

        self.window.transport.setCurrentText("LoRa home-hub relay (lora_hub)")
        self.assertTrue(self.window.gateway_guid.isEnabled())
        self.assertFalse(self.window.cell_rsrp.isEnabled())
        self.assertEqual(TAG_MODES[self.window.tag_mode.currentText()], "valid")
        wrapper = self.window.preview_wrapper()
        self.assertIsNotNone(wrapper)
        assert wrapper is not None
        self.assertEqual(wrapper["ingest_path"], "lora_hub")
        self.assertEqual(wrapper["gateway_guid16"], "0016")
        self.assertNotIn("cell_rsrp_dbm", wrapper)

    def test_selected_credential_populates_masked_secret_fields(self) -> None:
        key = bytes(reversed(range(32)))
        credential = DeviceCredential(device_id=4321, token="z" * 48, hmac_key=key)
        self.window._apply_credential(credential)

        self.assertEqual(self.window.device_id.text(), "4321")
        self.assertEqual(self.window.hmac.text(), base64.b64encode(key).decode("ascii"))
        self.assertEqual(self.window.bearer.text(), "z" * 48)
        self.assertEqual(self.window.hmac.echoMode(), self.window.hmac.EchoMode.Password)
        self.assertEqual(self.window.bearer.echoMode(), self.window.bearer.EchoMode.Password)

    def test_background_send_returns_through_qt_signals(self) -> None:
        self.window.send_count.setText("2")
        self.window.send_interval.setText("0")
        with patch("tlv_simulator_qt.post_wrapper", return_value=(201, {"accepted": True})):
            self.window.send_requests()
            deadline = time.monotonic() + 2
            while self.window._worker is not None and self.window._worker.is_alive():
                self.app.processEvents()
                if time.monotonic() >= deadline:
                    self.fail("background send worker did not finish")
                time.sleep(0.01)
            for _ in range(3):
                self.app.processEvents()

        self.assertEqual(self.window.response_log.toPlainText().count('"status":201'), 2)
        self.assertIn("Completed 2 of 2 request(s).", self.window.sender_status.text())
        self.assertTrue(self.window.send_button.isEnabled())
        self.assertFalse(self.window.stop_button.isEnabled())


def json_keys_lower(value: dict[str, object]) -> set[str]:
    return {key.lower() for key in value}


if __name__ == "__main__":
    unittest.main()
