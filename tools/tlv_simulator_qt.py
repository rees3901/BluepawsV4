#!/usr/bin/env python3
"""Bluepaws PySide6 console for building and sending v1.1 TLV telemetry."""

from __future__ import annotations

import argparse
import base64
import json
import sys
import threading
import time
from dataclasses import replace
from pathlib import Path
from typing import Any

try:
    import PySide6
    from PySide6.QtCore import QObject, Qt, QTimer, Signal
    from PySide6.QtGui import QCloseEvent, QFont
    from PySide6.QtWidgets import (
        QApplication,
        QCheckBox,
        QComboBox,
        QFileDialog,
        QFormLayout,
        QGridLayout,
        QGroupBox,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QMainWindow,
        QMessageBox,
        QPlainTextEdit,
        QPushButton,
        QScrollArea,
        QTabWidget,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )
except ModuleNotFoundError as error:
    raise SystemExit(
        "PySide6 is required. Run: "
        "py -3.11 -m pip install -r tools\\requirements-qt-gui.txt"
    ) from error

from tlv_packet_codec import (
    DEFAULT_URL,
    FLAG_MASKS,
    POWER_PROFILE_CODES,
    STATUS_CODES,
    TX_REASON_CODES,
    BuiltPacket,
    DeviceCredential,
    PacketFields,
    TlvEntry,
    build_tlv_packet,
    build_transport_wrapper,
    custom_tlv,
    decode_hmac_key,
    firmware_tlv,
    known_tlv,
    load_credentials,
    post_wrapper,
)


REQUIRED_PYSIDE_VERSION = "6.11.1"
NAVY_DEEP = "#04111D"
NAVY = "#071827"
SURFACE = "#0D2438"
SURFACE_RAISED = "#12314A"
BORDER = "#1D4F73"
BLUE = "#1687FF"
BLUE_HOVER = "#3297FF"
TEXT = "#F4F8FC"
MUTED = "#91ABC2"
SUCCESS = "#35D0A0"
DANGER = "#FF6B7A"

STATUS_CHOICES = [f"{name} ({code})" for name, code in STATUS_CODES.items()]
PROFILE_CHOICES = [f"{name} ({code})" for name, code in POWER_PROFILE_CODES.items()]
REASON_CHOICES = [f"{name} ({code})" for name, code in TX_REASON_CODES.items()]
TAG_MODES = {
    "Valid HMAC (normal packet)": "valid",
    "Corrupt one HMAC bit (negative test)": "corrupt",
    "Custom 8-byte HMAC tag": "custom",
}
TRANSPORTS = {
    "LTE direct (cellular_direct)": "cellular_direct",
    "LoRa home-hub relay (lora_hub)": "lora_hub",
}


STYLESHEET = f"""
QWidget {{
    background-color: {NAVY_DEEP};
    color: {TEXT};
    font-family: "Segoe UI";
    font-size: 12px;
}}
QLabel, QCheckBox {{
    background: transparent;
}}
QMainWindow, QScrollArea, QScrollArea > QWidget > QWidget {{
    background-color: {NAVY_DEEP};
}}
QTabWidget::pane {{
    background: {NAVY};
    border: 1px solid {BORDER};
    border-radius: 10px;
    top: -1px;
}}
QTabBar::tab {{
    background: {SURFACE};
    color: {MUTED};
    border: 1px solid {BORDER};
    padding: 9px 22px;
    min-width: 220px;
}}
QTabBar::tab:selected {{
    background: {BLUE};
    color: white;
}}
QGroupBox {{
    background: {SURFACE};
    border: 1px solid {BORDER};
    border-radius: 10px;
    font-weight: 600;
    margin-top: 14px;
    padding: 15px 10px 10px 10px;
}}
QGroupBox::title {{
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 5px;
}}
QLineEdit, QComboBox, QPlainTextEdit, QTableWidget {{
    background: {SURFACE_RAISED};
    border: 1px solid {BORDER};
    border-radius: 5px;
    padding: 5px;
    selection-background-color: {BLUE};
}}
QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus {{
    border: 1px solid {BLUE};
}}
QComboBox::drop-down {{
    border: 0;
    width: 25px;
}}
QComboBox QAbstractItemView {{
    background: {SURFACE_RAISED};
    color: {TEXT};
    selection-background-color: {BLUE};
}}
QPushButton {{
    background: {BLUE};
    border: 0;
    border-radius: 6px;
    color: white;
    font-weight: 600;
    padding: 7px 13px;
}}
QPushButton:hover {{ background: {BLUE_HOVER}; }}
QPushButton:pressed {{ background: #0E6FCC; }}
QPushButton[secondary="true"] {{
    background: {SURFACE_RAISED};
    border: 1px solid {BORDER};
}}
QPushButton[danger="true"] {{ background: {DANGER}; }}
QCheckBox {{ spacing: 7px; }}
QCheckBox::indicator {{
    width: 16px;
    height: 16px;
}}
QHeaderView::section {{
    background: {BORDER};
    color: {TEXT};
    border: 0;
    padding: 6px;
}}
QScrollBar:vertical {{
    background: {NAVY};
    width: 12px;
    margin: 0;
}}
QScrollBar::handle:vertical {{
    background: {BORDER};
    border-radius: 5px;
    min-height: 28px;
}}
QStatusBar {{ background: {SURFACE}; color: {MUTED}; }}
"""


def line_edit(value: str = "", *, secret: bool = False) -> QLineEdit:
    field = QLineEdit(value)
    if secret:
        field.setEchoMode(QLineEdit.EchoMode.Password)
    return field


def combo(values: list[str]) -> QComboBox:
    field = QComboBox()
    field.addItems(values)
    return field


def secondary_button(text: str) -> QPushButton:
    button = QPushButton(text)
    button.setProperty("secondary", True)
    return button


class WorkerSignals(QObject):
    log_line = Signal(str)
    finished = Signal(int, int, object)


class BluepawsTlvConsole(QMainWindow):
    """Feature-complete Qt interface backed by the shared packet codec."""

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Bluepaws TLV Telemetry Test Console")
        self.resize(1220, 860)
        self.setMinimumSize(980, 700)
        self._credentials: dict[str, DeviceCredential] = {}
        self._custom_tlvs: list[TlvEntry] = []
        self._last_built: BuiltPacket | None = None
        self._stop_event = threading.Event()
        self._worker: threading.Thread | None = None
        self.worker_signals = WorkerSignals()
        self.worker_signals.log_line.connect(self._append_log)
        self.worker_signals.finished.connect(self._send_finished)
        self._build_ui()
        self._transport_changed()
        self._tag_mode_changed()
        self.statusBar().showMessage("Ready • protocol v1.1 • secrets remain on this computer")
        QTimer.singleShot(100, self.build_packet)

    def _build_ui(self) -> None:
        central = QWidget()
        outer = QVBoxLayout(central)
        outer.setContentsMargins(16, 14, 16, 14)
        outer.setSpacing(10)

        heading = QHBoxLayout()
        title = QLabel("Bluepaws TLV Test Console")
        title.setFont(QFont("Segoe UI", 20, QFont.Weight.Bold))
        heading.addWidget(title)
        heading.addStretch()
        subtitle = QLabel("Protocol v1.1 • local validation • secrets remain on this computer")
        subtitle.setStyleSheet(f"color: {MUTED};")
        heading.addWidget(subtitle)
        outer.addLayout(heading)

        tabs = QTabWidget()
        tabs.addTab(self._packet_tab(), "1. TLV Packet Builder")
        tabs.addTab(self._wrapper_tab(), "2. HTTPS Wrapper & Send")
        outer.addWidget(tabs, 1)
        self.setCentralWidget(central)

    def _packet_tab(self) -> QWidget:
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        body = QWidget()
        grid = QGridLayout(body)
        grid.setContentsMargins(10, 10, 10, 10)
        grid.setHorizontalSpacing(8)
        grid.setVerticalSpacing(8)
        grid.setColumnStretch(0, 1)
        grid.setColumnStretch(1, 1)

        credentials = QGroupBox("Provisioned test credentials")
        credential_row = QHBoxLayout(credentials)
        load = QPushButton("Load credentials JSON…")
        load.clicked.connect(self.load_credentials_file)
        credential_row.addWidget(load)
        credential_row.addWidget(QLabel("Device"))
        self.credential_combo = combo([])
        self.credential_combo.currentTextChanged.connect(self._credential_selected)
        credential_row.addWidget(self.credential_combo, 1)
        credential_row.addWidget(QLabel("Bearer and HMAC values stay masked and are never logged."))
        grid.addWidget(credentials, 0, 0, 1, 2)

        header = QGroupBox("Fixed 32-byte header")
        header_form = QFormLayout(header)
        protocol = line_edit("1")
        protocol.setEnabled(False)
        self.device_id = line_edit("1001")
        self.sequence = line_edit(str(int(time.time()) & 0xFFFF))
        self.timestamp = line_edit(str(int(time.time())))
        self.status = combo(STATUS_CHOICES)
        self.profile = combo(PROFILE_CHOICES)
        self.reason = combo(REASON_CHOICES)
        header_form.addRow("Protocol version", protocol)
        header_form.addRow("Device ID (1–65535)", self.device_id)
        header_form.addRow("Message sequence", self.sequence)
        timestamp_row = QHBoxLayout()
        timestamp_row.addWidget(self.timestamp, 1)
        timestamp_now = QPushButton("Now")
        timestamp_now.clicked.connect(self._set_now)
        timestamp_row.addWidget(timestamp_now)
        header_form.addRow("Unix timestamp", timestamp_row)
        header_form.addRow("Status", self.status)
        header_form.addRow("Power profile", self.profile)
        header_form.addRow("TX reason", self.reason)
        grid.addWidget(header, 1, 0)

        position = QGroupBox("Position and telemetry")
        position_form = QFormLayout(position)
        self.latitude = line_edit("51.907055")
        self.longitude = line_edit("-2.256660")
        self.battery_mv = line_edit("3900")
        self.accuracy_m = line_edit("8")
        self.fix_age_s = line_edit("0")
        self.satellites = line_edit("9")
        for label, field in (
            ("Latitude", self.latitude),
            ("Longitude", self.longitude),
            ("Battery (mV)", self.battery_mv),
            ("Accuracy (m)", self.accuracy_m),
            ("Fix age (s)", self.fix_age_s),
            ("Satellite count", self.satellites),
        ):
            position_form.addRow(label, field)
        position_form.addRow(QLabel("Use fix age 65535 or satellites 255 for unknown."))
        grid.addWidget(position, 1, 1)

        flags = QGroupBox("Header flags")
        flag_grid = QGridLayout(flags)
        self.flag_checks: dict[str, QCheckBox] = {}
        for index, (name, mask) in enumerate(FLAG_MASKS.items()):
            flag = QCheckBox(f"{name} (0x{mask:02X})")
            flag.setChecked(name in ("GNSS_VALID", "FIX_3D"))
            self.flag_checks[name] = flag
            flag_grid.addWidget(flag, index // 4, index % 4)
        grid.addWidget(flags, 2, 0, 1, 2)

        known = QGroupBox("Selected v1.1 TLVs (24-byte total budget)")
        known_grid = QGridLayout(known)
        known_grid.addWidget(QLabel("Enabled"), 0, 0)
        known_grid.addWidget(QLabel("Value"), 0, 1)
        known_grid.addWidget(QLabel("Range / format"), 0, 2)
        specs = [
            ("fw_ver", "0x04", "1.1", "major.minor", True),
            ("reset_reason", "0x06", "0", "0–255", False),
            ("uptime_s", "0x10", "60", "0–4294967295", True),
            ("activity_score", "0x13", "42", "0–255", True),
            ("acked_msg_seq_id", "0x20", "0", "0–65535", False),
        ]
        self.known_checks: dict[str, QCheckBox] = {}
        self.known_values: dict[str, QLineEdit] = {}
        for row, (name, code, value, hint, enabled) in enumerate(specs, start=1):
            check = QCheckBox(f"{code}  {name}")
            check.setChecked(enabled)
            value_field = line_edit(value)
            self.known_checks[name] = check
            self.known_values[name] = value_field
            known_grid.addWidget(check, row, 0)
            known_grid.addWidget(value_field, row, 1)
            known_grid.addWidget(QLabel(hint), row, 2)
        grid.addWidget(known, 3, 0)

        custom = QGroupBox("Custom / unknown TLVs")
        custom_layout = QVBoxLayout(custom)
        custom_row = QHBoxLayout()
        self.custom_type = line_edit("7E")
        self.custom_value = line_edit("010203")
        add = QPushButton("Add TLV")
        add.clicked.connect(self._add_custom_tlv)
        custom_row.addWidget(QLabel("Type (hex)"))
        custom_row.addWidget(self.custom_type)
        custom_row.addWidget(QLabel("Value bytes (hex)"))
        custom_row.addWidget(self.custom_value, 1)
        custom_row.addWidget(add)
        custom_layout.addLayout(custom_row)
        self.custom_table = QTableWidget(0, 3)
        self.custom_table.setHorizontalHeaderLabels(["Type", "Bytes", "Value hex"])
        self.custom_table.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        self.custom_table.setMinimumHeight(130)
        custom_layout.addWidget(self.custom_table)
        remove = secondary_button("Remove selected")
        remove.clicked.connect(self._remove_custom_tlv)
        custom_layout.addWidget(remove, alignment=Qt.AlignmentFlag.AlignRight)
        grid.addWidget(custom, 3, 1)

        authentication = QGroupBox("HMAC-SHA256 authentication (first 8 bytes)")
        authentication_grid = QGridLayout(authentication)
        self.hmac = line_edit("", secret=True)
        show_hmac = QCheckBox("Show")
        show_hmac.toggled.connect(
            lambda checked: self.hmac.setEchoMode(
                QLineEdit.EchoMode.Normal if checked else QLineEdit.EchoMode.Password
            )
        )
        authentication_grid.addWidget(QLabel("32-byte key (Base64)"), 0, 0)
        authentication_grid.addWidget(self.hmac, 0, 1)
        authentication_grid.addWidget(show_hmac, 0, 2)
        authentication_grid.addWidget(QLabel("Tag mode"), 1, 0)
        self.tag_mode = combo(list(TAG_MODES))
        self.tag_mode.currentTextChanged.connect(self._tag_mode_changed)
        self.custom_tag = line_edit("0000000000000000")
        authentication_grid.addWidget(self.tag_mode, 1, 1)
        authentication_grid.addWidget(self.custom_tag, 1, 2)
        grid.addWidget(authentication, 4, 0, 1, 2)

        output = QGroupBox("Packet output")
        output_layout = QVBoxLayout(output)
        output_actions = QHBoxLayout()
        build = QPushButton("Build and validate packet")
        build.clicked.connect(self.build_packet)
        output_actions.addWidget(build)
        self.packet_summary = QLabel("Packet not built")
        self.packet_summary.setStyleSheet(f"color: {SUCCESS};")
        output_actions.addWidget(self.packet_summary)
        output_actions.addStretch()
        copy_base64 = secondary_button("Copy Base64")
        copy_base64.clicked.connect(lambda: self._copy_text(self.packet_b64))
        output_actions.addWidget(copy_base64)
        copy_hex = secondary_button("Copy hex")
        copy_hex.clicked.connect(lambda: self._copy_text(self.packet_hex))
        output_actions.addWidget(copy_hex)
        output_layout.addLayout(output_actions)
        self.builder_status = QLabel("Load credentials or enter a Base64 HMAC key.")
        self.builder_status.setStyleSheet(f"color: {MUTED};")
        output_layout.addWidget(self.builder_status)
        output_layout.addWidget(QLabel("Packet Base64"))
        self.packet_b64 = QPlainTextEdit()
        self.packet_b64.setMaximumHeight(75)
        output_layout.addWidget(self.packet_b64)
        output_layout.addWidget(QLabel("Packet hex"))
        self.packet_hex = QPlainTextEdit()
        self.packet_hex.setMaximumHeight(100)
        output_layout.addWidget(self.packet_hex)
        grid.addWidget(output, 5, 0, 1, 2)

        scroll.setWidget(body)
        return scroll

    def _wrapper_tab(self) -> QWidget:
        body = QWidget()
        grid = QGridLayout(body)
        grid.setContentsMargins(10, 10, 10, 10)
        grid.setSpacing(8)
        grid.setColumnStretch(0, 1)
        grid.setColumnStretch(1, 1)
        grid.setRowStretch(1, 1)

        request = QGroupBox("HTTPS request")
        request_form = QFormLayout(request)
        self.endpoint = line_edit(DEFAULT_URL)
        self.transport = combo(list(TRANSPORTS))
        self.transport.currentTextChanged.connect(self._transport_changed)
        request_form.addRow("Supabase endpoint", self.endpoint)
        request_form.addRow("Transport", self.transport)
        self.bearer = line_edit("", secret=True)
        show_bearer = QCheckBox("Show bearer token")
        show_bearer.toggled.connect(
            lambda checked: self.bearer.setEchoMode(
                QLineEdit.EchoMode.Normal if checked else QLineEdit.EchoMode.Password
            )
        )
        request_form.addRow("Bearer token", self.bearer)
        request_form.addRow("", show_bearer)
        grid.addWidget(request, 0, 0)

        metadata = QGroupBox("Transport metadata")
        metadata_form = QFormLayout(metadata)
        self.gateway_guid = line_edit("0016")
        self.gateway_rx_time = line_edit(str(int(time.time())))
        self.link_rssi = line_edit("-104")
        self.link_snr = line_edit("7.0")
        self.cell_rsrp = line_edit("-104")
        self.cell_rsrq = line_edit("-9.5")
        self.cell_sinr = line_edit("7.0")
        self.gateway_now = QPushButton("Now")
        self.gateway_now.clicked.connect(self._set_gateway_now)
        gateway_time_row = QHBoxLayout()
        gateway_time_row.addWidget(self.gateway_rx_time, 1)
        gateway_time_row.addWidget(self.gateway_now)
        metadata_form.addRow("Gateway GUID16", self.gateway_guid)
        metadata_form.addRow("Gateway RX Unix", gateway_time_row)
        metadata_form.addRow("Link RSSI (dBm)", self.link_rssi)
        metadata_form.addRow("Link SNR (dB)", self.link_snr)
        metadata_form.addRow("LTE RSRP (dBm)", self.cell_rsrp)
        metadata_form.addRow("LTE RSRQ (dB)", self.cell_rsrq)
        metadata_form.addRow("LTE SINR (dB)", self.cell_sinr)
        self.gateway_widgets = [self.gateway_guid, self.gateway_rx_time, self.gateway_now]
        self.cell_widgets = [self.cell_rsrp, self.cell_rsrq, self.cell_sinr]
        grid.addWidget(metadata, 0, 1)

        preview = QGroupBox("JSON wrapper preview")
        preview_layout = QVBoxLayout(preview)
        preview_actions = QHBoxLayout()
        refresh = QPushButton("Refresh preview")
        refresh.clicked.connect(self.preview_wrapper)
        preview_actions.addWidget(refresh)
        copy_json = secondary_button("Copy JSON")
        copy_json.clicked.connect(lambda: self._copy_text(self.wrapper_preview))
        preview_actions.addWidget(copy_json)
        preview_actions.addStretch()
        preview_layout.addLayout(preview_actions)
        self.wrapper_preview = QPlainTextEdit()
        preview_layout.addWidget(self.wrapper_preview)
        grid.addWidget(preview, 1, 0)

        sender = QGroupBox("Send and response log")
        sender_layout = QVBoxLayout(sender)
        controls = QHBoxLayout()
        self.send_count = line_edit("1")
        self.send_count.setMaximumWidth(72)
        self.send_interval = line_edit("1")
        self.send_interval.setMaximumWidth(72)
        self.timeout = line_edit("15")
        self.timeout.setMaximumWidth(72)
        for label, field in (
            ("Count", self.send_count),
            ("Interval (s)", self.send_interval),
            ("Timeout (s)", self.timeout),
        ):
            controls.addWidget(QLabel(label))
            controls.addWidget(field)
        controls.addStretch()
        sender_layout.addLayout(controls)
        self.advance_packets = QCheckBox("Advance sequence and timestamp for each request")
        sender_layout.addWidget(self.advance_packets)
        buttons = QHBoxLayout()
        self.send_button = QPushButton("Send")
        self.send_button.clicked.connect(self.send_requests)
        self.stop_button = QPushButton("Stop")
        self.stop_button.setProperty("danger", True)
        self.stop_button.setEnabled(False)
        self.stop_button.clicked.connect(self.stop_sending)
        clear = secondary_button("Clear log")
        clear.clicked.connect(lambda: self.response_log.clear())
        buttons.addWidget(self.send_button)
        buttons.addWidget(self.stop_button)
        buttons.addStretch()
        buttons.addWidget(clear)
        sender_layout.addLayout(buttons)
        self.sender_status = QLabel("Build a packet, preview the wrapper, then send.")
        self.sender_status.setStyleSheet(f"color: {MUTED};")
        sender_layout.addWidget(self.sender_status)
        self.response_log = QPlainTextEdit()
        self.response_log.setPlaceholderText("No requests sent yet.")
        sender_layout.addWidget(self.response_log, 1)
        grid.addWidget(sender, 1, 1)
        return body

    def load_credentials_file(self) -> None:
        selected, _ = QFileDialog.getOpenFileName(
            self, "Load Bluepaws test credentials", "", "JSON files (*.json);;All files (*.*)"
        )
        if not selected:
            return
        try:
            credentials = load_credentials(Path(selected))
        except (OSError, ValueError, json.JSONDecodeError) as error:
            QMessageBox.critical(self, "Unable to load credentials", str(error))
            return
        self._credentials = {f"Device {item.device_id}": item for item in credentials}
        self.credential_combo.blockSignals(True)
        self.credential_combo.clear()
        self.credential_combo.addItems(list(self._credentials))
        self.credential_combo.blockSignals(False)
        self.credential_combo.setCurrentIndex(0)
        self._apply_credential(credentials[0])
        self.builder_status.setText(f"Loaded {len(credentials)} provisioned test device(s).")

    def _credential_selected(self, name: str) -> None:
        credential = self._credentials.get(name)
        if credential is not None:
            self._apply_credential(credential)

    def _apply_credential(self, credential: DeviceCredential) -> None:
        self.device_id.setText(str(credential.device_id))
        self.hmac.setText(base64.b64encode(credential.hmac_key).decode("ascii"))
        self.bearer.setText(credential.token)
        self.builder_status.setText(
            f"Device {credential.device_id} selected. Secrets remain masked."
        )
        self.build_packet()

    def _set_now(self) -> None:
        self.timestamp.setText(str(int(time.time())))

    def _set_gateway_now(self) -> None:
        self.gateway_rx_time.setText(str(int(time.time())))

    def _tag_mode_changed(self) -> None:
        self.custom_tag.setEnabled(TAG_MODES[self.tag_mode.currentText()] == "custom")

    def _transport_changed(self) -> None:
        is_lora = TRANSPORTS[self.transport.currentText()] == "lora_hub"
        for widget in self.gateway_widgets:
            widget.setEnabled(is_lora)
        for widget in self.cell_widgets:
            widget.setEnabled(not is_lora)
        self.preview_wrapper()

    def _add_custom_tlv(self) -> None:
        try:
            entry = custom_tlv(self.custom_type.text(), self.custom_value.text())
        except ValueError as error:
            QMessageBox.critical(self, "Invalid custom TLV", str(error))
            return
        row = self.custom_table.rowCount()
        self.custom_table.insertRow(row)
        self.custom_table.setItem(row, 0, QTableWidgetItem(f"0x{entry.tlv_type:02X}"))
        self.custom_table.setItem(row, 1, QTableWidgetItem(str(len(entry.value))))
        self.custom_table.setItem(row, 2, QTableWidgetItem(entry.value.hex().upper()))
        self._custom_tlvs.append(entry)
        self.build_packet()

    def _remove_custom_tlv(self) -> None:
        rows = sorted({item.row() for item in self.custom_table.selectedItems()}, reverse=True)
        for row in rows:
            self._custom_tlvs.pop(row)
            self.custom_table.removeRow(row)
        self.build_packet()

    def _packet_fields(self) -> PacketFields:
        flags = sum(
            mask for name, mask in FLAG_MASKS.items() if self.flag_checks[name].isChecked()
        )
        return PacketFields(
            device_id=self._int(self.device_id.text(), "device ID"),
            message_sequence=self._int(self.sequence.text(), "message sequence"),
            timestamp=self._int(self.timestamp.text(), "Unix timestamp"),
            status=self._choice_code(self.status.currentText()),
            power_profile=self._choice_code(self.profile.currentText()),
            flags=flags,
            tx_reason=self._choice_code(self.reason.currentText()),
            latitude=self._float(self.latitude.text(), "latitude"),
            longitude=self._float(self.longitude.text(), "longitude"),
            battery_mv=self._int(self.battery_mv.text(), "battery millivolts"),
            accuracy_m=self._int(self.accuracy_m.text(), "accuracy metres"),
            fix_age_s=self._int(self.fix_age_s.text(), "fix age seconds"),
            satellite_count=self._int(self.satellites.text(), "satellite count"),
        )

    def _packet_tlvs(self) -> list[TlvEntry]:
        result: list[TlvEntry] = []
        if self.known_checks["fw_ver"].isChecked():
            result.append(firmware_tlv(self.known_values["fw_ver"].text()))
        for name, tlv_type in (
            ("reset_reason", 0x06),
            ("uptime_s", 0x10),
            ("activity_score", 0x13),
            ("acked_msg_seq_id", 0x20),
        ):
            if self.known_checks[name].isChecked():
                result.append(known_tlv(tlv_type, self._int(self.known_values[name].text(), name)))
        result.extend(self._custom_tlvs)
        return result

    def _build_from_fields(self, fields: PacketFields | None = None) -> BuiltPacket:
        return build_tlv_packet(
            fields or self._packet_fields(),
            self._packet_tlvs(),
            decode_hmac_key(self.hmac.text()),
            tag_mode=TAG_MODES[self.tag_mode.currentText()],
            custom_tag_hex=self.custom_tag.text(),
        )

    def build_packet(self) -> BuiltPacket | None:
        try:
            built = self._build_from_fields()
        except ValueError as error:
            self._last_built = None
            self.packet_summary.setText("Packet not built")
            self.builder_status.setText(str(error))
            return None
        self._last_built = built
        self.packet_b64.setPlainText(built.payload_b64)
        self.packet_hex.setPlainText(self._group_hex(built.packet_hex))
        tag_note = "valid" if built.transmitted_tag == built.expected_tag else "intentionally invalid"
        self.packet_summary.setText(
            f"{len(built.packet)} bytes • TLVs {built.tlv_length}/24 bytes • "
            f"tag {built.transmitted_tag.hex().upper()} ({tag_note}) • "
            f"SHA-256 {built.payload_hash[:16]}…"
        )
        self.builder_status.setText("Packet passes local structure validation.")
        self.preview_wrapper()
        return built

    def _wrapper(
        self,
        payload_b64: str | None = None,
        *,
        gateway_timestamp: int | None = None,
    ) -> dict[str, Any]:
        transport = TRANSPORTS[self.transport.currentText()]
        payload = payload_b64 if payload_b64 is not None else self.packet_b64.toPlainText().strip()
        common = {
            "link_rssi_dbm": self._optional_float(self.link_rssi.text(), "link RSSI"),
            "link_snr_db": self._optional_float(self.link_snr.text(), "link SNR"),
        }
        if transport == "lora_hub":
            return build_transport_wrapper(
                payload,
                transport,
                gateway_guid16=self.gateway_guid.text(),
                gateway_rx_time_unix=(
                    gateway_timestamp
                    if gateway_timestamp is not None
                    else self._int(self.gateway_rx_time.text(), "gateway receive timestamp")
                ),
                **common,
            )
        return build_transport_wrapper(
            payload,
            transport,
            cell_rsrp_dbm=self._optional_float(self.cell_rsrp.text(), "LTE RSRP"),
            cell_rsrq_db=self._optional_float(self.cell_rsrq.text(), "LTE RSRQ"),
            cell_sinr_db=self._optional_float(self.cell_sinr.text(), "LTE SINR"),
            **common,
        )

    def preview_wrapper(self) -> dict[str, Any] | None:
        try:
            wrapper = self._wrapper()
        except ValueError as error:
            self.sender_status.setText(str(error))
            return None
        self.wrapper_preview.setPlainText(json.dumps(wrapper, indent=2))
        self.sender_status.setText(
            "Wrapper passes local validation. Authorization header is not shown."
        )
        return wrapper

    def send_requests(self) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        try:
            count = self._int(self.send_count.text(), "send count")
            if not 1 <= count <= 1000:
                raise ValueError("send count must be from 1 to 1000")
            interval = self._float(self.send_interval.text(), "send interval")
            timeout = self._float(self.timeout.text(), "HTTP timeout")
            if not 0 <= interval <= 3600:
                raise ValueError("send interval must be from 0 to 3600 seconds")
            if not 0 < timeout <= 300:
                raise ValueError("HTTP timeout must be from 0 to 300 seconds")
            endpoint = self.endpoint.text().strip()
            token = self.bearer.text().strip()
            requests = self._prepare_requests(count, interval)
            if not endpoint.lower().startswith("https://"):
                raise ValueError("Supabase endpoint must use HTTPS")
            if not 32 <= len(token) <= 256:
                raise ValueError("bearer token must contain 32..256 characters")
        except ValueError as error:
            QMessageBox.critical(self, "Cannot send", str(error))
            return

        self._stop_event.clear()
        self.send_button.setEnabled(False)
        self.stop_button.setEnabled(True)
        self.sender_status.setText(f"Sending {count} request(s)…")
        self._worker = threading.Thread(
            target=self._send_worker,
            args=(endpoint, token, requests, interval, timeout),
            daemon=True,
        )
        self._worker.start()

    def _prepare_requests(
        self, count: int, interval: float
    ) -> list[tuple[int, dict[str, Any]]]:
        if not self.advance_packets.isChecked():
            wrapper = self._wrapper()
            sequence = self._int(self.sequence.text(), "message sequence")
            return [(sequence, wrapper) for _ in range(count)]

        base_fields = self._packet_fields()
        hmac_key = decode_hmac_key(self.hmac.text())
        tlvs = self._packet_tlvs()
        mode = TAG_MODES[self.tag_mode.currentText()]
        base_gateway_time = (
            self._int(self.gateway_rx_time.text(), "gateway receive timestamp")
            if TRANSPORTS[self.transport.currentText()] == "lora_hub"
            else None
        )
        result: list[tuple[int, dict[str, Any]]] = []
        for index in range(count):
            offset = round(index * interval)
            fields = replace(
                base_fields,
                message_sequence=(base_fields.message_sequence + index) & 0xFFFF,
                timestamp=min(0xFFFF_FFFF, base_fields.timestamp + offset),
            )
            built = build_tlv_packet(
                fields,
                tlvs,
                hmac_key,
                tag_mode=mode,
                custom_tag_hex=self.custom_tag.text(),
            )
            gateway_time = (
                min(0xFFFF_FFFF, base_gateway_time + offset)
                if base_gateway_time is not None
                else None
            )
            result.append(
                (fields.message_sequence, self._wrapper(built.payload_b64, gateway_timestamp=gateway_time))
            )
        return result

    def _send_worker(
        self,
        endpoint: str,
        token: str,
        requests: list[tuple[int, dict[str, Any]]],
        interval: float,
        timeout: float,
    ) -> None:
        completed = 0
        for index, (sequence, wrapper) in enumerate(requests, start=1):
            if self._stop_event.is_set():
                break
            started = time.monotonic()
            try:
                status, response = post_wrapper(
                    endpoint,
                    token,
                    wrapper,
                    timeout,
                    user_agent="bluepaws-tlv-qt-console/1",
                )
                line = json.dumps(
                    {
                        "request": index,
                        "status": status,
                        "message_sequence": sequence,
                        "elapsed_ms": round((time.monotonic() - started) * 1000),
                        "response": response,
                    },
                    separators=(",", ":"),
                )
            except (OSError, TimeoutError, ValueError) as error:
                line = json.dumps(
                    {
                        "request": index,
                        "status": 0,
                        "message_sequence": sequence,
                        "error": str(error),
                    },
                    separators=(",", ":"),
                )
            completed += 1
            self.worker_signals.log_line.emit(line)
            if index < len(requests) and self._stop_event.wait(interval):
                break
        self.worker_signals.finished.emit(completed, len(requests), requests)

    def _send_finished(
        self,
        completed: int,
        total: int,
        requests: object,
    ) -> None:
        request_list = list(requests)  # type: ignore[arg-type]
        self.send_button.setEnabled(True)
        self.stop_button.setEnabled(False)
        stopped = completed < total
        self.sender_status.setText(
            f"{'Stopped after' if stopped else 'Completed'} {completed} of {total} request(s)."
        )
        if completed and self.advance_packets.isChecked():
            self.sequence.setText(str((request_list[completed - 1][0] + 1) & 0xFFFF))
            self._set_now()
        self._worker = None

    def stop_sending(self) -> None:
        self._stop_event.set()
        self.sender_status.setText("Stopping after the current request…")

    def _append_log(self, line: str) -> None:
        self.response_log.appendPlainText(f"[{time.strftime('%H:%M:%S')}] {line}")

    @staticmethod
    def _copy_text(widget: QPlainTextEdit) -> None:
        value = widget.toPlainText().strip()
        clipboard = QApplication.clipboard()
        if value and clipboard is not None:
            clipboard.setText(value)

    @staticmethod
    def _group_hex(value: str) -> str:
        return " ".join(value[index : index + 2] for index in range(0, len(value), 2))

    @staticmethod
    def _choice_code(value: str) -> int:
        try:
            return int(value.rsplit("(", 1)[1].rstrip(")"))
        except (IndexError, ValueError) as error:
            raise ValueError(f"invalid enum selection: {value}") from error

    @staticmethod
    def _int(value: str, field: str) -> int:
        try:
            return int(value.strip(), 10)
        except ValueError as error:
            raise ValueError(f"{field} must be a decimal integer") from error

    @staticmethod
    def _float(value: str, field: str) -> float:
        try:
            return float(value.strip())
        except ValueError as error:
            raise ValueError(f"{field} must be a number") from error

    @classmethod
    def _optional_float(cls, value: str, field: str) -> float | None:
        return None if not value.strip() else cls._float(value, field)

    def closeEvent(self, event: QCloseEvent) -> None:  # noqa: N802 - Qt override name
        self._stop_event.set()
        event.accept()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Bluepaws PySide6 TLV desktop console")
    parser.add_argument("--check", action="store_true", help="check imports without opening a window")
    parser.add_argument(
        "--auto-close-ms",
        type=int,
        default=0,
        help="close automatically after this many milliseconds (test automation)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if PySide6.__version__ != REQUIRED_PYSIDE_VERSION:
        raise SystemExit(
            f"This console requires PySide6 {REQUIRED_PYSIDE_VERSION}; "
            f"found {PySide6.__version__}."
        )
    if args.check:
        print(f"Bluepaws Qt console dependencies available (PySide6 {PySide6.__version__}).")
        return 0

    app = QApplication(sys.argv[:1])
    app.setApplicationName("Bluepaws TLV Console")
    app.setStyle("Fusion")
    app.setStyleSheet(STYLESHEET)
    window = BluepawsTlvConsole()
    window.show()
    if args.auto_close_ms > 0:
        QTimer.singleShot(args.auto_close_ms, app.quit)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
