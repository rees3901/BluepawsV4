#!/usr/bin/env python3
"""Bluepaws PySide6 console for building and sending v1.1 TLV telemetry."""

from __future__ import annotations

import argparse
import base64
import json
import math
import random
import re
import sys
import threading
import time
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any
from urllib.parse import unquote

try:
    import PySide6
    from PySide6.QtCore import QObject, Qt, QTimer, QUrl, QUrlQuery, Signal
    from PySide6.QtGui import QBrush, QCloseEvent, QColor, QDesktopServices, QFont
    from PySide6.QtWidgets import (
        QAbstractItemView,
        QApplication,
        QCheckBox,
        QComboBox,
        QFileDialog,
        QFormLayout,
        QGridLayout,
        QGroupBox,
        QHBoxLayout,
        QHeaderView,
        QInputDialog,
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
    MAX_TLV_SIZE,
    POWER_PROFILE_CODES,
    STATUS_CODES,
    TX_REASON_CODES,
    BuiltPacket,
    DeviceCredential,
    GatewayCredential,
    PacketFields,
    TlvEntry,
    build_tlv_packet,
    build_transport_wrapper,
    custom_tlv,
    decode_tlv_payload,
    decode_hmac_key,
    firmware_tlv,
    known_tlv,
    load_credential_bundle,
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
EARTH_RADIUS_METRES = 6_371_000.0
EARTH_METRES_PER_DEGREE = math.tau * EARTH_RADIUS_METRES / 360.0
LIVE_PREVIEW_DELAY_MS = 250
DEFAULT_MOVEMENT_METRES = 200
MAX_MOVEMENT_METRES = 300
RESPONSE_HEADERS = ("Time", "#", "HTTP", "Result", "Seq", "ms", "Message")

RESULT_STYLES = {
    "success": ("#0B3A31", "#C3FFEB"),
    "redirect": ("#183A55", "#D6EEFF"),
    "auth": ("#38264A", "#F0DBFF"),
    "client": ("#473716", "#FFE7A3"),
    "server": ("#4A2028", "#FFD7DE"),
    "network": ("#3B2D34", "#F0DCE5"),
    "unknown": ("#24374A", "#D9E9F7"),
}


@dataclass(frozen=True)
class TestRecipe:
    description: str
    count: int
    interval: float
    strategy: str = "all_valid"
    tlv_mode: str = "none"
    transport: str = "cellular_direct"
    movement_metres: int | None = None


TEST_RECIPES = {
    "Basic sunny day": TestRecipe(
        "10 valid LTE header-only packets with gentle live movement and measurement variation.",
        10,
        2.0,
        movement_metres=50,
    ),
    "Moving pet": TestRecipe(
        "12 valid LTE packets with a bounded 200 m random walk.",
        12,
        2.0,
        movement_metres=200,
    ),
    "Rich known TLVs": TestRecipe(
        "10 valid packets carrying every selected v1.1 known TLV.",
        10,
        2.0,
        tlv_mode="full",
        movement_metres=50,
    ),
    "Maximum TLV budget": TestRecipe(
        "5 valid packets using the complete 24-byte optional TLV budget.",
        5,
        2.0,
        tlv_mode="maximum",
    ),
    "Random TLV assortment": TestRecipe(
        "10 valid packets with a different bounded selection of known and unknown TLVs.",
        10,
        2.0,
        tlv_mode="random",
        movement_metres=100,
    ),
    "Bad day — only 2 of 10 valid": TestRecipe(
        "Exactly 2 valid packets and 8 packets with deliberately corrupt HMAC tags.",
        10,
        1.0,
        strategy="bad_day",
        movement_metres=100,
    ),
    "Mixed bag": TestRecipe(
        "10 packets mixing valid/corrupt HMACs and randomized optional TLVs.",
        10,
        1.0,
        strategy="mixed",
        tlv_mode="random",
        movement_metres=150,
    ),
    "Fully randomized": TestRecipe(
        "12 bounded random packets mixing authentication outcomes, telemetry and TLVs.",
        12,
        1.0,
        strategy="randomized",
        tlv_mode="random",
        movement_metres=300,
    ),
    "Duplicate retry storm": TestRecipe(
        "Send the same valid packet 6 times to exercise idempotent retry handling.",
        6,
        1.0,
        strategy="duplicates",
    ),
    "Sequence rollover": TestRecipe(
        "5 valid packets beginning at sequence 65533 and wrapping through zero.",
        5,
        1.0,
        strategy="rollover",
        movement_metres=25,
    ),
    "Out-of-order delivery": TestRecipe(
        "6 valid packets with sequence 3 delivered before sequence 2.",
        6,
        1.0,
        strategy="out_of_order",
        movement_metres=75,
    ),
    "LoRa relay sunny day": TestRecipe(
        "10 valid header-only packets relayed through the selected provisioned gateway.",
        10,
        2.0,
        transport="lora_hub",
        movement_metres=50,
    ),
    "LTE radio fade": TestRecipe(
        "10 valid packets whose LTE signal measurements progressively deteriorate.",
        10,
        2.0,
        strategy="radio_fade",
    ),
    "HMAC rejection only": TestRecipe(
        "5 packets with deliberately corrupt HMAC tags; none should be accepted.",
        5,
        1.0,
        strategy="all_corrupt",
    ),
}

LIVE_INTEGER_VARIATION = {
    "battery_mv": (3, 0, 65_535),
    "accuracy_m": (2, 0, 65_535),
    "fix_age_s": (1, 0, 65_534),
    "satellite_count": (1, 0, 254),
    "activity_score": (2, 0, 255),
}
LIVE_RADIO_VARIATION = {
    "link_rssi_dbm": (2.0, -200.0, 0.0, 1),
    "link_snr_db": (0.5, -100.0, 100.0, 1),
    "cell_rsrp_dbm": (2.0, -200.0, 0.0, 1),
    "cell_rsrq_db": (0.5, -100.0, 0.0, 1),
    "cell_sinr_db": (0.5, -100.0, 100.0, 1),
}


def parse_coordinates(value: str) -> tuple[float, float]:
    """Extract latitude/longitude from plain text or a common Google Maps URL."""
    decoded = unquote(value.strip())
    patterns = (
        r"!3d(-?\d+(?:\.\d+)?)!4d(-?\d+(?:\.\d+)?)",
        r"(?<![\d.])(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)(?![\d.])",
    )
    for pattern in patterns:
        match = re.search(pattern, decoded, flags=re.IGNORECASE)
        if match is None:
            continue
        latitude, longitude = float(match.group(1)), float(match.group(2))
        if not -90 <= latitude <= 90:
            raise ValueError("latitude must be from -90 to 90")
        if not -180 <= longitude <= 180:
            raise ValueError("longitude must be from -180 to 180")
        return latitude, longitude
    raise ValueError("paste coordinates as latitude, longitude or a Google Maps URL")


def drift_coordinates(
    latitude: float,
    longitude: float,
    maximum_metres: float,
    *,
    rng: Any = random,
) -> tuple[float, float]:
    """Return one uniformly distributed random-walk step within the radius."""
    if not 0 < maximum_metres <= MAX_MOVEMENT_METRES:
        raise ValueError(
            f"maximum drift must be greater than 0 and at most {MAX_MOVEMENT_METRES} metres"
        )
    radius = maximum_metres * math.sqrt(rng.random())
    angle = rng.random() * math.tau
    latitude_delta = (radius * math.cos(angle)) / EARTH_METRES_PER_DEGREE
    longitude_scale = EARTH_METRES_PER_DEGREE * max(
        abs(math.cos(math.radians(latitude))), 1e-6
    )
    longitude_delta = (radius * math.sin(angle)) / longitude_scale
    next_latitude = max(-90.0, min(90.0, latitude + latitude_delta))
    next_longitude = ((longitude + longitude_delta + 180.0) % 360.0) - 180.0
    return next_latitude, next_longitude


def vary_integer(
    value: int,
    maximum_delta: int,
    minimum: int,
    maximum: int,
    *,
    rng: Any = random,
) -> int:
    """Apply bounded integer measurement noise around a configured baseline."""
    return max(minimum, min(maximum, value + rng.randint(-maximum_delta, maximum_delta)))


def vary_float(
    value: float | None,
    maximum_delta: float,
    minimum: float,
    maximum: float,
    decimals: int,
    *,
    rng: Any = random,
) -> float | None:
    """Apply bounded floating-point measurement noise, preserving omitted fields."""
    if value is None:
        return None
    varied = max(minimum, min(maximum, value + rng.uniform(-maximum_delta, maximum_delta)))
    return round(varied, decimals)


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
QGroupBox#optionalTlvGroup:unchecked,
QGroupBox#recipeGroup:unchecked {{
    background: #05111C;
    border-color: #102A3F;
    color: #587086;
}}
QGroupBox#optionalTlvGroup:unchecked QGroupBox {{
    background: #061522;
    border-color: #102A3F;
    color: #587086;
}}
QGroupBox#optionalTlvGroup:unchecked QTableWidget {{
    background: #04101A;
    border-color: #102A3F;
    color: #587086;
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
QTableWidget {{
    gridline-color: {BORDER};
    padding: 0;
}}
QTableWidget::item {{
    padding: 5px;
}}
QTableWidget::item:selected {{
    border: 1px solid {BLUE};
}}
QHeaderView::section {{
    background: {NAVY};
    color: {MUTED};
    border: 0;
    border-right: 1px solid {BORDER};
    border-bottom: 1px solid {BORDER};
    padding: 6px;
    font-weight: 600;
}}
QLineEdit:disabled {{
    background: #091B2A;
    color: {MUTED};
    border-color: #153A56;
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
QCheckBox::indicator:unchecked {{
    background: {SURFACE_RAISED};
    border: 1px solid {MUTED};
    border-radius: 3px;
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
    response = Signal(object)
    finished = Signal(int, int, object)


class BluepawsTlvConsole(QMainWindow):
    """Feature-complete Qt interface backed by the shared packet codec."""

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Bluepaws TLV Telemetry Test Console")
        self.resize(1220, 860)
        self.setMinimumSize(980, 700)
        self._credentials: dict[str, DeviceCredential] = {}
        self._gateway_credentials: dict[str, GatewayCredential] = {}
        self._custom_tlvs: list[TlvEntry] = []
        self._last_built: BuiltPacket | None = None
        self._prepared_fields: list[PacketFields] = []
        self._stop_event = threading.Event()
        self._worker: threading.Thread | None = None
        self._auto_build_timer = QTimer(self)
        self._auto_build_timer.setSingleShot(True)
        self._auto_build_timer.setInterval(LIVE_PREVIEW_DELAY_MS)
        self._auto_build_timer.timeout.connect(self.build_packet)
        self._auto_preview_timer = QTimer(self)
        self._auto_preview_timer.setSingleShot(True)
        self._auto_preview_timer.setInterval(LIVE_PREVIEW_DELAY_MS)
        self._auto_preview_timer.timeout.connect(self.preview_wrapper)
        self.worker_signals = WorkerSignals()
        self.worker_signals.response.connect(self._append_response)
        self.worker_signals.finished.connect(self._send_finished)
        self._build_ui()
        self._connect_live_previews()
        self._transport_changed()
        self._tag_mode_changed()
        self.statusBar().showMessage("Ready • protocol v1.1 • secrets remain on this computer")
        self._schedule_packet_build()

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

        self.tabs = QTabWidget()
        self.packet_builder_tab = self._packet_tab()
        self.wrapper_tab = self._wrapper_tab()
        self.response_tab = self._response_tab()
        self.tabs.addTab(self.packet_builder_tab, "1. TLV Packet Builder")
        self.tabs.addTab(self.wrapper_tab, "2. HTTPS Wrapper")
        self.tabs.addTab(self.response_tab, "3. Send & Response Log")
        outer.addWidget(self.tabs, 1)
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

        self.cookbook_group = QGroupBox("Use test cookbook")
        self.cookbook_group.setObjectName("recipeGroup")
        self.cookbook_group.setCheckable(True)
        self.cookbook_group.setChecked(False)
        self.cookbook_group.setToolTip(
            "Enable a predefined multi-packet test. Manual packet configuration remains "
            "the default when this section is off."
        )
        cookbook_layout = QGridLayout(self.cookbook_group)
        cookbook_layout.addWidget(QLabel("Recipe"), 0, 0)
        self.recipe_combo = combo(list(TEST_RECIPES))
        self.recipe_combo.currentTextChanged.connect(self._recipe_changed)
        cookbook_layout.addWidget(self.recipe_combo, 0, 1)
        self.run_recipe_button = QPushButton("Run selected recipe")
        self.run_recipe_button.clicked.connect(self._run_recipe)
        cookbook_layout.addWidget(self.run_recipe_button, 0, 2)
        self.recipe_summary = QLabel()
        self.recipe_summary.setWordWrap(True)
        self.recipe_summary.setStyleSheet(f"color: {MUTED};")
        cookbook_layout.addWidget(self.recipe_summary, 1, 0, 1, 3)
        cookbook_layout.setColumnStretch(1, 1)
        self.cookbook_group.toggled.connect(self._recipe_enabled)
        grid.addWidget(self.cookbook_group, 0, 0, 1, 2)
        self._update_recipe_summary()

        credentials = QGroupBox("Provisioned test credentials")
        credential_row = QGridLayout(credentials)
        load = QPushButton("Load credentials JSON…")
        load.clicked.connect(self.load_credentials_file)
        credential_row.addWidget(load, 0, 0)
        credential_row.addWidget(QLabel("Device"), 0, 1)
        self.credential_combo = combo([])
        self.credential_combo.currentTextChanged.connect(self._credential_selected)
        credential_row.addWidget(self.credential_combo, 0, 2)
        credential_row.addWidget(QLabel("Gateway"), 0, 3)
        self.gateway_combo = combo([])
        self.gateway_combo.setEnabled(False)
        self.gateway_combo.currentTextChanged.connect(self._gateway_selected)
        credential_row.addWidget(self.gateway_combo, 0, 4)
        credential_note = QLabel(
            "The selected transport chooses the device or gateway bearer automatically. "
            "Secrets stay masked and are never logged."
        )
        credential_note.setStyleSheet(f"color: {MUTED};")
        credential_row.addWidget(credential_note, 1, 0, 1, 5)
        credential_row.setColumnStretch(2, 1)
        credential_row.setColumnStretch(4, 1)
        grid.addWidget(credentials, 1, 0, 1, 2)

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
        header_form.addRow("Message sequence  ↗", self.sequence)
        timestamp_row = QHBoxLayout()
        timestamp_row.addWidget(self.timestamp, 1)
        timestamp_now = QPushButton("Now")
        timestamp_now.clicked.connect(self._set_now)
        timestamp_row.addWidget(timestamp_now)
        header_form.addRow("Unix timestamp  ↗", timestamp_row)
        header_form.addRow("Status", self.status)
        header_form.addRow("Power profile", self.profile)
        header_form.addRow("TX reason", self.reason)
        grid.addWidget(header, 2, 0)

        position = QGroupBox("Position and telemetry")
        position_form = QFormLayout(position)
        self.latitude = line_edit("51.907055")
        self.longitude = line_edit("-2.256660")
        self.battery_mv = line_edit("3900")
        self.accuracy_m = line_edit("8")
        self.fix_age_s = line_edit("0")
        self.satellites = line_edit("9")
        for label, field in (
            ("Latitude  ±", self.latitude),
            ("Longitude  ±", self.longitude),
            ("Battery (mV)  ±3", self.battery_mv),
            ("Accuracy (m)  ±2", self.accuracy_m),
            ("Fix age (s)  ±1", self.fix_age_s),
            ("Satellite count  ±1", self.satellites),
        ):
            position_form.addRow(label, field)
        variation_note = QLabel(
            "Live markers: ↗ advances • ± bounded variation after the first packet. "
            "Location varies only with movement enabled."
        )
        variation_note.setStyleSheet(f"color: {MUTED};")
        variation_note.setWordWrap(True)
        position_form.addRow(variation_note)
        position_form.addRow(QLabel("Use fix age 65535 or satellites 255 for unknown; sentinels do not vary."))
        map_tools = QHBoxLayout()
        open_maps = QPushButton("Open Google Maps…")
        open_maps.clicked.connect(self._open_google_maps)
        open_maps.setToolTip("Open Google Maps at the current latitude and longitude.")
        paste_maps = secondary_button("Paste coordinates…")
        paste_maps.clicked.connect(self._paste_map_coordinates)
        paste_maps.setToolTip(
            "Paste copied latitude/longitude text or a full Google Maps URL."
        )
        map_tools.addWidget(open_maps)
        map_tools.addWidget(paste_maps)
        map_tools.addStretch()
        position_form.addRow("Map tools", map_tools)

        drift_row = QHBoxLayout()
        self.drift_enabled = QCheckBox("Random-walk movement")
        self.drift_enabled.setChecked(True)
        self.drift_enabled.setToolTip(
            "Move each packet after the first by a random distance within this radius "
            f"(maximum {MAX_MOVEMENT_METRES} metres)."
        )
        self.maximum_drift = line_edit(str(DEFAULT_MOVEMENT_METRES))
        self.maximum_drift.setMaximumWidth(70)
        self.maximum_drift.setEnabled(True)
        self.drift_enabled.toggled.connect(self._drift_mode_changed)
        drift_row.addWidget(self.drift_enabled)
        drift_row.addWidget(QLabel("Maximum"))
        drift_row.addWidget(self.maximum_drift)
        drift_row.addWidget(QLabel("metres per packet"))
        drift_row.addStretch()
        position_form.addRow("Movement", drift_row)
        grid.addWidget(position, 2, 1)

        flags = QGroupBox("Header flags")
        flag_grid = QGridLayout(flags)
        self.flag_checks: dict[str, QCheckBox] = {}
        for index, (name, mask) in enumerate(FLAG_MASKS.items()):
            flag = QCheckBox(f"{name} (0x{mask:02X})")
            flag.setChecked(name in ("GNSS_VALID", "FIX_3D"))
            self.flag_checks[name] = flag
            flag_grid.addWidget(flag, index // 4, index % 4)
        grid.addWidget(flags, 3, 0, 1, 2)

        self.tlv_options = QGroupBox("Include optional TLVs in this packet")
        self.tlv_options.setObjectName("optionalTlvGroup")
        self.tlv_options.setCheckable(True)
        self.tlv_options.setChecked(False)
        self.tlv_options.setToolTip(
            "Off by default for a basic authenticated 40-byte header packet. "
            "Enable to edit selected or custom TLVs."
        )
        self.tlv_options.toggled.connect(self._schedule_packet_build)
        tlv_options_layout = QHBoxLayout(self.tlv_options)

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
            live_marker = "  ↗" if name == "uptime_s" else "  ±2" if name == "activity_score" else ""
            check = QCheckBox(f"{code}  {name}{live_marker}")
            check.setChecked(enabled)
            value_field = line_edit(value)
            self.known_checks[name] = check
            self.known_values[name] = value_field
            known_grid.addWidget(check, row, 0)
            known_grid.addWidget(value_field, row, 1)
            known_grid.addWidget(QLabel(hint), row, 2)
        tlv_options_layout.addWidget(known, 1)

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
        tlv_options_layout.addWidget(custom, 1)
        grid.addWidget(self.tlv_options, 4, 0, 1, 2)

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
        self.tag_mode.setCurrentText("Valid HMAC (normal packet)")
        self.tag_mode.currentTextChanged.connect(self._tag_mode_changed)
        self.custom_tag = line_edit("0000000000000000")
        authentication_grid.addWidget(self.tag_mode, 1, 1)
        authentication_grid.addWidget(self.custom_tag, 1, 2)
        grid.addWidget(authentication, 5, 0, 1, 2)

        output = QGroupBox("Packet output")
        output_layout = QVBoxLayout(output)
        output_actions = QHBoxLayout()
        build = QPushButton("Rebuild now")
        build.clicked.connect(self.build_packet)
        build.setToolTip("The preview updates automatically; this forces an immediate rebuild.")
        output_actions.addWidget(build)
        self.packet_summary = QLabel("Auto-updating packet preview")
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
        self.packet_b64.setReadOnly(True)
        output_layout.addWidget(self.packet_b64)
        output_layout.addWidget(QLabel("Packet hex"))
        self.packet_hex = QPlainTextEdit()
        self.packet_hex.setMaximumHeight(100)
        self.packet_hex.setReadOnly(True)
        output_layout.addWidget(self.packet_hex)
        grid.addWidget(output, 6, 0, 1, 2)

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
        self.bearer_hint = QLabel()
        self.bearer_hint.setStyleSheet(f"color: {MUTED};")
        self.bearer_hint.setWordWrap(True)
        request_form.addRow("", self.bearer_hint)
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
        metadata_form.addRow("Link RSSI (dBm)  ±2", self.link_rssi)
        metadata_form.addRow("Link SNR (dB)  ±0.5", self.link_snr)
        metadata_form.addRow("LTE RSRP (dBm)  ±2", self.cell_rsrp)
        metadata_form.addRow("LTE RSRQ (dB)  ±0.5", self.cell_rsrq)
        metadata_form.addRow("LTE SINR (dB)  ±0.5", self.cell_sinr)
        self.gateway_widgets = [self.gateway_guid, self.gateway_rx_time, self.gateway_now]
        self.cell_widgets = [self.cell_rsrp, self.cell_rsrq, self.cell_sinr]
        grid.addWidget(metadata, 0, 1)

        preview = QGroupBox("Payload and packet previews")
        preview_layout = QVBoxLayout(preview)
        preview_actions = QHBoxLayout()
        refresh = QPushButton("Refresh previews")
        refresh.clicked.connect(self.preview_wrapper)
        preview_actions.addWidget(refresh)
        self.wrapper_summary = QLabel("JSON body not available")
        self.wrapper_summary.setStyleSheet(f"color: {SUCCESS};")
        self.wrapper_summary.setToolTip(
            "Compact UTF-8 JSON request body and decoded TLV packet sizes; "
            "HTTPS headers and TLS overhead are not included."
        )
        preview_actions.addWidget(self.wrapper_summary)
        preview_actions.addStretch()
        preview_layout.addLayout(preview_actions)

        payload_heading = QHBoxLayout()
        payload_heading.addWidget(QLabel("Decoded TLV payload — human-readable JSON"))
        payload_heading.addStretch()
        copy_payload = secondary_button("Copy payload JSON")
        copy_payload.clicked.connect(lambda: self._copy_text(self.payload_preview))
        payload_heading.addWidget(copy_payload)
        preview_layout.addLayout(payload_heading)
        self.payload_summary = QLabel("Decoded payload not available")
        self.payload_summary.setStyleSheet(f"color: {MUTED};")
        preview_layout.addWidget(self.payload_summary)
        self.payload_preview = QPlainTextEdit()
        self.payload_preview.setReadOnly(True)
        self.payload_preview.setMinimumHeight(160)
        preview_layout.addWidget(self.payload_preview, 1)

        wrapper_heading = QHBoxLayout()
        wrapper_heading.addWidget(QLabel("HTTPS JSON wrapper — Authorization header omitted"))
        wrapper_heading.addStretch()
        copy_json = secondary_button("Copy wrapper JSON")
        copy_json.clicked.connect(lambda: self._copy_text(self.wrapper_preview))
        wrapper_heading.addWidget(copy_json)
        preview_layout.addLayout(wrapper_heading)
        self.wrapper_preview = QPlainTextEdit()
        self.wrapper_preview.setReadOnly(True)
        self.wrapper_preview.setMinimumHeight(160)
        preview_layout.addWidget(self.wrapper_preview, 1)
        grid.addWidget(preview, 1, 0, 1, 2)
        return body

    def _response_tab(self) -> QWidget:
        body = QWidget()
        body_layout = QVBoxLayout(body)
        body_layout.setContentsMargins(10, 10, 10, 10)
        sender = QGroupBox("Send and response log")
        sender_layout = QVBoxLayout(sender)
        controls = QHBoxLayout()
        self.send_count = line_edit("5")
        self.send_count.setMaximumWidth(72)
        self.send_interval = line_edit("5")
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
        self.advance_packets = QCheckBox(
            "Live simulation: advance time/sequence and vary marked measurements"
        )
        self.advance_packets.setChecked(True)
        self.advance_packets.setToolTip(
            "Recommended for realistic telemetry. Marked measurements vary within their "
            "displayed bounds; turn off only to resend the exact same packet."
        )
        self.advance_packets.toggled.connect(self._live_mode_changed)
        sender_layout.addWidget(self.advance_packets)
        duplicate_hint = QLabel(
            "Turn live simulation off only when deliberately testing duplicate handling."
        )
        duplicate_hint.setStyleSheet(f"color: {MUTED};")
        sender_layout.addWidget(duplicate_hint)
        buttons = QHBoxLayout()
        self.send_button = QPushButton("Send")
        self.send_button.clicked.connect(self.send_requests)
        self.stop_button = QPushButton("Stop")
        self.stop_button.setProperty("danger", True)
        self.stop_button.setEnabled(False)
        self.stop_button.clicked.connect(self.stop_sending)
        clear = secondary_button("Clear results")
        clear.clicked.connect(self._clear_responses)
        buttons.addWidget(self.send_button)
        buttons.addWidget(self.stop_button)
        buttons.addStretch()
        buttons.addWidget(clear)
        sender_layout.addLayout(buttons)
        self.sender_status = QLabel("Packet and wrapper previews update automatically.")
        self.sender_status.setStyleSheet(f"color: {MUTED};")
        sender_layout.addWidget(self.sender_status)
        self.response_table = QTableWidget(0, len(RESPONSE_HEADERS))
        self.response_table.setHorizontalHeaderLabels(RESPONSE_HEADERS)
        self.response_table.verticalHeader().setVisible(False)
        self.response_table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.response_table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.response_table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.response_table.setAlternatingRowColors(False)
        self.response_table.itemSelectionChanged.connect(self._response_selected)
        response_header = self.response_table.horizontalHeader()
        response_header.setMinimumSectionSize(42)
        response_header.setSectionsMovable(True)
        for column in range(len(RESPONSE_HEADERS)):
            response_header.setSectionResizeMode(column, QHeaderView.ResizeMode.Interactive)
        for column, width in enumerate((72, 42, 58, 130, 84, 60, 260)):
            self.response_table.setColumnWidth(column, width)
        self.response_table.setMinimumHeight(190)
        sender_layout.addWidget(self.response_table, 1)
        sender_layout.addWidget(QLabel("Selected response JSON"))
        self.response_detail = QPlainTextEdit()
        self.response_detail.setReadOnly(True)
        self.response_detail.setPlaceholderText("Select a result row to inspect its full response.")
        self.response_detail.setMaximumHeight(180)
        sender_layout.addWidget(self.response_detail)
        body_layout.addWidget(sender, 1)
        return body

    def _connect_live_previews(self) -> None:
        """Debounce edits so generated output always follows the visible form state."""
        packet_lines = (
            self.device_id,
            self.sequence,
            self.timestamp,
            self.latitude,
            self.longitude,
            self.battery_mv,
            self.accuracy_m,
            self.fix_age_s,
            self.satellites,
            self.hmac,
            self.custom_tag,
            *self.known_values.values(),
        )
        for field in packet_lines:
            field.textChanged.connect(self._schedule_packet_build)

        for selector in (self.status, self.profile, self.reason, self.tag_mode):
            selector.currentTextChanged.connect(self._schedule_packet_build)
        for check in (*self.flag_checks.values(), *self.known_checks.values()):
            check.toggled.connect(self._schedule_packet_build)

        wrapper_lines = (
            self.gateway_guid,
            self.gateway_rx_time,
            self.link_rssi,
            self.link_snr,
            self.cell_rsrp,
            self.cell_rsrq,
            self.cell_sinr,
        )
        for field in wrapper_lines:
            field.textChanged.connect(self._schedule_wrapper_preview)

    def _schedule_packet_build(self, *_unused: object) -> None:
        self._auto_build_timer.start()

    def _schedule_wrapper_preview(self, *_unused: object) -> None:
        self._auto_preview_timer.start()

    def load_credentials_file(self) -> None:
        selected, _ = QFileDialog.getOpenFileName(
            self, "Load Bluepaws credential bundle", "", "JSON files (*.json);;All files (*.*)"
        )
        if not selected:
            return
        try:
            bundle = load_credential_bundle(Path(selected))
        except (OSError, ValueError, json.JSONDecodeError) as error:
            QMessageBox.critical(self, "Unable to load credentials", str(error))
            return
        self._credentials = {f"Device {item.device_id}": item for item in bundle.devices}
        self._gateway_credentials = {
            self._gateway_label(item): item for item in bundle.gateways
        }
        self.credential_combo.blockSignals(True)
        self.credential_combo.clear()
        self.credential_combo.addItems(list(self._credentials))
        self.credential_combo.blockSignals(False)
        self.credential_combo.setCurrentIndex(0)
        self.gateway_combo.blockSignals(True)
        self.gateway_combo.clear()
        self.gateway_combo.addItems(list(self._gateway_credentials))
        self.gateway_combo.blockSignals(False)
        if self._gateway_credentials:
            self.gateway_combo.setCurrentIndex(0)
        self._apply_credential(bundle.devices[0])
        if bundle.gateways:
            self._apply_gateway(bundle.gateways[0])
        self._transport_changed()
        self.builder_status.setText(
            f"Loaded {len(bundle.devices)} device(s) and {len(bundle.gateways)} gateway(s)."
        )

    def _credential_selected(self, name: str) -> None:
        credential = self._credentials.get(name)
        if credential is not None:
            self._apply_credential(credential)

    def _gateway_selected(self, name: str) -> None:
        credential = self._gateway_credentials.get(name)
        if credential is not None:
            self._apply_gateway(credential)

    def _apply_credential(self, credential: DeviceCredential) -> None:
        self.device_id.setText(str(credential.device_id))
        self.hmac.setText(base64.b64encode(credential.hmac_key).decode("ascii"))
        if TRANSPORTS[self.transport.currentText()] == "cellular_direct":
            self.bearer.setText(credential.token)
        self.builder_status.setText(
            f"Device {credential.device_id} selected. Secrets remain masked."
        )
        self.build_packet()

    def _apply_gateway(self, credential: GatewayCredential) -> None:
        self.gateway_guid.setText(credential.gateway_guid16)
        if TRANSPORTS[self.transport.currentText()] == "lora_hub":
            self.bearer.setText(credential.token)
        self.preview_wrapper()

    @staticmethod
    def _gateway_label(credential: GatewayCredential) -> str:
        suffix = f" — {credential.display_name}" if credential.display_name else ""
        return f"Gateway {credential.gateway_guid16}{suffix}"

    def _set_now(self) -> None:
        self.timestamp.setText(str(int(time.time())))

    def _set_gateway_now(self) -> None:
        self.gateway_rx_time.setText(str(int(time.time())))

    def _open_google_maps(self) -> None:
        try:
            latitude = self._float(self.latitude.text(), "latitude")
            longitude = self._float(self.longitude.text(), "longitude")
            if not -90 <= latitude <= 90 or not -180 <= longitude <= 180:
                raise ValueError("coordinates are outside the valid latitude/longitude range")
        except ValueError as error:
            QMessageBox.critical(self, "Cannot open Google Maps", str(error))
            return
        url = QUrl("https://www.google.com/maps/search/")
        query = QUrlQuery()
        query.addQueryItem("api", "1")
        query.addQueryItem("query", f"{latitude:.7f},{longitude:.7f}")
        url.setQuery(query)
        if not QDesktopServices.openUrl(url):
            QMessageBox.critical(self, "Cannot open Google Maps", "Windows could not open the web browser.")

    def _paste_map_coordinates(self) -> None:
        clipboard = QApplication.clipboard()
        initial = clipboard.text().strip() if clipboard is not None else ""
        value, accepted = QInputDialog.getMultiLineText(
            self,
            "Import coordinates from Google Maps",
            "In Google Maps, right-click a point and copy its coordinates, or copy the page URL.\n"
            "Paste latitude, longitude or the Google Maps URL here:",
            initial,
        )
        if not accepted:
            return
        try:
            latitude, longitude = parse_coordinates(value)
        except ValueError as error:
            QMessageBox.critical(self, "Invalid coordinates", str(error))
            return
        self.latitude.setText(f"{latitude:.7f}")
        self.longitude.setText(f"{longitude:.7f}")
        self.builder_status.setText("Coordinates imported from Google Maps.")
        self.build_packet()

    def _live_mode_changed(self, enabled: bool) -> None:
        self.drift_enabled.setEnabled(enabled)
        self.maximum_drift.setEnabled(enabled and self.drift_enabled.isChecked())

    def _drift_mode_changed(self, enabled: bool) -> None:
        self.maximum_drift.setEnabled(enabled and self.advance_packets.isChecked())

    def _active_recipe(self) -> TestRecipe | None:
        if not self.cookbook_group.isChecked():
            return None
        return TEST_RECIPES[self.recipe_combo.currentText()]

    def _update_recipe_summary(self) -> None:
        recipe = TEST_RECIPES[self.recipe_combo.currentText()]
        movement = (
            f"movement ≤ {recipe.movement_metres} m"
            if recipe.movement_metres is not None
            else "stationary"
        )
        self.recipe_summary.setText(
            f"{recipe.count} packets • {recipe.interval:g} s interval • {movement} • "
            f"{recipe.description}"
        )

    def _recipe_enabled(self, enabled: bool) -> None:
        self.send_count.setEnabled(not enabled)
        self.send_interval.setEnabled(not enabled)
        self.advance_packets.setEnabled(not enabled)
        if enabled:
            self._apply_recipe()
        else:
            self.sender_status.setText(
                "Manual mode restored. Recipe values remain visible and can now be edited."
            )

    def _recipe_changed(self, _name: str) -> None:
        self._update_recipe_summary()
        if self.cookbook_group.isChecked():
            self._apply_recipe()

    def _run_recipe(self) -> None:
        if not self.cookbook_group.isChecked():
            return
        self.tabs.setCurrentWidget(self.response_tab)
        self.send_requests()

    def _apply_recipe(self) -> None:
        recipe = TEST_RECIPES[self.recipe_combo.currentText()]
        self.send_count.setText(str(recipe.count))
        self.send_interval.setText(f"{recipe.interval:g}")
        self.advance_packets.setChecked(recipe.strategy != "duplicates")
        self.drift_enabled.setChecked(recipe.movement_metres is not None)
        self.maximum_drift.setText(
            str(recipe.movement_metres or DEFAULT_MOVEMENT_METRES)
        )
        self.tag_mode.setCurrentText("Valid HMAC (normal packet)")
        self.status.setCurrentText("OUT (1)")
        self.profile.setCurrentText("NORMAL (1)")
        self.reason.setCurrentText("TELEMETRY (0)")
        for name, flag in self.flag_checks.items():
            flag.setChecked(name in ("GNSS_VALID", "FIX_3D"))
        for name, value in {
            "battery_mv": "3900",
            "accuracy_m": "8",
            "fix_age_s": "0",
            "satellites": "9",
            "link_rssi": "-104",
            "link_snr": "7.0",
            "cell_rsrp": "-104",
            "cell_rsrq": "-9.5",
            "cell_sinr": "7.0",
        }.items():
            getattr(self, name).setText(value)
        transport_label = next(
            label for label, value in TRANSPORTS.items() if value == recipe.transport
        )
        self.transport.setCurrentText(transport_label)
        self.sequence.setText(
            "65533" if recipe.strategy == "rollover" else str(int(time.time()) & 0xFFFF)
        )

        include_tlvs = recipe.tlv_mode != "none"
        self.tlv_options.setChecked(include_tlvs)
        for check in self.known_checks.values():
            check.setChecked(include_tlvs)
        custom_entries: list[TlvEntry] = []
        if recipe.tlv_mode == "maximum":
            custom_entries.append(TlvEntry(0x7E, b"\x01\x02", "custom"))
        self._set_custom_tlvs(custom_entries)
        self._update_recipe_summary()
        self.sender_status.setText(
            f"Cookbook ready: {self.recipe_combo.currentText()}. Choose Send on tab 2."
        )
        self.build_packet()

    def _tag_mode_changed(self) -> None:
        self.custom_tag.setEnabled(TAG_MODES[self.tag_mode.currentText()] == "custom")

    def _transport_changed(self) -> None:
        is_lora = TRANSPORTS[self.transport.currentText()] == "lora_hub"
        self.gateway_combo.setEnabled(is_lora and bool(self._gateway_credentials))
        if is_lora:
            gateway = self._gateway_credentials.get(self.gateway_combo.currentText())
            if gateway is not None:
                self.gateway_guid.setText(gateway.gateway_guid16)
                self.bearer.setText(gateway.token)
            else:
                device = self._credentials.get(self.credential_combo.currentText())
                if device is not None and self.bearer.text() == device.token:
                    self.bearer.clear()
        else:
            device = self._credentials.get(self.credential_combo.currentText())
            if device is not None:
                self.bearer.setText(device.token)
        self.bearer_hint.setText(
            "The selected gateway bearer token authenticates this relay. "
            "Its four-digit GUID identifies the hub but is not a secret."
            if is_lora
            else "The selected device bearer token authenticates direct LTE ingestion."
        )
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

    def _set_custom_tlvs(self, entries: list[TlvEntry]) -> None:
        self._custom_tlvs = list(entries)
        self.custom_table.setRowCount(0)
        for entry in entries:
            row = self.custom_table.rowCount()
            self.custom_table.insertRow(row)
            self.custom_table.setItem(row, 0, QTableWidgetItem(f"0x{entry.tlv_type:02X}"))
            self.custom_table.setItem(row, 1, QTableWidgetItem(str(len(entry.value))))
            self.custom_table.setItem(row, 2, QTableWidgetItem(entry.value.hex().upper()))

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
        if not self.tlv_options.isChecked():
            return []
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
        self._auto_build_timer.stop()
        try:
            built = self._build_from_fields()
        except ValueError as error:
            self._last_built = None
            self.packet_summary.setText("Packet not built")
            self.wrapper_summary.setText("JSON body not available")
            self.payload_summary.setText("Decoded payload not available")
            self.builder_status.setText(str(error))
            self.packet_b64.clear()
            self.packet_hex.clear()
            self.payload_preview.clear()
            self.wrapper_preview.clear()
            self.sender_status.setText("Correct the packet fields before sending.")
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
        metadata_overrides: dict[str, float | None] | None = None,
    ) -> dict[str, Any]:
        transport = TRANSPORTS[self.transport.currentText()]
        payload = payload_b64 if payload_b64 is not None else self.packet_b64.toPlainText().strip()
        common = {
            "link_rssi_dbm": self._optional_float(self.link_rssi.text(), "link RSSI"),
            "link_snr_db": self._optional_float(self.link_snr.text(), "link SNR"),
        }
        if metadata_overrides is not None:
            common.update(
                {
                    key: metadata_overrides[key]
                    for key in ("link_rssi_dbm", "link_snr_db")
                    if key in metadata_overrides
                }
            )
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
        cellular = {
            "cell_rsrp_dbm": self._optional_float(self.cell_rsrp.text(), "LTE RSRP"),
            "cell_rsrq_db": self._optional_float(self.cell_rsrq.text(), "LTE RSRQ"),
            "cell_sinr_db": self._optional_float(self.cell_sinr.text(), "LTE SINR"),
        }
        if metadata_overrides is not None:
            cellular.update(
                {
                    key: metadata_overrides[key]
                    for key in cellular
                    if key in metadata_overrides
                }
            )
        return build_transport_wrapper(
            payload,
            transport,
            **cellular,
            **common,
        )

    def preview_wrapper(self) -> dict[str, Any] | None:
        self._auto_preview_timer.stop()
        try:
            payload = decode_tlv_payload(
                self.packet_b64.toPlainText().strip(),
                decode_hmac_key(self.hmac.text()),
            )
        except ValueError as error:
            self.payload_preview.clear()
            self.payload_summary.setText("Decoded payload not available")
            self.wrapper_preview.clear()
            self.wrapper_summary.setText("JSON body not available")
            self.sender_status.setText(str(error))
            return None

        self.payload_preview.setPlainText(json.dumps(payload, indent=2))
        authentication_valid = payload["authentication"]["valid"]
        authentication_label = "valid" if authentication_valid else "invalid"
        self.payload_summary.setText(
            f"Payload {payload['packet']['size_bytes']} bytes • "
            f"TLVs {payload['packet']['tlv_length_bytes']} bytes "
            f"({len(payload['tlvs'])} entries) • HMAC {authentication_label}"
        )

        try:
            wrapper = self._wrapper()
        except ValueError as error:
            self.wrapper_preview.clear()
            self.wrapper_summary.setText("JSON body not available")
            self.sender_status.setText(str(error))
            return None
        self.wrapper_preview.setPlainText(json.dumps(wrapper, indent=2))
        compact_body = json.dumps(wrapper, separators=(",", ":")).encode("utf-8")
        decoded_packet = base64.b64decode(wrapper["payload_b64"], validate=True)
        self.wrapper_summary.setText(
            f"JSON body {len(compact_body)} bytes • TLV packet {len(decoded_packet)} bytes"
        )
        self.sender_status.setText(
            "Wrapper passes local validation. Authorization header is not shown."
        )
        return wrapper

    def send_requests(self) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        try:
            if self.build_packet() is None:
                raise ValueError(self.builder_status.text())
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
        recipe_name = self.recipe_combo.currentText() if self._active_recipe() else None
        self.sender_status.setText(
            f"Sending {count} request(s)"
            + (f" from {recipe_name}" if recipe_name else "")
            + "…"
        )
        self._worker = threading.Thread(
            target=self._send_worker,
            args=(endpoint, token, requests, interval, timeout),
            daemon=True,
        )
        self._worker.start()

    def _prepare_requests(
        self, count: int, interval: float
    ) -> list[tuple[int, dict[str, Any]]]:
        self._prepared_fields = []
        recipe = self._active_recipe()
        if not self.advance_packets.isChecked():
            wrapper = self._wrapper()
            sequence = self._int(self.sequence.text(), "message sequence")
            return [(sequence, wrapper) for _ in range(count)]

        base_fields = self._packet_fields()
        base_send_time = int(time.time())
        hmac_key = decode_hmac_key(self.hmac.text())
        base_tlvs = self._packet_tlvs()
        base_metadata = self._base_transport_metadata()
        default_mode = TAG_MODES[self.tag_mode.currentText()]
        base_gateway_time = (
            base_send_time
            if TRANSPORTS[self.transport.currentText()] == "lora_hub"
            else None
        )
        drift_radius = None
        if self.drift_enabled.isChecked():
            drift_radius = self._float(self.maximum_drift.text(), "maximum drift")
            if not 0 < drift_radius <= MAX_MOVEMENT_METRES:
                raise ValueError(
                    "maximum drift must be greater than 0 and at most "
                    f"{MAX_MOVEMENT_METRES} metres"
                )
        latitude = base_fields.latitude
        longitude = base_fields.longitude
        result: list[tuple[int, dict[str, Any]]] = []
        for index in range(count):
            sequence_offset = self._recipe_sequence_offset(recipe, index)
            offset = round(sequence_offset * interval)
            arrival_offset = round(index * interval)
            if index and drift_radius is not None:
                latitude, longitude = drift_coordinates(latitude, longitude, drift_radius)
            battery_mv = base_fields.battery_mv
            accuracy_m = base_fields.accuracy_m
            fix_age_s = base_fields.fix_age_s
            satellite_count = base_fields.satellite_count
            if index:
                battery_mv = vary_integer(
                    battery_mv, *LIVE_INTEGER_VARIATION["battery_mv"]
                )
                accuracy_m = vary_integer(
                    accuracy_m, *LIVE_INTEGER_VARIATION["accuracy_m"]
                )
                if fix_age_s != 65_535:
                    fix_age_s = vary_integer(
                        fix_age_s, *LIVE_INTEGER_VARIATION["fix_age_s"]
                    )
                if satellite_count != 255:
                    satellite_count = vary_integer(
                        satellite_count, *LIVE_INTEGER_VARIATION["satellite_count"]
                    )
            if recipe is not None and recipe.strategy == "randomized":
                battery_mv = random.randint(3_300, 4_200)
                accuracy_m = random.randint(1, 100)
                fix_age_s = random.randint(0, 60)
                satellite_count = random.randint(4, 16)
            fields = replace(
                base_fields,
                message_sequence=(base_fields.message_sequence + sequence_offset) & 0xFFFF,
                timestamp=min(0xFFFF_FFFF, base_send_time + offset),
                latitude=latitude,
                longitude=longitude,
                battery_mv=battery_mv,
                accuracy_m=accuracy_m,
                fix_age_s=fix_age_s,
                satellite_count=satellite_count,
            )
            self._prepared_fields.append(fields)
            tlvs = self._vary_live_tlvs(base_tlvs, index, offset)
            tlvs = self._recipe_tlvs(recipe, tlvs)
            mode = self._recipe_tag_mode(recipe, index, default_mode)
            built = build_tlv_packet(
                fields,
                tlvs,
                hmac_key,
                tag_mode=mode,
                custom_tag_hex=self.custom_tag.text(),
            )
            gateway_time = (
                min(0xFFFF_FFFF, base_gateway_time + arrival_offset)
                if base_gateway_time is not None
                else None
            )
            metadata = self._vary_live_metadata(base_metadata, index)
            metadata = self._recipe_metadata(recipe, metadata, index)
            result.append(
                (
                    fields.message_sequence,
                    self._wrapper(
                        built.payload_b64,
                        gateway_timestamp=gateway_time,
                        metadata_overrides=metadata,
                    ),
                )
            )
        return result

    @staticmethod
    def _recipe_sequence_offset(recipe: TestRecipe | None, index: int) -> int:
        if recipe is not None and recipe.strategy == "out_of_order":
            return (0, 1, 3, 2, 4, 5)[index]
        return index

    @staticmethod
    def _recipe_tag_mode(
        recipe: TestRecipe | None, index: int, default_mode: str
    ) -> str:
        if recipe is None:
            return default_mode
        if recipe.strategy == "bad_day":
            return "valid" if index in (1, 6) else "corrupt"
        if recipe.strategy == "mixed":
            return "corrupt" if index in (1, 4, 6, 9) else "valid"
        if recipe.strategy == "randomized":
            if index == 0:
                return "valid"
            if index == 1:
                return "corrupt"
            return "valid" if random.random() < 0.6 else "corrupt"
        if recipe.strategy == "all_corrupt":
            return "corrupt"
        return "valid"

    @staticmethod
    def _recipe_tlvs(
        recipe: TestRecipe | None, base_tlvs: list[TlvEntry]
    ) -> list[TlvEntry]:
        if recipe is None or recipe.tlv_mode != "random":
            return base_tlvs
        selected = [entry for entry in base_tlvs if random.random() < 0.6]
        used = sum(2 + len(entry.value) for entry in selected)
        maximum_value_length = min(4, MAX_TLV_SIZE - used - 2)
        if maximum_value_length >= 1 and random.random() < 0.75:
            length = random.randint(1, maximum_value_length)
            selected.append(
                TlvEntry(
                    random.randint(0x70, 0x7D),
                    bytes(random.getrandbits(8) for _ in range(length)),
                    "random_custom",
                )
            )
        return selected

    @staticmethod
    def _recipe_metadata(
        recipe: TestRecipe | None,
        metadata: dict[str, float | None],
        index: int,
    ) -> dict[str, float | None]:
        if recipe is None:
            return metadata
        if recipe.strategy == "radio_fade":
            result = dict(metadata)
            for name, decrement in {
                "link_rssi_dbm": 3.0,
                "link_snr_db": 1.0,
                "cell_rsrp_dbm": 3.0,
                "cell_rsrq_db": 0.7,
                "cell_sinr_db": 1.2,
            }.items():
                value = result.get(name)
                if value is not None:
                    result[name] = round(value - decrement * index, 1)
            return result
        if recipe.strategy == "randomized":
            result = dict(metadata)
            ranges = {
                "link_rssi_dbm": (-135.0, -65.0),
                "link_snr_db": (-10.0, 15.0),
                "cell_rsrp_dbm": (-140.0, -65.0),
                "cell_rsrq_db": (-25.0, -3.0),
                "cell_sinr_db": (-10.0, 25.0),
            }
            for name in result:
                if result[name] is not None:
                    result[name] = round(random.uniform(*ranges[name]), 1)
            return result
        return metadata

    def _base_transport_metadata(self) -> dict[str, float | None]:
        metadata = {
            "link_rssi_dbm": self._optional_float(self.link_rssi.text(), "link RSSI"),
            "link_snr_db": self._optional_float(self.link_snr.text(), "link SNR"),
        }
        if TRANSPORTS[self.transport.currentText()] == "cellular_direct":
            metadata.update(
                {
                    "cell_rsrp_dbm": self._optional_float(self.cell_rsrp.text(), "LTE RSRP"),
                    "cell_rsrq_db": self._optional_float(self.cell_rsrq.text(), "LTE RSRQ"),
                    "cell_sinr_db": self._optional_float(self.cell_sinr.text(), "LTE SINR"),
                }
            )
        return metadata

    @staticmethod
    def _vary_live_metadata(
        base_metadata: dict[str, float | None], index: int
    ) -> dict[str, float | None]:
        if index == 0:
            return dict(base_metadata)
        return {
            name: vary_float(value, *LIVE_RADIO_VARIATION[name])
            for name, value in base_metadata.items()
        }

    @staticmethod
    def _vary_live_tlvs(
        base_tlvs: list[TlvEntry], index: int, elapsed_seconds: int
    ) -> list[TlvEntry]:
        varied: list[TlvEntry] = []
        for entry in base_tlvs:
            if entry.name == "uptime_s":
                value = min(0xFFFF_FFFF, int.from_bytes(entry.value, "little") + elapsed_seconds)
                varied.append(known_tlv(entry.tlv_type, value))
            elif entry.name == "activity_score" and index:
                value = vary_integer(
                    int.from_bytes(entry.value, "little"),
                    *LIVE_INTEGER_VARIATION["activity_score"],
                )
                varied.append(known_tlv(entry.tlv_type, value))
            else:
                varied.append(entry)
        return varied

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
                entry = {
                    "time": time.strftime("%H:%M:%S"),
                    "request": index,
                    "status": status,
                    "message_sequence": sequence,
                    "elapsed_ms": round((time.monotonic() - started) * 1000),
                    "response": response,
                }
            except (OSError, TimeoutError, ValueError) as error:
                entry = {
                    "time": time.strftime("%H:%M:%S"),
                    "request": index,
                    "status": 0,
                    "message_sequence": sequence,
                    "elapsed_ms": round((time.monotonic() - started) * 1000),
                    "error": str(error),
                }
            completed += 1
            self.worker_signals.response.emit(entry)
            if index < len(requests) and self._stop_event.wait(interval):
                break
        self.worker_signals.finished.emit(completed, len(requests), requests)

    def _send_finished(
        self,
        completed: int,
        total: int,
        _requests: object,
    ) -> None:
        self.send_button.setEnabled(True)
        self.stop_button.setEnabled(False)
        stopped = completed < total
        completion_status = (
            f"{'Stopped after' if stopped else 'Completed'} {completed} of {total} request(s)."
        )
        if completed and self.advance_packets.isChecked():
            last_fields = self._prepared_fields[completed - 1]
            self.sequence.setText(str((last_fields.message_sequence + 1) & 0xFFFF))
            self.timestamp.setText(str(int(time.time())))
            self.latitude.setText(f"{last_fields.latitude:.7f}")
            self.longitude.setText(f"{last_fields.longitude:.7f}")
            if TRANSPORTS[self.transport.currentText()] == "lora_hub":
                self._set_gateway_now()
            self.build_packet()
        self.sender_status.setText(completion_status)
        self._prepared_fields = []
        self._worker = None

    def stop_sending(self) -> None:
        self._stop_event.set()
        self.sender_status.setText("Stopping after the current request…")

    @staticmethod
    def _classify_response(
        status: int, response: Any
    ) -> tuple[str, str, str]:
        if status == 0:
            return "🌐", "Network error", "network"
        if 200 <= status < 300:
            if isinstance(response, dict) and response.get("duplicate") is True:
                return "🔁", "Duplicate", "success"
            if status == 201:
                return "✅", "Created", "success"
            return "✅", "Success", "success"
        if 300 <= status < 400:
            return "↪", "Redirect", "redirect"
        if status == 401:
            return "🔒", "Unauthorized", "auth"
        if status == 403:
            return "🔒", "Forbidden", "auth"
        if status == 400:
            return "⚠", "Bad request", "client"
        if status == 409:
            return "⚠", "Conflict", "client"
        if status == 429:
            return "⏳", "Rate limited", "client"
        if 400 <= status < 500:
            return "⚠", "Client error", "client"
        if 500 <= status < 600:
            return "❌", "Server error", "server"
        return "ℹ", "Unexpected", "unknown"

    @staticmethod
    def _response_message(entry: dict[str, Any]) -> str:
        if entry.get("error"):
            return str(entry["error"])
        response = entry.get("response")
        if isinstance(response, dict):
            details: list[str] = []
            for key in ("error", "message", "detail", "code", "stage"):
                value = response.get(key)
                if value not in (None, ""):
                    details.append(str(value))
            codes = response.get("codes")
            if codes:
                details.append(", ".join(str(code) for code in codes))
            if details:
                return " • ".join(details)
            if response.get("duplicate") is True:
                return "Accepted idempotent duplicate"
            if response.get("accepted") is True:
                return "Accepted new telemetry"
        compact = json.dumps(response, separators=(",", ":"), ensure_ascii=False, default=str)
        return compact if len(compact) <= 160 else compact[:157] + "…"

    def _append_response(self, entry: object) -> None:
        if not isinstance(entry, dict):
            return
        status = int(entry.get("status", 0))
        icon, label, style = self._classify_response(status, entry.get("response"))
        background, foreground = RESULT_STYLES[style]
        row = self.response_table.rowCount()
        self.response_table.insertRow(row)
        values = (
            entry.get("time", ""),
            entry.get("request", ""),
            status if status else "—",
            f"{icon}  {label}",
            entry.get("message_sequence", ""),
            entry.get("elapsed_ms", ""),
            self._response_message(entry),
        )
        detail = json.dumps(entry, indent=2, ensure_ascii=False, default=str)
        for column, value in enumerate(values):
            item = QTableWidgetItem(str(value))
            item.setBackground(QBrush(QColor(background)))
            item.setForeground(QBrush(QColor(foreground)))
            item.setToolTip(detail)
            if column == 0:
                item.setData(Qt.ItemDataRole.UserRole, entry)
            self.response_table.setItem(row, column, item)
        self.response_table.selectRow(row)
        self.response_table.scrollToBottom()

    def _response_selected(self) -> None:
        selected = self.response_table.selectedItems()
        if not selected:
            self.response_detail.clear()
            return
        first = self.response_table.item(selected[0].row(), 0)
        entry = first.data(Qt.ItemDataRole.UserRole) if first is not None else None
        self.response_detail.setPlainText(
            json.dumps(entry, indent=2, ensure_ascii=False, default=str)
            if isinstance(entry, dict)
            else ""
        )

    def _clear_responses(self) -> None:
        self.response_table.setRowCount(0)
        self.response_detail.clear()

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
