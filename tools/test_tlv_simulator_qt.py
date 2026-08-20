"""Feature-parity smoke tests for the PySide6 TLV console."""

from __future__ import annotations

import base64
import hashlib
import hmac
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
    from PySide6.QtWidgets import QApplication, QHeaderView

    from tlv_simulator_qt import (
        DEFAULT_MOVEMENT_METRES,
        MAX_MOVEMENT_METRES,
        STYLESHEET,
        TAG_MODES,
        TEST_RECIPES,
        TRANSPORTS,
        BluepawsTlvConsole,
        LIVE_PREVIEW_DELAY_MS,
        drift_coordinates,
        parse_coordinates,
    )
    from tlv_packet_codec import CredentialBundle, DeviceCredential


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
        self.assertEqual(len(built.packet), 40)
        self.assertEqual(built.transmitted_tag, built.expected_tag)
        self.assertEqual(self.window.packet_b64.toPlainText(), built.payload_b64)

        wrapper = self.window.preview_wrapper()
        self.assertIsNotNone(wrapper)
        assert wrapper is not None
        self.assertEqual(wrapper["ingest_path"], "cellular_direct")
        self.assertEqual(wrapper["payload_b64"], built.payload_b64)
        self.assertNotIn("authorization", json_keys_lower(wrapper))
        compact_body = json.dumps(wrapper, separators=(",", ":")).encode("utf-8")
        self.assertIn(
            f"JSON body {len(compact_body)} bytes", self.window.wrapper_summary.text()
        )
        self.assertIn(
            f"TLV packet {len(built.packet)} bytes", self.window.wrapper_summary.text()
        )
        self.assertIn("TLS overhead", self.window.wrapper_summary.toolTip())

        decoded = json.loads(self.window.payload_preview.toPlainText())
        self.assertEqual(decoded["header"]["device_id"], 1001)
        self.assertEqual(decoded["header"]["status"]["name"], "HOME")
        self.assertTrue(decoded["authentication"]["valid"])
        self.assertIn("HMAC valid", self.window.payload_summary.text())

    def test_preview_and_response_log_have_dedicated_tabs(self) -> None:
        self.assertEqual(self.window.tabs.count(), 3)
        self.assertEqual(self.window.tabs.tabText(1), "2. HTTPS Wrapper")
        self.assertEqual(self.window.tabs.tabText(2), "3. Send & Response Log")
        self.assertTrue(self.window.wrapper_tab.isAncestorOf(self.window.payload_preview))
        self.assertTrue(self.window.wrapper_tab.isAncestorOf(self.window.wrapper_preview))
        self.assertTrue(
            self.window.response_tab.isAncestorOf(self.window.response_table)
        )
        self.assertFalse(
            self.window.wrapper_tab.isAncestorOf(self.window.response_table)
        )

    def test_safe_defaults_use_valid_hmac_and_disable_optional_tlvs(self) -> None:
        built = self.window.build_packet()

        self.assertEqual(
            self.window.tag_mode.currentText(), "Valid HMAC (normal packet)"
        )
        self.assertFalse(self.window.tlv_options.isChecked())
        self.assertTrue(self.window.drift_enabled.isChecked())
        self.assertTrue(self.window.maximum_drift.isEnabled())
        self.assertEqual(
            self.window.maximum_drift.text(), str(DEFAULT_MOVEMENT_METRES)
        )
        self.assertEqual(self.window.send_count.text(), "5")
        self.assertEqual(self.window.send_interval.text(), "5")
        self.assertEqual(self.window.timeout.text(), "15")
        self.assertFalse(self.window.known_values["uptime_s"].isEnabled())
        self.assertFalse(self.window.custom_type.isEnabled())
        self.assertIsNotNone(built)
        assert built is not None
        self.assertEqual(built.tlv_length, 0)
        self.assertEqual(len(built.packet), 40)
        self.assertEqual(built.transmitted_tag, built.expected_tag)

    def test_cookbook_is_opt_in_and_contains_useful_recipe_catalog(self) -> None:
        self.assertFalse(self.window.cookbook_group.isChecked())
        self.assertFalse(self.window.recipe_combo.isEnabled())
        self.assertFalse(self.window.run_recipe_button.isEnabled())
        self.assertGreaterEqual(len(TEST_RECIPES), 10)
        for expected in (
            "Basic sunny day",
            "Bad day — only 2 of 10 valid",
            "Mixed bag",
            "Fully randomized",
            "Maximum TLV budget",
            "LoRa relay sunny day",
        ):
            self.assertIn(expected, TEST_RECIPES)

        self.window.cookbook_group.setChecked(True)
        with patch.object(self.window, "send_requests") as sender:
            self.window._run_recipe()
        self.assertEqual(self.window.tabs.currentWidget(), self.window.response_tab)
        sender.assert_called_once_with()

    def test_basic_recipe_configures_valid_header_only_sequence(self) -> None:
        self.window.recipe_combo.setCurrentText("Basic sunny day")
        self.window.cookbook_group.setChecked(True)
        requests = self.window._prepare_requests(10, 2.0)
        key = bytes(range(32))

        self.assertEqual(self.window.send_count.text(), "10")
        self.assertFalse(self.window.send_count.isEnabled())
        self.assertEqual(TAG_MODES[self.window.tag_mode.currentText()], "valid")
        self.assertFalse(self.window.tlv_options.isChecked())
        self.assertTrue(all(packet_hmac_valid(wrapper, key) for _, wrapper in requests))
        self.assertTrue(
            all(base64.b64decode(wrapper["payload_b64"])[31] == 0 for _, wrapper in requests)
        )

    def test_bad_day_recipe_produces_exactly_two_valid_hmacs(self) -> None:
        self.window.recipe_combo.setCurrentText("Bad day — only 2 of 10 valid")
        self.window.cookbook_group.setChecked(True)
        requests = self.window._prepare_requests(10, 1.0)
        key = bytes(range(32))

        self.assertEqual(
            sum(packet_hmac_valid(wrapper, key) for _, wrapper in requests), 2
        )

    def test_specialized_recipes_cover_maximum_tlvs_duplicates_and_ordering(self) -> None:
        self.window.recipe_combo.setCurrentText("Maximum TLV budget")
        self.window.cookbook_group.setChecked(True)
        built = self.window.build_packet()
        self.assertIsNotNone(built)
        assert built is not None
        self.assertEqual(built.tlv_length, 24)

        self.window.recipe_combo.setCurrentText("Duplicate retry storm")
        duplicates = self.window._prepare_requests(6, 1.0)
        self.assertEqual(len({wrapper["payload_b64"] for _, wrapper in duplicates}), 1)

        self.window.recipe_combo.setCurrentText("Out-of-order delivery")
        base_sequence = int(self.window.sequence.text())
        reordered = self.window._prepare_requests(6, 1.0)
        self.assertEqual(
            [sequence for sequence, _ in reordered],
            [
                base_sequence,
                (base_sequence + 1) & 0xFFFF,
                (base_sequence + 3) & 0xFFFF,
                (base_sequence + 2) & 0xFFFF,
                (base_sequence + 4) & 0xFFFF,
                (base_sequence + 5) & 0xFFFF,
            ],
        )

    def test_recipes_apply_movement_bounds_that_match_their_purpose(self) -> None:
        self.window.recipe_combo.setCurrentText("Basic sunny day")
        self.window.cookbook_group.setChecked(True)
        self.assertTrue(self.window.drift_enabled.isChecked())
        self.assertEqual(self.window.maximum_drift.text(), "50")

        self.window.recipe_combo.setCurrentText("Fully randomized")
        self.assertTrue(self.window.drift_enabled.isChecked())
        self.assertEqual(
            self.window.maximum_drift.text(), str(MAX_MOVEMENT_METRES)
        )

        for stationary_recipe in (
            "Maximum TLV budget",
            "Duplicate retry storm",
            "LTE radio fade",
            "HMAC rejection only",
        ):
            with self.subTest(recipe=stationary_recipe):
                self.window.recipe_combo.setCurrentText(stationary_recipe)
                self.assertFalse(self.window.drift_enabled.isChecked())

    def test_disabled_tlv_container_uses_full_section_muted_styling(self) -> None:
        self.assertEqual(self.window.tlv_options.objectName(), "optionalTlvGroup")
        self.assertIn("QGroupBox#optionalTlvGroup:unchecked", STYLESHEET)
        self.assertIn(
            "QGroupBox#optionalTlvGroup:unchecked QGroupBox", STYLESHEET
        )

    def test_packet_and_wrapper_previews_update_automatically(self) -> None:
        original_payload = self.window.packet_b64.toPlainText()
        self.window.latitude.setText("51.5080000")
        QTest.qWait(LIVE_PREVIEW_DELAY_MS + 50)
        self.app.processEvents()

        self.assertNotEqual(self.window.packet_b64.toPlainText(), original_payload)
        self.assertIn(
            self.window.packet_b64.toPlainText(), self.window.wrapper_preview.toPlainText()
        )
        decoded = json.loads(self.window.payload_preview.toPlainText())
        self.assertEqual(decoded["header"]["position"]["latitude"], 51.508)

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
        self.assertEqual(self.window.payload_preview.toPlainText(), "")
        self.assertEqual(self.window.wrapper_preview.toPlainText(), "")

    def test_send_forces_rebuild_before_debounce_expires(self) -> None:
        self.window.advance_packets.setChecked(False)
        self.window.send_count.setText("1")
        self.window.send_interval.setText("0")
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
        self.window.tlv_options.setChecked(True)
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
        with self.assertRaisesRegex(ValueError, "at most 300 metres"):
            drift_coordinates(*start, MAX_MOVEMENT_METRES + 1)

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

    def test_live_mode_varies_measurements_within_documented_bounds(self) -> None:
        self.window.tlv_options.setChecked(True)
        self.window.accuracy_m.setText("8")
        self.window.fix_age_s.setText("0")
        self.window.satellites.setText("9")
        with (
            patch("tlv_simulator_qt.time.time", return_value=4000),
            patch("tlv_simulator_qt.random.randint", side_effect=lambda _low, high: high),
            patch("tlv_simulator_qt.random.uniform", side_effect=lambda _low, high: high),
        ):
            requests = self.window._prepare_requests(2, 2.0)
            varied_tlvs = self.window._vary_live_tlvs(
                self.window._packet_tlvs(), 1, 2
            )

        second = self.window._prepared_fields[1]
        self.assertEqual(second.battery_mv, 3903)
        self.assertEqual(second.accuracy_m, 10)
        self.assertEqual(second.fix_age_s, 1)
        self.assertEqual(second.satellite_count, 10)
        second_wrapper = requests[1][1]
        self.assertEqual(second_wrapper["link_rssi_dbm"], -102.0)
        self.assertEqual(second_wrapper["link_snr_db"], 7.5)
        self.assertEqual(second_wrapper["cell_rsrp_dbm"], -102.0)
        self.assertEqual(second_wrapper["cell_rsrq_db"], -9.0)
        self.assertEqual(second_wrapper["cell_sinr_db"], 7.5)
        values = {entry.name: int.from_bytes(entry.value, "little") for entry in varied_tlvs}
        self.assertEqual(values["uptime_s"], 62)
        self.assertEqual(values["activity_score"], 44)

    def test_live_mode_preserves_unknown_measurement_sentinels(self) -> None:
        self.window.fix_age_s.setText("65535")
        self.window.satellites.setText("255")
        with patch("tlv_simulator_qt.time.time", return_value=4000):
            self.window._prepare_requests(2, 1.0)

        second = self.window._prepared_fields[1]
        self.assertEqual(second.fix_age_s, 65_535)
        self.assertEqual(second.satellite_count, 255)

    def test_response_columns_are_readable_and_interactively_resizable(self) -> None:
        header = self.window.response_table.horizontalHeader()
        for column in range(self.window.response_table.columnCount()):
            self.assertEqual(
                header.sectionResizeMode(column), QHeaderView.ResizeMode.Interactive
            )
        self.assertGreaterEqual(self.window.response_table.columnWidth(5), 80)
        self.window.response_table.setColumnWidth(5, 140)
        self.assertEqual(self.window.response_table.columnWidth(5), 140)

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

    def test_gui_can_append_devices_and_gateways_to_one_credentials_bundle(self) -> None:
        first_key = bytes(range(32))
        first_device = self.window._add_device_credential(
            DeviceCredential(2001, "pasted-token-2001_" + "a" * 30, first_key)
        )
        gateway = self.window._add_generated_gateway("0016", "Kitchen Hub")
        second_device = self.window._add_generated_device(2002)

        self.assertEqual(self.window.credential_combo.currentText(), "Device 2002")
        self.assertEqual(self.window.device_id.text(), "2002")
        self.assertEqual(self.window.fleet_table.rowCount(), 2)
        self.assertEqual(
            [
                self.window.fleet_table.item(row, 1).text()
                for row in range(self.window.fleet_table.rowCount())
            ],
            ["2001", "2002"],
        )
        self.assertEqual(self.window.gateway_combo.count(), 1)
        self.assertIn("Kitchen Hub", self.window.gateway_combo.currentText())

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "credentials.json"
            self.window._save_credentials_bundle(path)
            data = json.loads(path.read_text(encoding="utf-8"))

        self.assertEqual(data["schema_version"], 1)
        self.assertEqual(
            [device["device_id"] for device in data["devices"]],
            [first_device.device_id, second_device.device_id],
        )
        self.assertEqual(data["gateways"][0]["gateway_guid16"], gateway.gateway_guid16)
        self.assertEqual(data["gateways"][0]["display_name"], "Kitchen Hub")
        self.assertEqual(base64.b64decode(data["devices"][0]["hmac_key_b64"]), first_key)
        self.assertEqual(data["devices"][0]["bearer_token"], first_device.token)
        self.assertGreaterEqual(len(data["devices"][1]["bearer_token"]), 32)

    def test_typed_device_id_selects_loaded_credentials(self) -> None:
        first_key = bytes(range(32))
        second_key = bytes(reversed(range(32)))
        self.window._replace_credential_bundle(
            CredentialBundle(
                devices=(
                    DeviceCredential(2001, "a" * 48, first_key),
                    DeviceCredential(2002, "b" * 48, second_key),
                ),
                gateways=(),
            )
        )

        self.window.device_id.setText("2002")
        self.window._sync_device_credentials_from_field()

        self.assertEqual(self.window.credential_combo.currentText(), "Device 2002")
        self.assertEqual(self.window.bearer.text(), "b" * 48)
        self.assertEqual(
            self.window.hmac.text(), base64.b64encode(second_key).decode("ascii")
        )

    def test_fleet_mode_prepares_independently_signed_device_cycles(self) -> None:
        keys = [bytes((offset + index) % 256 for index in range(32)) for offset in range(3)]
        tokens = [character * 48 for character in ("a", "b", "c")]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "credentials.json"
            path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "devices": [
                            {
                                "device_id": 1001 + index,
                                "bearer_token": tokens[index],
                                "hmac_key_b64": base64.b64encode(keys[index]).decode(),
                            }
                            for index in range(3)
                        ],
                        "gateways": [
                            {
                                "gateway_guid16": "0016",
                                "display_name": "Fleet Hub",
                                "bearer_token": "g" * 48,
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

        self.window.fleet_mode.setChecked(True)
        self.window.sequence.setText("500")
        with patch("tlv_simulator_qt.time.time", return_value=5_000):
            requests = self.window._prepare_requests(2, 0)

        self.assertTrue(self.window.fleet_table.isEnabled())
        self.assertEqual(len(requests), 6)
        self.assertEqual([request.device_id for request in requests], [1001, 1002, 1003] * 2)
        self.assertEqual([request.token for request in requests], tokens * 2)
        for device_index, device_id in enumerate((1001, 1002, 1003)):
            device_requests = [request for request in requests if request.device_id == device_id]
            self.assertEqual(
                [request.sequence for request in device_requests],
                [500 + device_index, 501 + device_index],
            )
            for request in device_requests:
                packet = base64.b64decode(request.wrapper["payload_b64"], validate=True)
                self.assertEqual(int.from_bytes(packet[1:3], "little"), device_id)
                self.assertTrue(packet_hmac_valid(request.wrapper, keys[device_index]))
        first_cycle_positions = {
            (request.fields.latitude, request.fields.longitude)
            for request in requests
            if request.cycle == 0
        }
        self.assertEqual(len(first_cycle_positions), 3)

        self.window.transport.setCurrentText("LoRa home-hub relay (lora_hub)")
        with patch("tlv_simulator_qt.time.time", return_value=5_001):
            lora_requests = self.window._prepare_requests(1, 0)
        self.assertEqual({request.token for request in lora_requests}, {"g" * 48})
        self.assertEqual(
            {request.wrapper["gateway_guid16"] for request in lora_requests},
            {"0016"},
        )
        for device_index, request in enumerate(lora_requests):
            self.assertTrue(packet_hmac_valid(request.wrapper, keys[device_index]))

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

        self.assertEqual(self.window.response_table.rowCount(), 2)
        self.assertEqual(self.window.response_table.item(0, 3).text(), "201")
        self.assertIn("Created", self.window.response_table.item(0, 4).text())
        self.assertIn('"accepted": true', self.window.response_detail.toPlainText())
        self.assertIn("Completed 2 of 2 request(s).", self.window.sender_status.text())
        self.assertTrue(self.window.send_button.isEnabled())
        self.assertFalse(self.window.stop_button.isEnabled())
        sent_payloads = [call.args[2]["payload_b64"] for call in mocked_post.call_args_list]
        self.assertEqual(len(sent_payloads), 2)
        self.assertEqual(len(set(sent_payloads)), 2)

    def test_response_table_classifies_success_auth_server_and_network_results(self) -> None:
        entries = (
            {"status": 201, "request": 1, "response": {"accepted": True}},
            {"status": 401, "request": 2, "response": {"error": "invalid token"}},
            {"status": 503, "request": 3, "response": {"error": "database unavailable"}},
            {"status": 0, "request": 4, "error": "connection timed out"},
        )
        for entry in entries:
            self.window._append_response(entry)
        self.app.processEvents()

        self.assertEqual(self.window.response_table.rowCount(), 4)
        labels = [self.window.response_table.item(row, 4).text() for row in range(4)]
        self.assertIn("Created", labels[0])
        self.assertIn("Unauthorized", labels[1])
        self.assertIn("Server error", labels[2])
        self.assertIn("Network error", labels[3])
        colours = {
            self.window.response_table.item(row, 4).background().color().name()
            for row in range(4)
        }
        self.assertEqual(len(colours), 4)
        self.assertIn("connection timed out", self.window.response_detail.toPlainText())

        self.window._clear_responses()
        self.assertEqual(self.window.response_table.rowCount(), 0)
        self.assertEqual(self.window.response_detail.toPlainText(), "")


def packet_hmac_valid(wrapper: dict[str, object], key: bytes) -> bool:
    packet = base64.b64decode(str(wrapper["payload_b64"]), validate=True)
    expected = hmac.new(key, packet[:-8], hashlib.sha256).digest()[:8]
    return hmac.compare_digest(packet[-8:], expected)


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
