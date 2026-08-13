#!/usr/bin/env python3
"""Representative PySide6 workload used to validate Windows drag performance."""

from __future__ import annotations

import argparse
import json
import sys
import time

try:
    import PySide6
    from PySide6.QtCore import Qt, QTimer
    from PySide6.QtGui import QFont
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
        QSizePolicy,
        QSpinBox,
        QTabWidget,
        QTableWidget,
        QTableWidgetItem,
        QVBoxLayout,
        QWidget,
    )
except ModuleNotFoundError as error:
    raise SystemExit(
        "PySide6 is required. Run: "
        "py -3.11 -m pip install -r tools\\requirements-qt-prototype.txt"
    ) from error


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

STATUS_CHOICES = ["normal (0)", "alert (1)", "lost (2)", "emergency (3)"]
PROFILE_CHOICES = ["normal (0)", "power_save (1)", "performance (2)"]
REASON_CHOICES = ["scheduled (0)", "movement (1)", "button (2)", "alert (3)"]
FLAG_NAMES = [
    "GNSS_VALID (0x01)",
    "FIX_3D (0x02)",
    "CHARGING (0x04)",
    "HOME (0x08)",
    "GEOFENCE (0x10)",
    "LOW_BATTERY (0x20)",
    "MOTION (0x40)",
    "RESERVED (0x80)",
]


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
QLineEdit, QComboBox, QSpinBox, QPlainTextEdit, QTableWidget {{
    background: {SURFACE_RAISED};
    border: 1px solid {BORDER};
    border-radius: 5px;
    padding: 5px;
    selection-background-color: {BLUE};
}}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QPlainTextEdit:focus {{
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


class QtDragPrototype(QMainWindow):
    """A realistic widget load, intentionally separate from production simulator logic."""

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Bluepaws TLV Console — PySide6 Drag Prototype")
        self.resize(1220, 860)
        self.setMinimumSize(980, 700)
        self._build_ui()
        widget_count = len(self.findChildren(QWidget))
        self.statusBar().showMessage(
            f"Drag-performance prototype • {widget_count} Qt widgets • no network requests are sent"
        )

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
        subtitle = QLabel("PySide6 validation • representative widget workload • no secrets")
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
        load.clicked.connect(self._choose_credentials)
        credential_row.addWidget(load)
        credential_row.addWidget(QLabel("Device"))
        credential_row.addWidget(combo(["Device 1001", "Device 1002", "Device 1003"]), 1)
        credential_row.addWidget(QLabel("Prototype values only; no credentials are loaded."))
        grid.addWidget(credentials, 0, 0, 1, 2)

        header = QGroupBox("Fixed 32-byte header")
        header_form = QFormLayout(header)
        header_form.addRow("Protocol version", line_edit("1"))
        header_form.addRow("Device ID (1–65535)", line_edit("1001"))
        header_form.addRow("Message sequence", line_edit(str(int(time.time()) & 0xFFFF)))
        header_form.addRow("Unix timestamp", line_edit(str(int(time.time()))))
        header_form.addRow("Status", combo(STATUS_CHOICES))
        header_form.addRow("Power profile", combo(PROFILE_CHOICES))
        header_form.addRow("TX reason", combo(REASON_CHOICES))
        grid.addWidget(header, 1, 0)

        position = QGroupBox("Position and telemetry")
        position_form = QFormLayout(position)
        for label, value in (
            ("Latitude", "51.907055"),
            ("Longitude", "-2.256660"),
            ("Battery (mV)", "3900"),
            ("Accuracy (m)", "8"),
            ("Fix age (s)", "0"),
            ("Satellite count", "9"),
        ):
            position_form.addRow(label, line_edit(value))
        position_form.addRow(QLabel("Use fix age 65535 or satellites 255 for unknown."))
        grid.addWidget(position, 1, 1)

        flags = QGroupBox("Header flags")
        flag_grid = QGridLayout(flags)
        for index, name in enumerate(FLAG_NAMES):
            flag = QCheckBox(name)
            flag.setChecked(index < 2)
            flag_grid.addWidget(flag, index // 4, index % 4)
        grid.addWidget(flags, 2, 0, 1, 2)

        known = QGroupBox("Selected v1.1 TLVs (24-byte total budget)")
        known_grid = QGridLayout(known)
        known_grid.addWidget(QLabel("Enabled"), 0, 0)
        known_grid.addWidget(QLabel("Value"), 0, 1)
        known_grid.addWidget(QLabel("Range / format"), 0, 2)
        specs = [
            ("0x04  fw_ver", "1.1", "major.minor", True),
            ("0x06  reset_reason", "0", "0–255", False),
            ("0x10  uptime_s", "60", "0–4294967295", True),
            ("0x13  activity_score", "42", "0–255", True),
            ("0x20  acked_msg_seq_id", "0", "0–65535", False),
        ]
        for row, (name, value, hint, enabled) in enumerate(specs, start=1):
            check = QCheckBox(name)
            check.setChecked(enabled)
            known_grid.addWidget(check, row, 0)
            known_grid.addWidget(line_edit(value), row, 1)
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
        self.hmac = line_edit("prototype-key-is-not-a-real-secret", secret=True)
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
        authentication_grid.addWidget(
            combo(["Valid HMAC", "Corrupt one HMAC bit", "Custom 8-byte tag"]), 1, 1
        )
        authentication_grid.addWidget(line_edit("0000000000000000"), 1, 2)
        grid.addWidget(authentication, 4, 0, 1, 2)

        output = QGroupBox("Packet output")
        output_layout = QVBoxLayout(output)
        output_actions = QHBoxLayout()
        build = QPushButton("Build representative packet")
        build.clicked.connect(self._build_preview)
        output_actions.addWidget(build)
        output_actions.addWidget(QLabel("40 bytes • TLV budget 16 / 24 • prototype only"))
        output_actions.addStretch()
        output_actions.addWidget(secondary_button("Copy Base64"))
        output_actions.addWidget(secondary_button("Copy hex"))
        output_layout.addLayout(output_actions)
        self.packet_output = QPlainTextEdit()
        self.packet_output.setPlainText(
            "AQPoFcFmaJxQAAADGuAUAHq4FQAADDU8CAgQPAQAAAP9mYXRlLXByb3RvdHlwZQ==\n\n"
            "01 03 E8 15 C1 66 68 9C 50 00 00 03 0A E0 14 00 7A B8 15 00 00 0C 35 3C"
        )
        self.packet_output.setMinimumHeight(130)
        output_layout.addWidget(self.packet_output)
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
        request_form.addRow(
            "Supabase endpoint",
            line_edit("https://example.supabase.co/functions/v1/ingest-position"),
        )
        request_form.addRow(
            "Transport", combo(["LTE direct (cellular_direct)", "LoRa home-hub relay (lora_hub)"])
        )
        self.bearer = line_edit("prototype-token-is-not-real", secret=True)
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
        for label, value in (
            ("Gateway GUID16", "0016"),
            ("Gateway RX Unix", str(int(time.time()))),
            ("Link RSSI (dBm)", "-104"),
            ("Link SNR (dB)", "7.0"),
            ("LTE RSRP (dBm)", "-104"),
            ("LTE RSRQ (dB)", "-9.5"),
            ("LTE SINR (dB)", "7.0"),
        ):
            metadata_form.addRow(label, line_edit(value))
        grid.addWidget(metadata, 0, 1)

        preview = QGroupBox("JSON wrapper preview")
        preview_layout = QVBoxLayout(preview)
        preview_actions = QHBoxLayout()
        refresh = QPushButton("Refresh preview")
        refresh.clicked.connect(self._refresh_wrapper)
        preview_actions.addWidget(refresh)
        preview_actions.addWidget(secondary_button("Copy JSON"))
        preview_actions.addStretch()
        preview_layout.addLayout(preview_actions)
        self.wrapper_preview = QPlainTextEdit()
        self.wrapper_preview.setPlainText(self._wrapper_json())
        preview_layout.addWidget(self.wrapper_preview)
        grid.addWidget(preview, 1, 0)

        sender = QGroupBox("Send and response log")
        sender_layout = QVBoxLayout(sender)
        controls = QHBoxLayout()
        for label, value, maximum in (
            ("Count", 1, 10000),
            ("Interval (s)", 1, 3600),
            ("Timeout (s)", 15, 120),
        ):
            controls.addWidget(QLabel(label))
            spin = QSpinBox()
            spin.setRange(0 if label != "Count" else 1, maximum)
            spin.setValue(value)
            controls.addWidget(spin)
        sender_layout.addLayout(controls)
        sender_layout.addWidget(QCheckBox("Advance sequence and timestamp for each request"))
        buttons = QHBoxLayout()
        send = QPushButton("Simulate send")
        send.clicked.connect(self._simulate_send)
        stop = QPushButton("Stop")
        stop.setProperty("danger", True)
        clear = secondary_button("Clear log")
        clear.clicked.connect(lambda: self.response_log.clear())
        buttons.addWidget(send)
        buttons.addWidget(stop)
        buttons.addStretch()
        buttons.addWidget(clear)
        sender_layout.addLayout(buttons)
        self.response_log = QPlainTextEdit()
        self.response_log.setPlaceholderText("No requests sent. This is a drag prototype.")
        sender_layout.addWidget(self.response_log, 1)
        grid.addWidget(sender, 1, 1)
        return body

    def _choose_credentials(self) -> None:
        selected, _ = QFileDialog.getOpenFileName(
            self, "Choose a credentials file (prototype does not read it)", "", "JSON files (*.json)"
        )
        if selected:
            QMessageBox.information(
                self,
                "Prototype only",
                "The file chooser is working. This validation build deliberately did not read the file.",
            )

    def _add_custom_tlv(self) -> None:
        value = self.custom_value.text().strip()
        row = self.custom_table.rowCount()
        self.custom_table.insertRow(row)
        self.custom_table.setItem(row, 0, QTableWidgetItem(self.custom_type.text().strip()))
        self.custom_table.setItem(row, 1, QTableWidgetItem(str(len(value) // 2)))
        self.custom_table.setItem(row, 2, QTableWidgetItem(value))

    def _remove_custom_tlv(self) -> None:
        rows = sorted({item.row() for item in self.custom_table.selectedItems()}, reverse=True)
        for row in rows:
            self.custom_table.removeRow(row)

    def _build_preview(self) -> None:
        self.packet_output.appendPlainText(
            f"\n[{time.strftime('%H:%M:%S')}] Representative packet rebuilt successfully."
        )

    @staticmethod
    def _wrapper_json() -> str:
        return json.dumps(
            {
                "transport": "cellular_direct",
                "received_at": int(time.time()),
                "payload_b64": "AQPoFcFmaJxQAAADGuAUAHq4FQAADDU8...",
                "cell": {"rsrp_dbm": -104, "rsrq_db": -9.5, "sinr_db": 7.0},
            },
            indent=2,
        )

    def _refresh_wrapper(self) -> None:
        self.wrapper_preview.setPlainText(self._wrapper_json())

    def _simulate_send(self) -> None:
        self.response_log.appendPlainText(
            json.dumps(
                {
                    "status": "prototype_only",
                    "message": "No network request was sent.",
                    "timestamp": int(time.time()),
                }
            )
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Bluepaws PySide6 drag-performance prototype")
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
            f"This prototype requires PySide6 {REQUIRED_PYSIDE_VERSION}; "
            f"found {PySide6.__version__}."
        )
    if args.check:
        print(f"Bluepaws Qt prototype dependencies available (PySide6 {PySide6.__version__}).")
        return 0

    app = QApplication(sys.argv[:1])
    app.setApplicationName("Bluepaws TLV Qt Prototype")
    app.setStyle("Fusion")
    app.setStyleSheet(STYLESHEET)
    window = QtDragPrototype()
    window.show()
    if args.auto_close_ms > 0:
        QTimer.singleShot(args.auto_close_ms, app.quit)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
