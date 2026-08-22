#!/usr/bin/env python3
"""Send one Bluepaws TLV HTTPS wrapper through a Quectel EG800K serial modem.

This is a transport smoke-test, not a replacement for the local TLV web console.
It proves the PC -> UART -> EG800K -> LTE -> Supabase Edge Function path using
the same v1.1 TLV packet builder and wrapper contract as the existing tools.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

from tlv_packet_codec import (
    DEFAULT_URL,
    FLAG_MASKS,
    PacketFields,
    build_tlv_packet,
    build_transport_wrapper,
    firmware_tlv,
    known_tlv,
    load_credential_bundle,
    validate_bearer_token,
)

DEFAULT_PORTS = ("COM20", "COM21", "COM18")
DEFAULT_BAUD = 115200
DEFAULT_APN = "iot.1nce.net"
DEFAULT_COMMAND_BYTE_DELAY = 0.002
DEFAULT_PAYLOAD_BYTE_DELAY = 0.002
DEFAULT_TIMEOUT_SECONDS = 15.0
DEFAULT_LATITUDE = 51.907055
DEFAULT_LONGITUDE = -2.256660
CONNECTION_HEADERS = ("close", "keep-alive")


class ModemError(RuntimeError):
    """Raised when the modem does not complete the expected AT-command state."""


@dataclass(frozen=True)
class HttpRequest:
    host: str
    port: int
    path: str
    body: bytes
    raw: bytes
    masked_preview: dict[str, Any]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send a Bluepaws TLV packet through a Quectel EG800K LTE modem."
    )
    parser.add_argument("--url", default=DEFAULT_URL)
    parser.add_argument(
        "--devices-file",
        type=Path,
        default=Path("tools/devices.json"),
        help="Credential bundle containing bearer token and HMAC key.",
    )
    parser.add_argument("--device-id", type=int, help="Device ID to send as. Defaults to first device.")
    parser.add_argument(
        "--port",
        help="Serial port to use, for example COM20. If omitted, --ports are tried in order.",
    )
    parser.add_argument(
        "--ports",
        nargs="+",
        default=list(DEFAULT_PORTS),
        help="Candidate ports for auto-open when --port is omitted.",
    )
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--apn", default=DEFAULT_APN)
    parser.add_argument(
        "--supabase-apikey",
        default=os.getenv("BLUEPAWS_SUPABASE_APIKEY") or os.getenv("NEXT_PUBLIC_SUPABASE_PUBLISHABLE_KEY"),
        help="Optional Supabase publishable/anon key to include as the apikey header.",
    )
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--command-byte-delay", type=float, default=DEFAULT_COMMAND_BYTE_DELAY)
    parser.add_argument("--payload-byte-delay", type=float, default=DEFAULT_PAYLOAD_BYTE_DELAY)
    parser.add_argument("--ssl-context", type=int, default=0)
    parser.add_argument("--socket-id", type=int, default=0)
    parser.add_argument("--pdp-context", type=int, default=1)
    parser.add_argument(
        "--access-mode",
        type=int,
        default=0,
        choices=(0, 1),
        help="Quectel SSL socket access mode. 0=buffer mode, 1=direct push mode.",
    )
    parser.add_argument(
        "--http-version",
        default="1.1",
        choices=("1.0", "1.1"),
        help="Raw HTTP version to use in the request line.",
    )
    parser.add_argument(
        "--connection",
        default="close",
        choices=CONNECTION_HEADERS,
        help="Connection header. keep-alive can help diagnose servers that close too quickly for QSSLRECV.",
    )
    parser.add_argument(
        "--ssl-seclevel",
        type=int,
        default=0,
        choices=(0, 1, 2),
        help="Quectel certificate verification level. 0 matches the current proof-of-concept notes.",
    )
    parser.add_argument("--latitude", type=float, default=DEFAULT_LATITUDE)
    parser.add_argument("--longitude", type=float, default=DEFAULT_LONGITUDE)
    parser.add_argument("--battery-mv", type=int, default=3900)
    parser.add_argument("--accuracy-m", type=int, default=8)
    parser.add_argument("--satellites", type=int, default=9)
    parser.add_argument("--sequence", type=int, default=random.randint(0, 65_535))
    parser.add_argument(
        "--probe-only",
        action="store_true",
        help="Bring up modem/SIM/PDP/TLS config, but do not POST telemetry.",
    )
    parser.add_argument(
        "--http-get-probe",
        action="store_true",
        help="Send a simple GET request to the target host/path instead of telemetry.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Build and print the masked HTTP request without opening the serial port.",
    )
    parser.add_argument(
        "--print-http-response",
        action="store_true",
        help="Print the raw HTTP response text returned by QSSLRECV.",
    )
    parser.add_argument(
        "--trace-at",
        action="store_true",
        help="Print AT-command responses. Authorization values are still not printed.",
    )
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.command_byte_delay < 0 or args.payload_byte_delay < 0:
        parser.error("byte delays cannot be negative")
    if args.baud <= 0:
        parser.error("--baud must be positive")
    return args


def build_demo_request(args: argparse.Namespace) -> tuple[int, HttpRequest]:
    bundle = load_credential_bundle(args.devices_file)
    credential = (
        next((item for item in bundle.devices if item.device_id == args.device_id), None)
        if args.device_id is not None
        else bundle.devices[0]
    )
    if credential is None:
        raise ValueError(f"device_id {args.device_id} was not found in {args.devices_file}")
    flags = FLAG_MASKS["GNSS_VALID"] | FLAG_MASKS["FIX_3D"]
    built = build_tlv_packet(
        PacketFields(
            device_id=credential.device_id,
            message_sequence=args.sequence,
            timestamp=int(time.time()),
            status=1,
            power_profile=1,
            flags=flags,
            tx_reason=0,
            latitude=args.latitude,
            longitude=args.longitude,
            battery_mv=args.battery_mv,
            accuracy_m=args.accuracy_m,
            fix_age_s=0,
            satellite_count=args.satellites,
        ),
        (
            firmware_tlv("1.1"),
            known_tlv(0x10, 60),
            known_tlv(0x13, 42),
        ),
        credential.hmac_key,
    )
    wrapper = build_transport_wrapper(
        built.payload_b64,
        "cellular_direct",
        link_rssi_dbm=-92,
        link_snr_db=10,
        cell_rsrp_dbm=-92,
        cell_rsrq_db=-9,
        cell_sinr_db=10,
    )
    return credential.device_id, build_http_request(
        args.url,
        credential.token,
        wrapper,
        apikey=args.supabase_apikey,
        http_version=args.http_version,
        connection=args.connection,
        user_agent="bluepaws-eg800k-smoke-test/1",
    )


def build_http_request(
    url: str,
    bearer_token: str,
    wrapper: dict[str, Any],
    *,
    apikey: str | None = None,
    http_version: str = "1.1",
    connection: str = "close",
    user_agent: str,
) -> HttpRequest:
    validate_bearer_token(bearer_token)
    parsed = urlparse(url)
    if parsed.scheme.lower() != "https":
        raise ValueError("EG800K smoke test endpoint must use HTTPS")
    if not parsed.hostname:
        raise ValueError("endpoint URL must include a hostname")
    if http_version not in ("1.0", "1.1"):
        raise ValueError("HTTP version must be 1.0 or 1.1")
    if connection not in CONNECTION_HEADERS:
        raise ValueError("Connection header must be close or keep-alive")
    path = parsed.path or "/"
    if parsed.query:
        path = f"{path}?{parsed.query}"
    body = json.dumps(wrapper, separators=(",", ":")).encode("utf-8")
    port = parsed.port or 443
    host_header = parsed.hostname if port == 443 else f"{parsed.hostname}:{port}"
    header_lines = [
        f"POST {path} HTTP/{http_version}",
        f"Host: {host_header}",
        f"Authorization: Bearer {bearer_token}",
        "Content-Type: application/json",
        "Accept: application/json",
        f"User-Agent: {user_agent}",
        f"Content-Length: {len(body)}",
        f"Connection: {connection}",
        "",
        "",
    ]
    if apikey:
        validate_bearer_token(apikey, "Supabase apikey")
        header_lines.insert(3, f"apikey: {apikey}")
    raw = "\r\n".join(header_lines).encode("utf-8") + body
    masked_headers = {
        "Host": host_header,
        "Authorization": f"Bearer {mask_secret(bearer_token)}",
        "Content-Type": "application/json",
        "Accept": "application/json",
        "User-Agent": user_agent,
        "Content-Length": len(body),
        "Connection": connection,
    }
    if apikey:
        masked_headers["apikey"] = mask_secret(apikey)
    return HttpRequest(
        host=parsed.hostname,
        port=port,
        path=path,
        body=body,
        raw=raw,
        masked_preview={
            "method": "POST",
            "url": url,
            "headers": masked_headers,
            "body": wrapper,
            "body_size_bytes": len(body),
            "raw_http_size_bytes": len(raw),
        },
    )


def build_get_probe_request(url: str, *, user_agent: str, apikey: str | None = None) -> HttpRequest:
    parsed = urlparse(url)
    if parsed.scheme.lower() != "https":
        raise ValueError("EG800K smoke test endpoint must use HTTPS")
    if not parsed.hostname:
        raise ValueError("endpoint URL must include a hostname")
    path = parsed.path or "/"
    if parsed.query:
        path = f"{path}?{parsed.query}"
    port = parsed.port or 443
    host_header = parsed.hostname if port == 443 else f"{parsed.hostname}:{port}"
    header_lines = [
        f"GET {path} HTTP/1.1",
        f"Host: {host_header}",
        "Accept: application/json",
        f"User-Agent: {user_agent}",
        "Connection: close",
        "",
        "",
    ]
    masked_headers = {
        "Host": host_header,
        "Accept": "application/json",
        "User-Agent": user_agent,
        "Connection": "close",
    }
    if apikey:
        validate_bearer_token(apikey, "Supabase apikey")
        header_lines.insert(2, f"apikey: {apikey}")
        masked_headers["apikey"] = mask_secret(apikey)
    raw = "\r\n".join(header_lines).encode("utf-8")
    return HttpRequest(
        host=parsed.hostname,
        port=port,
        path=path,
        body=b"",
        raw=raw,
        masked_preview={
            "method": "GET",
            "url": url,
            "headers": masked_headers,
            "body": None,
            "body_size_bytes": 0,
            "raw_http_size_bytes": len(raw),
        },
    )


def mask_secret(value: str) -> str:
    if len(value) <= 10:
        return "••••"
    return f"{value[:4]}…{value[-4:]}"


def open_serial_port(port: str | None, ports: list[str], baud: int, timeout: float):
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError as error:
        raise RuntimeError(
            "pyserial is required. Install it with: py -3.11 -m pip install -r tools\\requirements-lte-modem.txt"
        ) from error

    candidates = [port] if port else ports
    errors: list[str] = []
    for candidate in candidates:
        try:
            return serial.Serial(
                port=candidate,
                baudrate=baud,
                timeout=0.1,
                write_timeout=max(1.0, timeout),
            )
        except Exception as error:  # serial.SerialException is not available without import typing help.
            errors.append(f"{candidate}: {error}")
    raise RuntimeError("Unable to open serial port. Tried " + "; ".join(errors))


class QuectelSslSocket:
    def __init__(
        self,
        serial_port: Any,
        *,
        command_byte_delay: float,
        payload_byte_delay: float,
        timeout: float,
        trace: bool = False,
    ) -> None:
        self.serial = serial_port
        self.command_byte_delay = command_byte_delay
        self.payload_byte_delay = payload_byte_delay
        self.timeout = timeout
        self.trace = trace

    def slow_write(self, data: str | bytes, *, payload: bool = False) -> None:
        raw = data.encode("utf-8") if isinstance(data, str) else data
        delay = self.payload_byte_delay if payload else self.command_byte_delay
        for byte in raw:
            self.serial.write(bytes([byte]))
            self.serial.flush()
            if delay:
                time.sleep(delay)

    def drain(self) -> None:
        end = time.monotonic() + 0.25
        while time.monotonic() < end:
            waiting = getattr(self.serial, "in_waiting", 0)
            data = self.serial.read(waiting or 1)
            if not data:
                break

    def read_until(self, patterns: tuple[str, ...], timeout: float | None = None) -> str:
        end = time.monotonic() + (self.timeout if timeout is None else timeout)
        chunks: list[str] = []
        while time.monotonic() < end:
            waiting = getattr(self.serial, "in_waiting", 0)
            raw = self.serial.read(waiting or 1)
            if raw:
                text = raw.decode("utf-8", errors="replace")
                chunks.append(text)
                current = "".join(chunks)
                if any(pattern in current for pattern in patterns):
                    return current
            else:
                time.sleep(0.02)
        return "".join(chunks)

    def at(
        self,
        command: str,
        *,
        timeout: float | None = None,
        tolerate_error: bool = False,
        expect: tuple[str, ...] = ("OK\r\n", "ERROR\r\n"),
    ) -> str:
        self.slow_write(command + "\r\n")
        response = self.read_until(expect, timeout)
        if self.trace and command != "AT+QSSLSEND":
            print_trace(command, response)
        if not response:
            raise ModemError(f"{command} timed out")
        if "ERROR" in response and not tolerate_error:
            raise ModemError(f"{command} returned ERROR:\n{response}")
        return response

    def configure(
        self,
        *,
        apn: str,
        pdp_context: int,
        ssl_context: int,
        ssl_seclevel: int,
    ) -> None:
        self.drain()
        self.at("AT")
        self.at("ATE0")
        self.at("AT+CMEE=2")
        self.at("AT+CPIN?")
        self.at("AT+CSQ")
        self.at("AT+CEREG?")
        self.at(f'AT+QICSGP={pdp_context},1,"{apn}","","",1')
        self.at(f"AT+QIACT={pdp_context}", tolerate_error=True, timeout=max(20, self.timeout))
        qiact = self.at("AT+QIACT?")
        if f"+QIACT: {pdp_context},1,1," not in qiact:
            raise ModemError(f"PDP context {pdp_context} is not active:\n{qiact}")
        self.at(f'AT+QSSLCFG="sslversion",{ssl_context},4')
        self.at(f'AT+QSSLCFG="ciphersuite",{ssl_context},0xFFFF')
        self.at(f'AT+QSSLCFG="seclevel",{ssl_context},{ssl_seclevel}')
        self.at(f'AT+QSSLCFG="sni",{ssl_context},1')

    def open_socket(
        self,
        *,
        pdp_context: int,
        ssl_context: int,
        socket_id: int,
        host: str,
        port: int,
        access_mode: int,
    ) -> None:
        self.close_socket(socket_id, tolerate_error=True)
        time.sleep(1)
        command = f'AT+QSSLOPEN={pdp_context},{ssl_context},{socket_id},"{host}",{port},{access_mode}'
        self.at(command, timeout=max(5, self.timeout))
        opened = self.read_until(
            (f"+QSSLOPEN: {socket_id},0", f"+QSSLOPEN: {socket_id},563", f"+QSSLOPEN: {socket_id},552"),
            timeout=max(45, self.timeout),
        )
        if self.trace:
            print_trace("+QSSLOPEN result", opened)
        if f"+QSSLOPEN: {socket_id},0" in opened:
            return
        if f"+QSSLOPEN: {socket_id},563" in opened:
            self.close_socket(socket_id, tolerate_error=True)
            time.sleep(1)
            self.at(command, timeout=max(5, self.timeout))
            opened = self.read_until((f"+QSSLOPEN: {socket_id},0",), timeout=max(45, self.timeout))
            if f"+QSSLOPEN: {socket_id},0" in opened:
                return
        error_detail = self.at("AT+QIGETERROR", tolerate_error=True)
        raise ModemError(f"TLS socket failed to open:\n{opened}\n{error_detail}")

    def send_http(self, socket_id: int, request: bytes) -> str:
        self.slow_write(f"AT+QSSLSEND={socket_id},{len(request)}\r\n")
        prompt = self.read_until((">", "ERROR\r\n"), timeout=max(5, self.timeout))
        if self.trace:
            print_trace("AT+QSSLSEND prompt", prompt)
        if ">" not in prompt:
            raise ModemError(f"Modem did not enter SSL send mode:\n{prompt}")
        self.slow_write(request, payload=True)
        sent = self.read_until(("SEND OK", "SEND FAIL", "ERROR\r\n"), timeout=max(20, self.timeout))
        if self.trace:
            print_trace("QSSLSEND result", sent)
        if "SEND OK" not in sent:
            raise ModemError(f"SSL payload send failed:\n{sent}")
        recv_notice = self.read_until(
            ("HTTP/", f'+QSSLURC: "recv",{socket_id}', f'+QSSLURC: "closed",{socket_id}'),
            timeout=max(30, self.timeout),
        )
        if self.trace:
            print_trace("QSSLURC", recv_notice)
        if "HTTP/" in recv_notice:
            return recv_notice
        if f'+QSSLURC: "recv",{socket_id}' not in recv_notice:
            # Some endpoints close quickly. Give the modem a final short window in case
            # the recv URC arrives immediately after the closed URC in a separate read.
            trailing_notice = self.read_until(("HTTP/", f'+QSSLURC: "recv",{socket_id}'), timeout=2)
            if self.trace and trailing_notice:
                print_trace("Trailing QSSLURC", trailing_notice)
            recv_notice += trailing_notice
        if "HTTP/" in recv_notice:
            return recv_notice
        if f'+QSSLURC: "recv",{socket_id}' in recv_notice:
            return self.at(
                f"AT+QSSLRECV={socket_id},4096",
                tolerate_error=True,
                timeout=max(10, self.timeout),
            )
        error_detail = self.at("AT+QIGETERROR", tolerate_error=True)
        return (
            "NO_HTTP_RESPONSE_AVAILABLE\r\n"
            + recv_notice
            + "\r\n"
            + error_detail
        )

    def close_socket(self, socket_id: int, *, tolerate_error: bool = False) -> None:
        try:
            self.at(f"AT+QSSLCLOSE={socket_id}", timeout=10, tolerate_error=tolerate_error)
        except ModemError:
            if not tolerate_error:
                raise


def print_trace(label: str, response: str) -> None:
    cleaned = response.replace("\r", "")
    print(f"\n--- {label} ---\n{cleaned.strip() or '<no response>'}")


def summarize_http_response(modem_response: str) -> dict[str, Any]:
    http_start = modem_response.find("HTTP/")
    if http_start < 0:
        return {"http_status": None, "body": None, "raw": modem_response.strip()}
    http_text = modem_response[http_start:]
    header_text, separator, body_text = http_text.partition("\r\n\r\n")
    status_line = header_text.splitlines()[0] if header_text else ""
    status_code = None
    parts = status_line.split()
    if len(parts) >= 2 and parts[1].isdigit():
        status_code = int(parts[1])
    body = strip_modem_footer(body_text)
    parsed_body: Any = body
    if body:
        try:
            parsed_body = json.loads(body)
        except json.JSONDecodeError:
            parsed_body = body
    return {
        "http_status": status_code,
        "body": parsed_body,
        "raw": http_text.strip() if separator else modem_response.strip(),
    }


def strip_modem_footer(body_text: str) -> str:
    body = body_text.strip()
    for marker in ("\r\nOK", "\nOK", "\r\nERROR", "\nERROR"):
        index = body.find(marker)
        if index >= 0:
            body = body[:index].strip()
    return body


def main() -> int:
    args = parse_args()
    try:
        if args.http_get_probe:
            device_id = None
            request = build_get_probe_request(
                args.url,
                user_agent="bluepaws-eg800k-smoke-test/1",
                apikey=args.supabase_apikey,
            )
        else:
            device_id, request = build_demo_request(args)
    except Exception as error:
        print(f"Unable to build TLV request: {error}", file=sys.stderr)
        return 2

    print(
        json.dumps(
            {
                "device_id": device_id,
                "target": {"host": request.host, "port": request.port, "path": request.path},
                "request_preview": request.masked_preview,
            },
            indent=2,
        )
    )
    if args.dry_run:
        return 0

    if args.ssl_seclevel == 0:
        print("Warning: Quectel SSL seclevel=0 disables certificate validation; acceptable for this smoke test only.")

    serial_port = None
    try:
        serial_port = open_serial_port(args.port, args.ports, args.baud, args.timeout)
        print(f"Opened {serial_port.port} at {args.baud} baud.")
        modem = QuectelSslSocket(
            serial_port,
            command_byte_delay=args.command_byte_delay,
            payload_byte_delay=args.payload_byte_delay,
            timeout=args.timeout,
            trace=args.trace_at,
        )
        modem.configure(
            apn=args.apn,
            pdp_context=args.pdp_context,
            ssl_context=args.ssl_context,
            ssl_seclevel=args.ssl_seclevel,
        )
        if args.probe_only:
            print("Probe complete: modem, SIM, PDP context, and TLS settings responded.")
            return 0
        modem.open_socket(
            pdp_context=args.pdp_context,
            ssl_context=args.ssl_context,
            socket_id=args.socket_id,
            host=request.host,
            port=request.port,
            access_mode=args.access_mode,
        )
        response = modem.send_http(args.socket_id, request.raw)
        summary = summarize_http_response(response)
        print(json.dumps({"modem_post_result": summary}, indent=2))
        if args.print_http_response:
            print("\nRaw modem response:")
            print(response)
        return 0 if summary["http_status"] in (200, 201) else 1
    except (OSError, RuntimeError, ModemError) as error:
        print(f"LTE modem smoke test failed: {error}", file=sys.stderr)
        return 1
    finally:
        if serial_port is not None:
            try:
                serial_port.close()
            except Exception:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
