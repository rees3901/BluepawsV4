#!/usr/bin/env python3
"""Bluepaws desktop console for building and sending v1.1 TLV telemetry."""

from __future__ import annotations

import argparse
import base64
import json
import threading
import time
import tkinter as tk
from dataclasses import replace
from pathlib import Path
from tkinter import filedialog, messagebox, ttk as native_ttk
from typing import Any

try:
    import customtkinter as ctk
except ModuleNotFoundError as error:
    raise SystemExit(
        "CustomTkinter is required. Run: "
        "py -3 -m pip install -r tools\\requirements-gui.txt"
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

NAVY = "#071827"
NAVY_DEEP = "#04111D"
SURFACE = "#0D2438"
SURFACE_RAISED = "#12314A"
BORDER = "#1D4F73"
BLUE = "#1687FF"
BLUE_HOVER = "#0E6FCC"
TEXT = "#F4F8FC"
MUTED = "#91ABC2"
SUCCESS = "#35D0A0"
DANGER = "#FF6B7A"

ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("dark-blue")


class Section(ctk.CTkFrame):
    """Dark card with a compact section heading."""

    def __init__(self, parent: tk.Misc, *, text: str, padding: int = 0) -> None:
        del padding
        super().__init__(
            parent,
            fg_color=SURFACE,
            border_color=BORDER,
            border_width=1,
            corner_radius=12,
        )
        self.grid_rowconfigure(0, minsize=68)
        ctk.CTkLabel(
            self,
            text=text,
            text_color=TEXT,
            font=ctk.CTkFont(size=14, weight="bold"),
        ).place(x=14, y=8)


class AppButton(ctk.CTkButton):
    def __init__(self, parent: tk.Misc, **kwargs: Any) -> None:
        kwargs.setdefault("width", 108)
        kwargs.setdefault("height", 32)
        kwargs.setdefault("corner_radius", 8)
        kwargs.setdefault("fg_color", BLUE)
        kwargs.setdefault("hover_color", BLUE_HOVER)
        super().__init__(parent, **kwargs)


class ScrollableFrame(ctk.CTkScrollableFrame):
    def __init__(self, parent: tk.Misc) -> None:
        super().__init__(
            parent,
            fg_color="transparent",
            scrollbar_button_color=BORDER,
            scrollbar_button_hover_color=BLUE,
        )
        self.content = self


class BluepawsTlvSimulator(ctk.CTk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Bluepaws TLV Telemetry Test Console")
        self.geometry("1220x860")
        self.minsize(980, 700)
        self.protocol("WM_DELETE_WINDOW", self._close)
        self._stop_event = threading.Event()
        self._worker: threading.Thread | None = None
        self._credentials: dict[str, DeviceCredential] = {}
        self._custom_tlvs: dict[str, TlvEntry] = {}
        self._last_built: BuiltPacket | None = None
        self.configure(fg_color=NAVY_DEEP)
        self._configure_native_style()
        self._create_variables()
        self._build_layout()
        self._set_now()
        self._set_gateway_now()
        self._transport_changed()
        self._tag_mode_changed()
        self.after(100, self.build_packet)

    def _configure_native_style(self) -> None:
        """Theme the one native ttk widget CustomTkinter does not supply."""
        style = native_ttk.Style(self)
        style.theme_use("clam")
        style.configure(
            "Treeview",
            background=SURFACE_RAISED,
            fieldbackground=SURFACE_RAISED,
            foreground=TEXT,
            bordercolor=BORDER,
            rowheight=28,
        )
        style.configure(
            "Treeview.Heading",
            background=BORDER,
            foreground=TEXT,
            relief="flat",
        )
        style.map("Treeview", background=[("selected", BLUE)])

    def _create_variables(self) -> None:
        now = int(time.time())
        self.device_id = tk.StringVar(value="1001")
        self.sequence = tk.StringVar(value=str(now & 0xFFFF))
        self.timestamp = tk.StringVar(value=str(now))
        self.status = tk.StringVar(value=STATUS_CHOICES[1])
        self.profile = tk.StringVar(value=PROFILE_CHOICES[1])
        self.reason = tk.StringVar(value=REASON_CHOICES[0])
        self.latitude = tk.StringVar(value="51.907055")
        self.longitude = tk.StringVar(value="-2.256660")
        self.battery_mv = tk.StringVar(value="3900")
        self.accuracy_m = tk.StringVar(value="8")
        self.fix_age_s = tk.StringVar(value="0")
        self.satellites = tk.StringVar(value="9")
        self.flag_vars = {
            name: tk.BooleanVar(value=name in ("GNSS_VALID", "FIX_3D"))
            for name in FLAG_MASKS
        }
        self.known_enabled = {
            "fw_ver": tk.BooleanVar(value=True),
            "reset_reason": tk.BooleanVar(value=False),
            "uptime_s": tk.BooleanVar(value=True),
            "activity_score": tk.BooleanVar(value=True),
            "acked_msg_seq_id": tk.BooleanVar(value=False),
        }
        self.known_values = {
            "fw_ver": tk.StringVar(value="1.1"),
            "reset_reason": tk.StringVar(value="0"),
            "uptime_s": tk.StringVar(value="60"),
            "activity_score": tk.StringVar(value="42"),
            "acked_msg_seq_id": tk.StringVar(value="0"),
        }
        self.custom_type = tk.StringVar(value="7E")
        self.custom_value = tk.StringVar(value="010203")
        self.hmac_key_b64 = tk.StringVar()
        self.show_hmac = tk.BooleanVar(value=False)
        self.tag_mode = tk.StringVar(value=next(iter(TAG_MODES)))
        self.custom_tag = tk.StringVar(value="0000000000000000")
        self.packet_summary = tk.StringVar(value="Packet not built")
        self.builder_status = tk.StringVar(value="Load credentials or enter a Base64 HMAC key.")
        self.credential_choice = tk.StringVar()

        self.endpoint = tk.StringVar(value=DEFAULT_URL)
        self.bearer_token = tk.StringVar()
        self.show_token = tk.BooleanVar(value=False)
        self.transport = tk.StringVar(value=next(iter(TRANSPORTS)))
        self.gateway_guid = tk.StringVar(value="0016")
        self.gateway_rx_time = tk.StringVar(value=str(now))
        self.link_rssi = tk.StringVar(value="-104")
        self.link_snr = tk.StringVar(value="7.0")
        self.cell_rsrp = tk.StringVar(value="-104")
        self.cell_rsrq = tk.StringVar(value="-9.5")
        self.cell_sinr = tk.StringVar(value="7.0")
        self.send_count = tk.StringVar(value="1")
        self.send_interval = tk.StringVar(value="1")
        self.timeout = tk.StringVar(value="15")
        self.advance_packets = tk.BooleanVar(value=False)
        self.sender_status = tk.StringVar(value="Build a packet, preview the wrapper, then send.")

    def _build_layout(self) -> None:
        shell = ctk.CTkFrame(self, fg_color=NAVY_DEEP)
        shell.pack(fill="both", expand=True, padx=16, pady=14)
        header = ctk.CTkFrame(shell, fg_color="transparent")
        header.pack(fill="x", pady=(0, 10))
        ctk.CTkLabel(
            header,
            text="Bluepaws TLV Test Console",
            text_color=TEXT,
            font=ctk.CTkFont(size=22, weight="bold"),
        ).pack(side="left")
        ctk.CTkLabel(
            header,
            text="Protocol v1.1 • local validation • secrets remain on this computer",
            text_color=MUTED,
            font=ctk.CTkFont(size=12),
        ).pack(side="right", pady=(6, 0))

        notebook = ctk.CTkTabview(
            shell,
            fg_color=NAVY,
            segmented_button_fg_color=SURFACE,
            segmented_button_selected_color=BLUE,
            segmented_button_selected_hover_color=BLUE_HOVER,
            segmented_button_unselected_color=SURFACE,
            segmented_button_unselected_hover_color=SURFACE_RAISED,
            corner_radius=14,
        )
        notebook.pack(fill="both", expand=True)
        packet_tab = notebook.add("1. TLV Packet Builder")
        wrapper_tab = notebook.add("2. HTTPS Wrapper & Send")
        self._build_packet_tab(packet_tab)
        self._build_wrapper_tab(wrapper_tab)

    def _build_packet_tab(self, parent: ctk.CTkFrame) -> None:
        scroll = ScrollableFrame(parent)
        scroll.pack(fill="both", expand=True)
        root = scroll.content
        root.columnconfigure(0, weight=1)
        root.columnconfigure(1, weight=1)

        credentials = Section(root, text="Provisioned test credentials", padding=10)
        credentials.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 8))
        credentials.columnconfigure(2, weight=1)
        AppButton(credentials, text="Load credentials JSON…", width=190, command=self.load_credentials_file).grid(row=0, column=0, padx=(14, 10), pady=(26, 8))
        ctk.CTkLabel(credentials, text="Device", text_color=TEXT).grid(row=0, column=1, padx=(0, 6), pady=(26, 8))
        self.credential_combo = ctk.CTkComboBox(
            credentials,
            variable=self.credential_choice,
            state="readonly",
            width=240,
            fg_color=SURFACE_RAISED,
            border_color=BORDER,
            button_color=BLUE,
            button_hover_color=BLUE_HOVER,
        )
        self.credential_combo.grid(row=0, column=2, sticky="w")
        self.credential_combo.bind("<<ComboboxSelected>>", self._credential_selected)
        ctk.CTkLabel(
            credentials,
            text="Bearer and HMAC values are loaded into masked fields; they are never printed.",
            text_color=MUTED,
        ).grid(row=1, column=0, columnspan=3, sticky="w", padx=14, pady=(0, 12))

        header = Section(root, text="Fixed 32-byte header", padding=10)
        header.grid(row=1, column=0, sticky="nsew", padx=(0, 4), pady=4)
        header.columnconfigure(1, weight=1)
        self._form_entry(header, 0, "Protocol version", tk.StringVar(value="1"), disabled=True)
        self._form_entry(header, 1, "Device ID (1–65535)", self.device_id)
        self._form_entry(header, 2, "Message sequence", self.sequence)
        self._form_entry(header, 3, "Unix timestamp", self.timestamp, button=("Now", self._set_now))
        self._form_combo(header, 4, "Status", self.status, STATUS_CHOICES)
        self._form_combo(header, 5, "Power profile", self.profile, PROFILE_CHOICES)
        self._form_combo(header, 6, "TX reason", self.reason, REASON_CHOICES)

        location = Section(root, text="Position and telemetry", padding=10)
        location.grid(row=1, column=1, sticky="nsew", padx=(4, 0), pady=4)
        location.columnconfigure(1, weight=1)
        self._form_entry(location, 0, "Latitude", self.latitude)
        self._form_entry(location, 1, "Longitude", self.longitude)
        self._form_entry(location, 2, "Battery (mV)", self.battery_mv)
        self._form_entry(location, 3, "Accuracy (m)", self.accuracy_m)
        self._form_entry(location, 4, "Fix age (s)", self.fix_age_s)
        self._form_entry(location, 5, "Satellite count", self.satellites)
        ctk.CTkLabel(
            location,
            text="Use fix age 65535 or satellites 255 for unknown.",
            text_color=MUTED,
            wraplength=420,
        ).grid(row=6, column=0, columnspan=3, sticky="w", padx=12, pady=(8, 12))

        flags = Section(root, text="Header flags", padding=10)
        flags.grid(row=2, column=0, columnspan=2, sticky="ew", pady=4)
        for index, (name, mask) in enumerate(FLAG_MASKS.items()):
            ctk.CTkCheckBox(
                flags,
                text=f"{name} (0x{mask:02X})",
                variable=self.flag_vars[name],
                fg_color=BLUE,
                hover_color=BLUE_HOVER,
                border_color=BORDER,
            ).grid(row=index // 4, column=index % 4, sticky="w", padx=(14, 18), pady=(28 if index < 4 else 5, 10))

        known = Section(root, text="Selected v1.1 TLVs (24-byte total budget)", padding=10)
        known.grid(row=3, column=0, sticky="nsew", padx=(0, 4), pady=4)
        known.columnconfigure(2, weight=1)
        specs = [
            ("fw_ver", "0x04", "major.minor"),
            ("reset_reason", "0x06", "0–255"),
            ("uptime_s", "0x10", "0–4294967295"),
            ("activity_score", "0x13", "0–255"),
            ("acked_msg_seq_id", "0x20", "0–65535"),
        ]
        for row, (name, code, hint) in enumerate(specs):
            ctk.CTkCheckBox(
                known,
                text=f"{code}  {name}",
                variable=self.known_enabled[name],
                fg_color=BLUE,
                hover_color=BLUE_HOVER,
                border_color=BORDER,
            ).grid(row=row, column=0, sticky="w", padx=(14, 10), pady=(28 if row == 0 else 4, 4))
            ctk.CTkEntry(
                known,
                textvariable=self.known_values[name],
                width=145,
                fg_color=SURFACE_RAISED,
                border_color=BORDER,
            ).grid(row=row, column=1, sticky="ew", padx=(0, 8), pady=(28 if row == 0 else 4, 4))
            ctk.CTkLabel(known, text=hint, text_color=MUTED).grid(row=row, column=2, sticky="w", padx=(0, 12), pady=(28 if row == 0 else 4, 4))

        custom = Section(root, text="Custom / unknown TLVs", padding=10)
        custom.grid(row=3, column=1, sticky="nsew", padx=(4, 0), pady=4)
        custom.columnconfigure(1, weight=1)
        ctk.CTkLabel(custom, text="Type (hex)", text_color=TEXT).grid(row=0, column=0, sticky="w", padx=(14, 0), pady=(28, 4))
        ctk.CTkEntry(custom, textvariable=self.custom_type, width=90, fg_color=SURFACE_RAISED, border_color=BORDER).grid(row=0, column=1, sticky="ew", padx=(6, 8), pady=(28, 4))
        ctk.CTkLabel(custom, text="Value bytes (hex)", text_color=TEXT).grid(row=1, column=0, sticky="w", padx=(14, 0), pady=(6, 0))
        ctk.CTkEntry(custom, textvariable=self.custom_value, fg_color=SURFACE_RAISED, border_color=BORDER).grid(row=1, column=1, sticky="ew", padx=(6, 8), pady=(6, 0))
        AppButton(custom, text="Add TLV", command=self._add_custom_tlv).grid(row=0, column=2, rowspan=2, sticky="ns", padx=(0, 12), pady=(28, 0))
        self.custom_tree = native_ttk.Treeview(custom, columns=("type", "length", "value"), show="headings", height=4)
        for column, title, width in (("type", "Type", 70), ("length", "Bytes", 65), ("value", "Value hex", 230)):
            self.custom_tree.heading(column, text=title)
            self.custom_tree.column(column, width=width, anchor="w")
        self.custom_tree.grid(row=2, column=0, columnspan=3, sticky="nsew", padx=14, pady=(9, 5))
        AppButton(custom, text="Remove selected", fg_color=SURFACE_RAISED, hover_color=BORDER, command=self._remove_custom_tlv).grid(row=3, column=2, sticky="e", padx=14, pady=(0, 12))

        auth = Section(root, text="HMAC-SHA256 authentication (first 8 bytes)", padding=10)
        auth.grid(row=4, column=0, columnspan=2, sticky="ew", pady=4)
        auth.columnconfigure(1, weight=1)
        ctk.CTkLabel(auth, text="32-byte key (Base64)", text_color=TEXT).grid(row=0, column=0, sticky="w", padx=(14, 8), pady=(28, 4))
        self.hmac_entry = ctk.CTkEntry(auth, textvariable=self.hmac_key_b64, show="•", fg_color=SURFACE_RAISED, border_color=BORDER)
        self.hmac_entry.grid(row=0, column=1, sticky="ew", pady=(28, 4))
        ctk.CTkCheckBox(auth, text="Show", variable=self.show_hmac, command=self._toggle_hmac, fg_color=BLUE, hover_color=BLUE_HOVER, border_color=BORDER).grid(row=0, column=2, padx=(8, 14), pady=(28, 4))
        ctk.CTkLabel(auth, text="Tag mode", text_color=TEXT).grid(row=1, column=0, sticky="w", padx=(14, 8), pady=(7, 12))
        tag_combo = ctk.CTkComboBox(
            auth,
            variable=self.tag_mode,
            values=list(TAG_MODES),
            state="readonly",
            fg_color=SURFACE_RAISED,
            border_color=BORDER,
            button_color=BLUE,
            button_hover_color=BLUE_HOVER,
        )
        tag_combo.grid(row=1, column=1, sticky="ew", pady=(7, 0))
        tag_combo.bind("<<ComboboxSelected>>", lambda _event: self._tag_mode_changed())
        self.custom_tag_entry = ctk.CTkEntry(auth, textvariable=self.custom_tag, width=190, fg_color=SURFACE_RAISED, border_color=BORDER)
        self.custom_tag_entry.grid(row=1, column=2, padx=(8, 14), pady=(7, 12))

        actions = ctk.CTkFrame(root, fg_color="transparent")
        actions.grid(row=5, column=0, columnspan=2, sticky="ew", pady=(8, 4))
        AppButton(actions, text="Build and validate packet", width=210, height=38, command=self.build_packet).pack(side="left")
        ctk.CTkLabel(actions, textvariable=self.builder_status, text_color=MUTED).pack(side="left", padx=12)

        output = Section(root, text="Packet output", padding=10)
        output.grid(row=6, column=0, columnspan=2, sticky="nsew", pady=4)
        output.columnconfigure(0, weight=1)
        ctk.CTkLabel(output, textvariable=self.packet_summary, text_color=SUCCESS).grid(row=0, column=0, sticky="w", padx=14, pady=(28, 6))
        AppButton(output, text="Copy Base64", command=lambda: self._copy_text(self.packet_b64)).grid(row=0, column=1, padx=(6, 0), pady=(28, 6))
        AppButton(output, text="Copy hex", command=lambda: self._copy_text(self.packet_hex)).grid(row=0, column=2, padx=(6, 14), pady=(28, 6))
        ctk.CTkLabel(output, text="Packet Base64", text_color=TEXT).grid(row=1, column=0, sticky="w", padx=14)
        self.packet_b64 = ctk.CTkTextbox(output, height=72, wrap="word", font=("Consolas", 12), fg_color=SURFACE_RAISED, border_color=BORDER, border_width=1)
        self.packet_b64.grid(row=2, column=0, columnspan=3, sticky="ew", padx=14, pady=(3, 8))
        ctk.CTkLabel(output, text="Packet hex", text_color=TEXT).grid(row=3, column=0, sticky="w", padx=14)
        self.packet_hex = ctk.CTkTextbox(output, height=96, wrap="word", font=("Consolas", 12), fg_color=SURFACE_RAISED, border_color=BORDER, border_width=1)
        self.packet_hex.grid(row=4, column=0, columnspan=3, sticky="ew", padx=14, pady=(3, 14))

    def _build_wrapper_tab(self, parent: ctk.CTkFrame) -> None:
        parent.columnconfigure(0, weight=1)
        parent.columnconfigure(1, weight=1)
        parent.rowconfigure(1, weight=1)

        settings = Section(parent, text="HTTPS request", padding=12)
        settings.grid(row=0, column=0, sticky="nsew", padx=(10, 5), pady=10)
        settings.columnconfigure(1, weight=1)
        self._form_entry(settings, 0, "Supabase endpoint", self.endpoint)
        ctk.CTkLabel(settings, text="Transport", text_color=TEXT).grid(row=1, column=0, sticky="w", padx=(14, 8), pady=4)
        transport_combo = ctk.CTkComboBox(
            settings,
            variable=self.transport,
            values=list(TRANSPORTS),
            state="readonly",
            fg_color=SURFACE_RAISED,
            border_color=BORDER,
            button_color=BLUE,
            button_hover_color=BLUE_HOVER,
        )
        transport_combo.grid(row=1, column=1, columnspan=2, sticky="ew", pady=4)
        transport_combo.bind("<<ComboboxSelected>>", lambda _event: self._transport_changed())
        ctk.CTkLabel(settings, text="Bearer token", text_color=TEXT).grid(row=2, column=0, sticky="w", padx=(14, 8), pady=4)
        self.token_entry = ctk.CTkEntry(settings, textvariable=self.bearer_token, show="•", fg_color=SURFACE_RAISED, border_color=BORDER)
        self.token_entry.grid(row=2, column=1, sticky="ew", pady=4)
        ctk.CTkCheckBox(settings, text="Show", variable=self.show_token, command=self._toggle_token, fg_color=BLUE, hover_color=BLUE_HOVER, border_color=BORDER).grid(row=2, column=2, padx=(8, 14))
        ctk.CTkLabel(
            settings,
            text="Use the device token for LTE; use the provisioned gateway token for LoRa.",
            text_color=MUTED,
            wraplength=520,
        ).grid(row=3, column=0, columnspan=3, sticky="w", padx=14, pady=(2, 12))

        metadata = Section(parent, text="Transport metadata", padding=12)
        metadata.grid(row=0, column=1, sticky="nsew", padx=(5, 10), pady=10)
        metadata.columnconfigure(1, weight=1)
        self.gateway_widgets: list[ctk.CTkEntry | ctk.CTkButton] = []
        self.cell_widgets: list[ctk.CTkEntry] = []
        self._metadata_entry(metadata, 0, "Gateway GUID16", self.gateway_guid, "gateway")
        self._metadata_entry(metadata, 1, "Gateway RX Unix", self.gateway_rx_time, "gateway", button=("Now", self._set_gateway_now))
        self._metadata_entry(metadata, 2, "Link RSSI (dBm)", self.link_rssi, "common")
        self._metadata_entry(metadata, 3, "Link SNR (dB)", self.link_snr, "common")
        self._metadata_entry(metadata, 4, "LTE RSRP (dBm)", self.cell_rsrp, "cell")
        self._metadata_entry(metadata, 5, "LTE RSRQ (dB)", self.cell_rsrq, "cell")
        self._metadata_entry(metadata, 6, "LTE SINR (dB)", self.cell_sinr, "cell")
        ctk.CTkLabel(metadata, text="Leave optional RF fields blank to omit them from JSON.", text_color=MUTED).grid(row=7, column=0, columnspan=3, sticky="w", padx=14, pady=(6, 12))

        preview_frame = Section(parent, text="JSON wrapper preview (Authorization header intentionally omitted)", padding=10)
        preview_frame.grid(row=1, column=0, sticky="nsew", padx=(10, 5), pady=(0, 10))
        preview_frame.columnconfigure(0, weight=1)
        preview_frame.rowconfigure(1, weight=1)
        preview_actions = ctk.CTkFrame(preview_frame, fg_color="transparent")
        preview_actions.grid(row=0, column=0, sticky="ew", padx=14, pady=(28, 6))
        AppButton(preview_actions, text="Refresh preview", width=140, command=self.preview_wrapper).pack(side="left")
        AppButton(preview_actions, text="Copy JSON", fg_color=SURFACE_RAISED, hover_color=BORDER, command=lambda: self._copy_text(self.wrapper_preview)).pack(side="left", padx=6)
        self.wrapper_preview = ctk.CTkTextbox(preview_frame, wrap="word", font=("Consolas", 12), fg_color=SURFACE_RAISED, border_color=BORDER, border_width=1)
        self.wrapper_preview.grid(row=1, column=0, sticky="nsew", padx=14, pady=(0, 14))

        sender = Section(parent, text="Send and response log", padding=10)
        sender.grid(row=1, column=1, sticky="nsew", padx=(5, 10), pady=(0, 10))
        sender.columnconfigure(1, weight=1)
        sender.rowconfigure(4, weight=1)
        controls = ctk.CTkFrame(sender, fg_color="transparent")
        controls.grid(row=0, column=0, columnspan=3, sticky="ew", padx=14, pady=(28, 4))
        ctk.CTkLabel(controls, text="Count", text_color=TEXT).pack(side="left")
        ctk.CTkEntry(controls, textvariable=self.send_count, width=65, fg_color=SURFACE_RAISED, border_color=BORDER).pack(side="left", padx=(5, 12))
        ctk.CTkLabel(controls, text="Interval (s)", text_color=TEXT).pack(side="left")
        ctk.CTkEntry(controls, textvariable=self.send_interval, width=65, fg_color=SURFACE_RAISED, border_color=BORDER).pack(side="left", padx=(5, 12))
        ctk.CTkLabel(controls, text="Timeout (s)", text_color=TEXT).pack(side="left")
        ctk.CTkEntry(controls, textvariable=self.timeout, width=65, fg_color=SURFACE_RAISED, border_color=BORDER).pack(side="left", padx=(5, 0))
        ctk.CTkCheckBox(sender, text="Advance sequence and timestamp for each request", variable=self.advance_packets, fg_color=BLUE, hover_color=BLUE_HOVER, border_color=BORDER).grid(row=1, column=0, columnspan=3, sticky="w", padx=14, pady=(8, 6))
        buttons = ctk.CTkFrame(sender, fg_color="transparent")
        buttons.grid(row=2, column=0, columnspan=3, sticky="ew")
        self.send_button = AppButton(buttons, text="Send", command=self.send_requests)
        self.send_button.pack(side="left")
        self.stop_button = AppButton(buttons, text="Stop", fg_color=DANGER, hover_color="#D94B5B", command=self.stop_sending, state="disabled")
        self.stop_button.pack(side="left", padx=6)
        AppButton(buttons, text="Clear log", fg_color=SURFACE_RAISED, hover_color=BORDER, command=self._clear_log).pack(side="right")
        ctk.CTkLabel(sender, textvariable=self.sender_status, text_color=MUTED).grid(row=3, column=0, columnspan=3, sticky="w", padx=14, pady=(7, 4))
        self.response_log = ctk.CTkTextbox(sender, wrap="word", font=("Consolas", 12), fg_color=SURFACE_RAISED, border_color=BORDER, border_width=1)
        self.response_log.grid(row=4, column=0, columnspan=3, sticky="nsew", padx=14, pady=(0, 14))

    def _form_entry(
        self,
        parent: Section,
        row: int,
        label: str,
        variable: tk.StringVar,
        *,
        disabled: bool = False,
        button: tuple[str, Any] | None = None,
    ) -> ctk.CTkEntry:
        top_pad = 28 if row == 0 else 4
        ctk.CTkLabel(parent, text=label, text_color=TEXT).grid(
            row=row,
            column=0,
            sticky="w",
            pady=(top_pad, 4),
            padx=(14, 8),
        )
        entry = ctk.CTkEntry(
            parent,
            textvariable=variable,
            state="disabled" if disabled else "normal",
            fg_color=SURFACE_RAISED,
            border_color=BORDER,
        )
        entry.grid(row=row, column=1, sticky="ew", pady=(top_pad, 4))
        if button:
            AppButton(parent, text=button[0], width=68, command=button[1]).grid(
                row=row,
                column=2,
                padx=(7, 14),
                pady=(top_pad, 4),
            )
        return entry

    def _form_combo(self, parent: Section, row: int, label: str, variable: tk.StringVar, values: list[str]) -> None:
        top_pad = 28 if row == 0 else 4
        ctk.CTkLabel(parent, text=label, text_color=TEXT).grid(
            row=row,
            column=0,
            sticky="w",
            pady=(top_pad, 4),
            padx=(14, 8),
        )
        ctk.CTkComboBox(
            parent,
            variable=variable,
            values=values,
            state="readonly",
            fg_color=SURFACE_RAISED,
            border_color=BORDER,
            button_color=BLUE,
            button_hover_color=BLUE_HOVER,
        ).grid(row=row, column=1, columnspan=2, sticky="ew", padx=(0, 14), pady=(top_pad, 4))

    def _metadata_entry(
        self,
        parent: Section,
        row: int,
        label: str,
        variable: tk.StringVar,
        group: str,
        *,
        button: tuple[str, Any] | None = None,
    ) -> None:
        top_pad = 28 if row == 0 else 3
        ctk.CTkLabel(parent, text=label, text_color=TEXT).grid(
            row=row,
            column=0,
            sticky="w",
            pady=(top_pad, 3),
            padx=(14, 8),
        )
        entry = ctk.CTkEntry(
            parent,
            textvariable=variable,
            fg_color=SURFACE_RAISED,
            border_color=BORDER,
        )
        entry.grid(row=row, column=1, sticky="ew", pady=(top_pad, 3))
        if group == "gateway":
            self.gateway_widgets.append(entry)
        elif group == "cell":
            self.cell_widgets.append(entry)
        if button:
            control = AppButton(parent, text=button[0], width=68, command=button[1])
            control.grid(row=row, column=2, padx=(7, 14), pady=(top_pad, 3))
            if group == "gateway":
                self.gateway_widgets.append(control)

    def load_credentials_file(self) -> None:
        selected = filedialog.askopenfilename(
            title="Load Bluepaws test credentials",
            filetypes=(("JSON files", "*.json"), ("All files", "*.*")),
        )
        if not selected:
            return
        try:
            credentials = load_credentials(Path(selected))
        except (OSError, ValueError, json.JSONDecodeError) as error:
            messagebox.showerror("Unable to load credentials", str(error), parent=self)
            return
        self._credentials = {f"Device {item.device_id}": item for item in credentials}
        choices = list(self._credentials)
        self.credential_combo.configure(values=choices)
        self.credential_choice.set(choices[0])
        self._apply_credential(self._credentials[choices[0]])
        self.builder_status.set(f"Loaded {len(credentials)} provisioned test device(s).")

    def _credential_selected(self, _event: tk.Event[Any]) -> None:
        credential = self._credentials.get(self.credential_choice.get())
        if credential:
            self._apply_credential(credential)

    def _apply_credential(self, credential: DeviceCredential) -> None:
        self.device_id.set(str(credential.device_id))
        self.hmac_key_b64.set(base64.b64encode(credential.hmac_key).decode("ascii"))
        self.bearer_token.set(credential.token)
        self.builder_status.set(f"Device {credential.device_id} selected. Secrets remain masked.")
        self.build_packet()

    def _set_now(self) -> None:
        self.timestamp.set(str(int(time.time())))

    def _set_gateway_now(self) -> None:
        self.gateway_rx_time.set(str(int(time.time())))

    def _toggle_hmac(self) -> None:
        self.hmac_entry.configure(show="" if self.show_hmac.get() else "•")

    def _toggle_token(self) -> None:
        self.token_entry.configure(show="" if self.show_token.get() else "•")

    def _tag_mode_changed(self) -> None:
        state = "normal" if TAG_MODES[self.tag_mode.get()] == "custom" else "disabled"
        self.custom_tag_entry.configure(state=state)

    def _transport_changed(self) -> None:
        is_lora = TRANSPORTS[self.transport.get()] == "lora_hub"
        for widget in self.gateway_widgets:
            widget.configure(state="normal" if is_lora else "disabled")
        for widget in self.cell_widgets:
            widget.configure(state="disabled" if is_lora else "normal")
        self.preview_wrapper()

    def _add_custom_tlv(self) -> None:
        try:
            entry = custom_tlv(self.custom_type.get(), self.custom_value.get())
        except ValueError as error:
            messagebox.showerror("Invalid custom TLV", str(error), parent=self)
            return
        item = self.custom_tree.insert("", "end", values=(f"0x{entry.tlv_type:02X}", len(entry.value), entry.value.hex().upper()))
        self._custom_tlvs[item] = entry
        self.build_packet()

    def _remove_custom_tlv(self) -> None:
        for item in self.custom_tree.selection():
            self._custom_tlvs.pop(item, None)
            self.custom_tree.delete(item)
        self.build_packet()

    def _packet_fields(self) -> PacketFields:
        flags = sum(mask for name, mask in FLAG_MASKS.items() if self.flag_vars[name].get())
        return PacketFields(
            device_id=self._int(self.device_id.get(), "device ID"),
            message_sequence=self._int(self.sequence.get(), "message sequence"),
            timestamp=self._int(self.timestamp.get(), "Unix timestamp"),
            status=self._choice_code(self.status.get()),
            power_profile=self._choice_code(self.profile.get()),
            flags=flags,
            tx_reason=self._choice_code(self.reason.get()),
            latitude=self._float(self.latitude.get(), "latitude"),
            longitude=self._float(self.longitude.get(), "longitude"),
            battery_mv=self._int(self.battery_mv.get(), "battery millivolts"),
            accuracy_m=self._int(self.accuracy_m.get(), "accuracy metres"),
            fix_age_s=self._int(self.fix_age_s.get(), "fix age seconds"),
            satellite_count=self._int(self.satellites.get(), "satellite count"),
        )

    def _packet_tlvs(self) -> list[TlvEntry]:
        result: list[TlvEntry] = []
        if self.known_enabled["fw_ver"].get():
            result.append(firmware_tlv(self.known_values["fw_ver"].get()))
        mappings = (
            ("reset_reason", 0x06),
            ("uptime_s", 0x10),
            ("activity_score", 0x13),
            ("acked_msg_seq_id", 0x20),
        )
        for name, tlv_type in mappings:
            if self.known_enabled[name].get():
                result.append(known_tlv(tlv_type, self._int(self.known_values[name].get(), name)))
        result.extend(self._custom_tlvs.values())
        return result

    def _build_from_fields(self, fields: PacketFields | None = None) -> BuiltPacket:
        return build_tlv_packet(
            fields or self._packet_fields(),
            self._packet_tlvs(),
            decode_hmac_key(self.hmac_key_b64.get()),
            tag_mode=TAG_MODES[self.tag_mode.get()],
            custom_tag_hex=self.custom_tag.get(),
        )

    def build_packet(self) -> BuiltPacket | None:
        try:
            built = self._build_from_fields()
        except ValueError as error:
            self._last_built = None
            self.packet_summary.set("Packet not built")
            self.builder_status.set(str(error))
            return None
        self._last_built = built
        self._replace_text(self.packet_b64, built.payload_b64)
        self._replace_text(self.packet_hex, self._group_hex(built.packet_hex))
        tag_note = "valid" if built.transmitted_tag == built.expected_tag else "intentionally invalid"
        self.packet_summary.set(
            f"{len(built.packet)} bytes • TLVs {built.tlv_length}/24 bytes • tag {built.transmitted_tag.hex().upper()} ({tag_note}) • SHA-256 {built.payload_hash[:16]}…"
        )
        self.builder_status.set("Packet passes local structure validation.")
        if hasattr(self, "wrapper_preview"):
            self.preview_wrapper()
        return built

    def _wrapper(self, payload_b64: str | None = None, *, gateway_timestamp: int | None = None) -> dict[str, Any]:
        transport = TRANSPORTS[self.transport.get()]
        payload = payload_b64 if payload_b64 is not None else self._text(self.packet_b64)
        common = {
            "link_rssi_dbm": self._optional_float(self.link_rssi.get(), "link RSSI"),
            "link_snr_db": self._optional_float(self.link_snr.get(), "link SNR"),
        }
        if transport == "lora_hub":
            return build_transport_wrapper(
                payload,
                transport,
                gateway_guid16=self.gateway_guid.get(),
                gateway_rx_time_unix=gateway_timestamp if gateway_timestamp is not None else self._int(self.gateway_rx_time.get(), "gateway receive timestamp"),
                **common,
            )
        return build_transport_wrapper(
            payload,
            transport,
            cell_rsrp_dbm=self._optional_float(self.cell_rsrp.get(), "LTE RSRP"),
            cell_rsrq_db=self._optional_float(self.cell_rsrq.get(), "LTE RSRQ"),
            cell_sinr_db=self._optional_float(self.cell_sinr.get(), "LTE SINR"),
            **common,
        )

    def preview_wrapper(self) -> dict[str, Any] | None:
        if not hasattr(self, "wrapper_preview"):
            return None
        try:
            wrapper = self._wrapper()
        except ValueError as error:
            self.sender_status.set(str(error))
            return None
        self._replace_text(self.wrapper_preview, json.dumps(wrapper, indent=2))
        self.sender_status.set("Wrapper passes local validation. Authorization header is not shown.")
        return wrapper

    def send_requests(self) -> None:
        if self._worker and self._worker.is_alive():
            return
        try:
            count = self._int(self.send_count.get(), "send count")
            if not 1 <= count <= 1000:
                raise ValueError("send count must be from 1 to 1000")
            interval = self._float(self.send_interval.get(), "send interval")
            timeout = self._float(self.timeout.get(), "HTTP timeout")
            if not 0 <= interval <= 3600:
                raise ValueError("send interval must be from 0 to 3600 seconds")
            if not 0 < timeout <= 300:
                raise ValueError("HTTP timeout must be from 0 to 300 seconds")
            endpoint = self.endpoint.get().strip()
            token = self.bearer_token.get().strip()
            requests = self._prepare_requests(count, interval)
            # Validate endpoint/token before starting a worker by using the same
            # public constraints as post_wrapper without making a request.
            if not endpoint.lower().startswith("https://"):
                raise ValueError("Supabase endpoint must use HTTPS")
            if not 32 <= len(token) <= 256:
                raise ValueError("bearer token must contain 32..256 characters")
        except ValueError as error:
            messagebox.showerror("Cannot send", str(error), parent=self)
            return

        self._stop_event.clear()
        self.send_button.configure(state="disabled")
        self.stop_button.configure(state="normal")
        self.sender_status.set(f"Sending {count} request(s)…")
        self._worker = threading.Thread(
            target=self._send_worker,
            args=(endpoint, token, requests, interval, timeout),
            daemon=True,
        )
        self._worker.start()

    def _prepare_requests(self, count: int, interval: float) -> list[tuple[int, dict[str, Any]]]:
        if not self.advance_packets.get():
            wrapper = self._wrapper()
            sequence = self._int(self.sequence.get(), "message sequence")
            return [(sequence, wrapper) for _ in range(count)]

        base_fields = self._packet_fields()
        hmac_key = decode_hmac_key(self.hmac_key_b64.get())
        tlvs = self._packet_tlvs()
        mode = TAG_MODES[self.tag_mode.get()]
        base_gateway_time = self._int(self.gateway_rx_time.get(), "gateway receive timestamp") if TRANSPORTS[self.transport.get()] == "lora_hub" else None
        result: list[tuple[int, dict[str, Any]]] = []
        for index in range(count):
            offset = round(index * interval)
            fields = replace(
                base_fields,
                message_sequence=(base_fields.message_sequence + index) & 0xFFFF,
                timestamp=min(0xFFFF_FFFF, base_fields.timestamp + offset),
            )
            built = build_tlv_packet(fields, tlvs, hmac_key, tag_mode=mode, custom_tag_hex=self.custom_tag.get())
            gateway_time = min(0xFFFF_FFFF, base_gateway_time + offset) if base_gateway_time is not None else None
            result.append((fields.message_sequence, self._wrapper(built.payload_b64, gateway_timestamp=gateway_time)))
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
                status, response = post_wrapper(endpoint, token, wrapper, timeout, user_agent="bluepaws-tlv-desktop-console/1")
                elapsed_ms = round((time.monotonic() - started) * 1000)
                line = json.dumps(
                    {
                        "request": index,
                        "status": status,
                        "message_sequence": sequence,
                        "elapsed_ms": elapsed_ms,
                        "response": response,
                    },
                    separators=(",", ":"),
                )
            except (OSError, TimeoutError, ValueError) as error:
                line = json.dumps(
                    {"request": index, "status": 0, "message_sequence": sequence, "error": str(error)},
                    separators=(",", ":"),
                )
            completed += 1
            self.after(0, self._append_log, line)
            if index < len(requests) and self._stop_event.wait(interval):
                break
        self.after(0, self._send_finished, completed, len(requests), requests)

    def _send_finished(self, completed: int, total: int, requests: list[tuple[int, dict[str, Any]]]) -> None:
        self.send_button.configure(state="normal")
        self.stop_button.configure(state="disabled")
        stopped = completed < total
        self.sender_status.set(f"{'Stopped after' if stopped else 'Completed'} {completed} of {total} request(s).")
        if completed and self.advance_packets.get():
            self.sequence.set(str((requests[completed - 1][0] + 1) & 0xFFFF))
            self._set_now()
        self._worker = None

    def stop_sending(self) -> None:
        self._stop_event.set()
        self.sender_status.set("Stopping after the current request…")

    def _append_log(self, line: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        self.response_log.insert("end", f"[{stamp}] {line}\n")
        self.response_log.see("end")

    def _clear_log(self) -> None:
        self.response_log.delete("1.0", "end")

    def _copy_text(self, widget: ctk.CTkTextbox) -> None:
        value = self._text(widget)
        if not value:
            return
        self.clipboard_clear()
        self.clipboard_append(value)
        self.update_idletasks()

    @staticmethod
    def _replace_text(widget: ctk.CTkTextbox, value: str) -> None:
        widget.delete("1.0", "end")
        widget.insert("1.0", value)

    @staticmethod
    def _text(widget: ctk.CTkTextbox) -> str:
        return widget.get("1.0", "end-1c").strip()

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

    def _close(self) -> None:
        self._stop_event.set()
        self.destroy()

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Bluepaws TLV desktop test console")
    parser.add_argument("--check", action="store_true", help="check imports without opening a window")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.check:
        print(f"Bluepaws TLV GUI dependencies available (Tk {tk.TkVersion}).")
        return 0
    app = BluepawsTlvSimulator()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
