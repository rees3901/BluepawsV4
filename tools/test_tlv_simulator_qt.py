"""Feature-parity smoke tests for the PySide6 TLV console."""

from __future__ import annotations

import base64
import importlib.util
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


PYSIDE_AVAILABLE = importlib.util.find_spec("PySide6") is not None

if PYSIDE_AVAILABLE:
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from PySide6.QtCore import QUrlQuery
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication

    from tlv_simulator_qt import (
        STYLESHEET,
        TAG_MODES,
        TRANSPORTS,
        BluepawsTlvConsole,
        LIVE_PREVIEW_DELAY_MS,
        drift_coordinates,
        parse_coordinates,
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

    def test_packet_and_wrapper_previews_update_automatically(self) -> None:
        original_payload = self.window.packet_b64.toPlainText()
        self.window.latitude.setText("51.5080000")
        QTest.qWait(LIVE_PREVIEW_DELAY_MS + 50)
        self.app.processEvents()

        self.assertNotEqual(self.window.packet_b64.toPlainText(), original_payload)
        self.assertIn(
            self.window.packet_b64.toPlainText(), self.window.wrapper_preview.toPlainText()
        )

        self.window.link_rssi.setText("-91")
        QTest.qWait(LIVE_PREVIEW_DELAY_MS + 50)
        self.app.processEvents()
        self.assertIn('"link_rssi_dbm": -91.0', self.window.wrapper_preview.toPlainText())

    def test_invalid_edit_clears_generated_output(self) -> None:
        self.window.latitude.setText("not-a-coordinate")
        QTest.qWait(LIVE_PREVIEW_DELAY_MS + 50)
        self.app.processEvents()

        self.assertEqual(self.window.packet_b64.toPlainText(), "")
        self.assertEqual(self.window.packet_hex.toPlainText(), "")
        self.assertEqual(self.window.wrapper_preview.toPlainText(), "")

    def test_send_forces_rebuild_before_debounce_expires(self) -> None:
        self.window.advance_packets.setChecked(False)
        original_payload = self.window.packet_b64.toPlainText()
        self.window.latitude.setText("51.5090000")
        with patch(
            "tlv_simulator_qt.post_wrapper", return_value=(201, {"accepted": True})
        ) as mocked_post:
            self.window.send_requests()
            deadline = time.monotonic() + 2
            while self.window._worker is not None and self.window._worker.is_alive():
                self.app.processEvents()
                if time.monotonic() >= deadline:
                    self.fail("background send worker did not finish")
                time.sleep(0.01)
            self.app.processEvents()

        sent_payload = mocked_post.call_args.args[2]["payload_b64"]
        self.assertNotEqual(sent_payload, original_payload)
        self.assertEqual(sent_payload, self.window.packet_b64.toPlainText())

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
        with patch("tlv_simulator_qt.time.time", return_value=2000):
            requests = self.window._prepare_requests(3, 2.0)

        self.assertEqual([sequence for sequence, _ in requests], [65535, 0, 1])
        self.assertEqual([fields.timestamp for fields in self.window._prepared_fields], [2000, 2002, 2004])
        self.assertEqual(len({wrapper["payload_b64"] for _, wrapper in requests}), 3)

    def test_live_simulation_is_default_and_exact_repeat_is_opt_in(self) -> None:
        self.assertTrue(self.window.advance_packets.isChecked())
        base_sequence = self.window._int(self.window.sequence.text(), "sequence")
        with patch("tlv_simulator_qt.time.time", return_value=3000):
            live_requests = self.window._prepare_requests(3, 1.0)
        self.assertEqual(
            [sequence for sequence, _ in live_requests],
            [(base_sequence + index) & 0xFFFF for index in range(3)],
        )
        self.assertEqual(len({wrapper["payload_b64"] for _, wrapper in live_requests}), 3)

        self.window.advance_packets.setChecked(False)
        repeated = self.window._prepare_requests(3, 1.0)
        self.assertEqual(len({wrapper["payload_b64"] for _, wrapper in repeated}), 1)

    def test_coordinate_parser_accepts_google_maps_formats(self) -> None:
        expected = (51.5074, -0.1278)
        samples = (
            "51.5074, -0.1278",
            "https://www.google.com/maps/@51.5074,-0.1278,15z",
            "https://www.google.com/maps/search/?api=1&query=51.5074%2C-0.1278",
            "https://www.google.com/maps/place/Test/data=!3d51.5074!4d-0.1278",
        )
        for sample in samples:
            with self.subTest(sample=sample):
                self.assertEqual(parse_coordinates(sample), expected)

    def test_google_maps_launcher_uses_official_maps_url(self) -> None:
        self.window.latitude.setText("51.5074")
        self.window.longitude.setText("-0.1278")
        with patch("tlv_simulator_qt.QDesktopServices.openUrl", return_value=True) as opener:
            self.window._open_google_maps()

        url = opener.call_args.args[0]
        query = QUrlQuery(url)
        self.assertEqual(url.host(), "www.google.com")
        self.assertEqual(url.path(), "/maps/search/")
        self.assertEqual(query.queryItemValue("api"), "1")
        self.assertEqual(query.queryItemValue("query"), "51.5074000,-0.1278000")

    def test_drift_step_stays_within_configured_radius(self) -> None:
        class FixedRandom:
            values = iter((1.0, 0.25))

            @classmethod
            def random(cls) -> float:
                return next(cls.values)

        start = (51.5074, -0.1278)
        moved = drift_coordinates(*start, 100, rng=FixedRandom)
        self.assertLessEqual(haversine_metres(*start, *moved), 100.01)
        self.assertGreater(haversine_metres(*start, *moved), 99.9)

    def test_live_drift_changes_each_position_cumulatively(self) -> None:
        self.window.drift_enabled.setChecked(True)
        self.window.maximum_drift.setText("100")
        with (
            patch("tlv_simulator_qt.time.time", return_value=4000),
            patch(
                "tlv_simulator_qt.drift_coordinates",
                side_effect=lambda latitude, longitude, _radius: (
                    latitude + 0.0001,
                    longitude - 0.0001,
                ),
            ),
        ):
            requests = self.window._prepare_requests(3, 2.0)

        latitudes = [round(fields.latitude, 7) for fields in self.window._prepared_fields]
        longitudes = [round(fields.longitude, 7) for fields in self.window._prepared_fields]
        self.assertEqual(latitudes, [51.907055, 51.907155, 51.907255])
        self.assertEqual(longitudes, [-2.25666, -2.25676, -2.25686])
        self.assertEqual(len({wrapper["payload_b64"] for _, wrapper in requests}), 3)

    def test_transport_switches_relevant_metadata_controls(self) -> None:
        self.window.transport.setCurrentText(next(iter(TRANSPORTS)))
        self.assertFalse(self.window.gateway_guid.isEnabled())
        self.assertTrue(self.window.cell_rsrp.isEnabled())
        self.assertIn("device bearer token", self.window.bearer_hint.text())

        self.window.transport.setCurrentText("LoRa home-hub relay (lora_hub)")
        self.assertTrue(self.window.gateway_guid.isEnabled())
        self.assertFalse(self.window.cell_rsrp.isEnabled())
        self.assertIn("gateway bearer token", self.window.bearer_hint.text())
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

    def test_bundle_switches_device_and_gateway_bearers_by_transport(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "credentials.json"
            path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "devices": [
                            {
                                "device_id": 1001,
                                "bearer_token": "d" * 48,
                                "hmac_key_b64": base64.b64encode(bytes(32)).decode(),
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
            with patch(
                "tlv_simulator_qt.QFileDialog.getOpenFileName",
                return_value=(str(path), "JSON files (*.json)"),
            ):
                self.window.load_credentials_file()

        self.assertEqual(self.window.bearer.text(), "d" * 48)
        self.assertEqual(self.window.gateway_combo.count(), 1)
        self.assertIn("Test Hub", self.window.gateway_combo.currentText())

        self.window.transport.setCurrentText("LoRa home-hub relay (lora_hub)")
        self.assertEqual(self.window.bearer.text(), "g" * 48)
        self.assertEqual(self.window.gateway_guid.text(), "0016")

        self.window.transport.setCurrentText("LTE direct (cellular_direct)")
        self.assertEqual(self.window.bearer.text(), "d" * 48)

    def test_background_send_returns_through_qt_signals(self) -> None:
        self.window.send_count.setText("2")
        self.window.send_interval.setText("0")
        with patch(
            "tlv_simulator_qt.post_wrapper", return_value=(201, {"accepted": True})
        ) as mocked_post:
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
        sent_payloads = [call.args[2]["payload_b64"] for call in mocked_post.call_args_list]
        self.assertEqual(len(sent_payloads), 2)
        self.assertEqual(len(set(sent_payloads)), 2)


def json_keys_lower(value: dict[str, object]) -> set[str]:
    return {key.lower() for key in value}


def haversine_metres(
    latitude_a: float,
    longitude_a: float,
    latitude_b: float,
    longitude_b: float,
) -> float:
    latitude_delta = math.radians(latitude_b - latitude_a)
    longitude_delta = math.radians(longitude_b - longitude_a)
    a = (
        math.sin(latitude_delta / 2) ** 2
        + math.cos(math.radians(latitude_a))
        * math.cos(math.radians(latitude_b))
        * math.sin(longitude_delta / 2) ** 2
    )
    return 6_371_000 * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))


if __name__ == "__main__":
    unittest.main()
