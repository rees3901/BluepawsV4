#!/usr/bin/env python3
"""
Bluepaws V4 collar ↔ Home Hub command-pipeline test harness.

This is a bench-only helper for the current hardware setup:
  - Collar / RAK4631 testbed serial monitor, default COM23
  - Home Hub / Heltec Tracker V2 serial monitor, default COM7
  - T190 LoRa sniffer serial monitor, default COM11

It does not provision devices and it does not send secrets. It watches the
serial logs, queues commands through the Home Hub HTTP API, and optionally
nudges the collar into an immediate TX/RX cycle via its debug serial console.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import queue
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Callable, Iterable

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

try:
    import serial  # type: ignore
except ImportError:  # pragma: no cover - friendly runtime message
    serial = None


DEFAULT_BAUD = 115200


@dataclasses.dataclass
class Event:
    source: str
    text: str
    timestamp: float


class SerialMonitor:
    def __init__(self, label: str, port: str, baud: int, events: "queue.Queue[Event]") -> None:
        if serial is None:
            raise RuntimeError("pyserial is not installed. Run: py -3.11 -m pip install pyserial")
        self.label = label
        self.port = port
        self.baud = baud
        self.events = events
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._serial = None

    def start(self) -> None:
        try:
            self._serial = serial.Serial(self.port, self.baud, timeout=0.2)
        except Exception as exc:
            raise RuntimeError(f"Failed to open {self.label} serial port {self.port}: {exc}") from exc
        self._thread = threading.Thread(target=self._run, name=f"serial-{self.label}", daemon=True)
        self._thread.start()
        print(f"[{self.label}] Opened {self.port} at {self.baud} baud.")

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1.0)
        if self._serial:
            self._serial.close()

    def write_line(self, line: str) -> None:
        if not self._serial:
            raise RuntimeError(f"{self.label} serial monitor is not open")
        payload = (line.rstrip("\r\n") + "\n").encode("utf-8")
        self._serial.write(payload)
        self._serial.flush()
        print(f"[{self.label} ->] {line}")

    def _run(self) -> None:
        assert self._serial is not None
        while not self._stop.is_set():
            try:
                raw = self._serial.readline()
            except Exception as exc:  # pragma: no cover - hardware failure path
                self.events.put(Event(self.label, f"<serial read error: {exc}>", time.time()))
                return
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if text:
                event = Event(self.label, text, time.time())
                self.events.put(event)
                print(f"[{event.source}] {event.text}")


class EventLog:
    def __init__(self) -> None:
        self._events: list[Event] = []
        self._queue: "queue.Queue[Event]" = queue.Queue()

    @property
    def queue(self) -> "queue.Queue[Event]":
        return self._queue

    def drain(self) -> None:
        while True:
            try:
                self._events.append(self._queue.get_nowait())
            except queue.Empty:
                return

    def wait_for(self, source: str | None, patterns: Iterable[str], timeout: float, since: float) -> bool:
        compiled = [re.compile(pattern, re.IGNORECASE) for pattern in patterns]
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.drain()
            for event in self._events:
                if event.timestamp < since:
                    continue
                if source is not None and event.source != source:
                    continue
                if all(pattern.search(event.text) for pattern in compiled):
                    return True
            time.sleep(0.1)
        self.drain()
        return False


def post_form(base_url: str, path: str, data: dict[str, str], timeout: float) -> tuple[int | None, str]:
    url = base_url.rstrip("/") + path
    body = urllib.parse.urlencode(data).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Accept": "application/json",
            "User-Agent": "bluepaws-command-pipeline-test/1",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            text = response.read().decode("utf-8", errors="replace")
            return response.status, text
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", errors="replace")
    except Exception as exc:
        return None, str(exc)


def get_json(base_url: str, path: str, timeout: float) -> tuple[int | None, str]:
    url = base_url.rstrip("/") + path
    request = urllib.request.Request(url, method="GET", headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            text = response.read().decode("utf-8", errors="replace")
            return response.status, text
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", errors="replace")
    except Exception as exc:
        return None, str(exc)


def normalize_hub_url(raw_url: str) -> str:
    """Accept plain URLs and Markdown links accidentally pasted from chat."""
    value = raw_url.strip()

    markdown_match = re.fullmatch(r"\[(https?://[^\]]+)\]\(https?://[^)]+\)", value)
    if markdown_match:
        value = markdown_match.group(1)

    value = value.strip("<>")
    return value.rstrip("/")


def wait_for_hub_settle(events: EventLog, seconds: float) -> None:
    """Give the ESP32 hub time to recover if opening serial toggled reset."""
    if seconds <= 0:
        return
    print()
    print(f"[INFO] Waiting {seconds:g}s for hub serial/Wi-Fi to settle...")
    deadline = time.time() + seconds
    while time.time() < deadline:
        events.drain()
        time.sleep(0.2)


def hex_device_id(device_id: int) -> str:
    if not 1 <= device_id <= 65535:
        raise ValueError("--target-id must be 1..65535")
    return f"{device_id:04X}"


def run_step(
    name: str,
    action: Callable[[], bool | None],
    checks: list[tuple[str | None, list[str], float]],
    events: EventLog,
) -> bool:
    print()
    print(f"=== {name} ===")
    start = time.time()
    action_ok = action()
    if action_ok is False:
        print("❌ Step action failed; skipping waits that cannot succeed.")
        return False
    passed = True
    for source, patterns, timeout in checks:
        ok = events.wait_for(source, patterns, timeout, start)
        label = f"{source or 'any'} :: {' + '.join(patterns)}"
        print(f"{'✅' if ok else '❌'} {label}")
        passed = passed and ok
    return passed


def main() -> int:
    parser = argparse.ArgumentParser(description="Exercise Bluepaws collar ↔ Home Hub downlink commands.")
    parser.add_argument("--hub-url", default="http://bluepaws-hub.local", help="Home Hub HTTP base URL.")
    parser.add_argument("--target-id", type=int, default=1001, help="Collar device ID to command.")
    parser.add_argument("--hub-port", default="COM7", help="Home Hub serial port.")
    parser.add_argument("--collar-port", default="COM23", help="Collar serial port.")
    parser.add_argument("--sniffer-port", default="COM11", help="Sniffer serial port.")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="Serial baud rate.")
    parser.add_argument("--timeout", type=float, default=45.0, help="Seconds to wait for each command result.")
    parser.add_argument("--http-timeout", type=float, default=5.0, help="Seconds to wait for hub HTTP calls.")
    parser.add_argument("--hub-startup-wait", type=float, default=10.0, help="Seconds to wait after opening hub serial before HTTP checks.")
    parser.add_argument("--continue-without-hub-http", action="store_true", help="Keep monitoring serial logs even when the hub HTTP API is unreachable.")
    parser.add_argument("--command-transport", default="auto", choices=["auto", "http", "serial"], help="How to queue hub→collar commands.")
    parser.add_argument("--profile", default="active", choices=["normal", "powersave", "active", "lost"], help="Profile command to test.")
    parser.add_argument("--skip-sniffer", action="store_true", help="Do not open or assert against the sniffer port.")
    parser.add_argument("--skip-collar-control", action="store_true", help="Do not send debug/tx commands to collar serial.")
    parser.add_argument("--dry-run", action="store_true", help="Print planned actions without opening serial ports.")
    args = parser.parse_args()

    args.hub_url = normalize_hub_url(args.hub_url)
    target_hex = hex_device_id(args.target_id)

    print(json.dumps({
        "hub_url": args.hub_url,
        "target_id": args.target_id,
        "target_id_hex": target_hex,
        "ports": {
            "hub": args.hub_port,
            "collar": args.collar_port,
            "sniffer": None if args.skip_sniffer else args.sniffer_port,
        },
        "test_profile": args.profile,
    }, indent=2))

    if args.dry_run:
        print("Dry run only. No serial ports opened and no HTTP commands sent.")
        return 0

    events = EventLog()
    monitors: list[SerialMonitor] = [
        SerialMonitor("HUB", args.hub_port, args.baud, events.queue),
        SerialMonitor("COLLAR", args.collar_port, args.baud, events.queue),
    ]
    if not args.skip_sniffer:
        monitors.append(SerialMonitor("SNIFFER", args.sniffer_port, args.baud, events.queue))

    try:
        for monitor in monitors:
            monitor.start()

        hub = next(monitor for monitor in monitors if monitor.label == "HUB")
        collar = next(monitor for monitor in monitors if monitor.label == "COLLAR")
        command_transport = args.command_transport

        wait_for_hub_settle(events, args.hub_startup_wait)
        status_code, status_body = get_json(args.hub_url, "/api/status", args.http_timeout)
        print()
        print(f"[HTTP] GET /api/status -> {status_code}")
        print(status_body)
        if status_code is None and command_transport == "auto":
            command_transport = "serial"
            print("[INFO] Hub HTTP unreachable; falling back to COM7 serial command transport.")
        if status_code is not None and command_transport == "auto":
            command_transport = "http"
            print("[INFO] Hub HTTP reachable; using HTTP command transport.")
        if status_code is None and args.command_transport == "http" and not args.continue_without_hub_http:
            print()
            print("Hub HTTP API is unreachable, so downlink commands cannot be queued.")
            print("Use the hub IP shown on the hub serial monitor, for example:")
            print("  py -3.11 .\\tools\\hardware_command_pipeline_test.py --hub-url http://192.168.0.67 --target-id 1001")
            print("Or force serial fallback:")
            print("  py -3.11 .\\tools\\hardware_command_pipeline_test.py --command-transport serial --target-id 1001")
            return 2

        if not args.skip_collar_control:
            collar.write_line("debug on")
            time.sleep(0.2)
            collar.write_line("interval 15")

        results: list[tuple[str, bool]] = []

        def force_report() -> None:
            if not args.skip_collar_control:
                collar.write_line("tx")
            else:
                print("[INFO] Waiting for the next natural collar TX/RX window.")

        presence_checks = [
            ("HUB", [r"\[LORA\].*RX", rf"Collar_{target_hex}|device={args.target_id}"], args.timeout),
        ]
        if not args.skip_sniffer:
            presence_checks.append(("SNIFFER", [r"\[RX\]", rf"device={args.target_id}"], args.timeout))
        results.append(("presence/wake check-in observed", run_step("Presence / wake check-in", force_report, presence_checks, events)))

        def queue_profile_command() -> bool:
            if command_transport == "serial":
                hub.write_line(f"cmd mode {args.target_id} {args.profile}")
                if not args.skip_collar_control:
                    time.sleep(0.5)
                    collar.write_line("tx")
                return True

            code, body = post_form(args.hub_url, "/api/command", {"device": target_hex, "mode": args.profile}, args.http_timeout)
            print(f"[HTTP] POST /api/command mode={args.profile} -> {code} {body}")
            if code is None:
                return False
            if not args.skip_collar_control:
                time.sleep(0.5)
                collar.write_line("tx")
            return True

        results.append(("profile command ACKed", run_step(
            f"Profile command → {args.profile}",
            queue_profile_command,
            [
                ("HUB", [r"\[CMD\].*Queued", rf"{args.target_id}|{target_hex}"], args.timeout),
                ("HUB", [r"\[LORA\].*CMD TX"], args.timeout),
                ("COLLAR", [r"CMD_MODE", args.profile], args.timeout),
                ("COLLAR", [r"MODE_ACK"], args.timeout),
                ("HUB", [r"\[ACK\].*ACK"], args.timeout),
            ],
            events,
        )))

        def queue_status_command() -> bool:
            if command_transport == "serial":
                hub.write_line(f"cmd status {args.target_id}")
                if not args.skip_collar_control:
                    time.sleep(0.5)
                    collar.write_line("tx")
                return True

            code, body = post_form(args.hub_url, "/api/device-status", {"device": target_hex}, args.http_timeout)
            print(f"[HTTP] POST /api/device-status -> {code} {body}")
            if code is None:
                return False
            if not args.skip_collar_control:
                time.sleep(0.5)
                collar.write_line("tx")
            return True

        results.append(("status command response observed", run_step(
            "Status command / battery/profile response",
            queue_status_command,
            [
                ("HUB", [r"\[CMD\].*Queued", rf"{args.target_id}|{target_hex}"], args.timeout),
                ("COLLAR", [r"CMD_STATUS"], args.timeout),
                ("COLLAR", [r"STATUS_RESP"], args.timeout),
                ("HUB", [r"\[ACK\].*ACK"], args.timeout),
            ],
            events,
        )))

        def restore_normal() -> bool:
            if args.profile == "normal":
                print("[INFO] Already testing normal; no restore needed.")
                return True

            if command_transport == "serial":
                hub.write_line(f"cmd mode {args.target_id} normal")
                if not args.skip_collar_control:
                    time.sleep(0.5)
                    collar.write_line("tx")
                return True

            code, body = post_form(args.hub_url, "/api/command", {"device": target_hex, "mode": "normal"}, args.http_timeout)
            print(f"[HTTP] POST /api/command mode=normal -> {code} {body}")
            if code is None:
                return False
            if not args.skip_collar_control:
                time.sleep(0.5)
                collar.write_line("tx")
            return True

        if args.profile != "normal":
            results.append(("profile restored to normal", run_step(
                "Restore profile → normal",
                restore_normal,
                [
                    ("COLLAR", [r"CMD_MODE", "normal"], args.timeout),
                    ("COLLAR", [r"MODE_ACK"], args.timeout),
                    ("HUB", [r"\[ACK\].*ACK"], args.timeout),
                ],
                events,
            )))

        print()
        print("=== Summary ===")
        all_ok = True
        for label, ok in results:
            print(f"{'✅' if ok else '❌'} {label}")
            all_ok = all_ok and ok
        return 0 if all_ok else 1
    finally:
        for monitor in monitors:
            monitor.stop()


if __name__ == "__main__":
    sys.exit(main())
