import base64
import json
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

from lte_modem_smoke_test import (
    build_demo_request,
    build_get_probe_request,
    build_http_request,
    summarize_http_response,
)


class LteModemSmokeTestTests(unittest.TestCase):
    def test_build_http_request_uses_exact_content_length_and_masks_token(self):
        request = build_http_request(
            "https://example.com/functions/v1/ingest-position",
            "a" * 48,
            {"format": "tlv", "payload_b64": "AAAA"},
            user_agent="test-agent/1",
        )

        header, _, body = request.raw.partition(b"\r\n\r\n")
        self.assertIn(b"POST /functions/v1/ingest-position HTTP/1.1", header)
        self.assertIn(f"Content-Length: {len(body)}".encode(), header)
        self.assertIn(b"Authorization: Bearer " + b"a" * 48, header)
        self.assertEqual(request.masked_preview["headers"]["Authorization"], "Bearer aaaa…aaaa")
        self.assertNotIn("a" * 48, json.dumps(request.masked_preview))

    def test_build_http_request_can_include_masked_supabase_apikey(self):
        request = build_http_request(
            "https://example.com/functions/v1/ingest-position",
            "a" * 48,
            {"format": "tlv", "payload_b64": "AAAA"},
            apikey="b" * 48,
            user_agent="test-agent/1",
        )

        header, _, _body = request.raw.partition(b"\r\n\r\n")
        self.assertIn(b"apikey: " + b"b" * 48, header)
        self.assertEqual(request.masked_preview["headers"]["apikey"], "bbbb…bbbb")
        self.assertNotIn("b" * 48, json.dumps(request.masked_preview))

    def test_build_get_probe_request_uses_no_body_or_content_length(self):
        request = build_get_probe_request(
            "https://example.com/functions/v1/ingest-position",
            apikey="b" * 48,
            user_agent="test-agent/1",
        )

        self.assertEqual(request.body, b"")
        self.assertIn(b"GET /functions/v1/ingest-position HTTP/1.1", request.raw)
        self.assertNotIn(b"Content-Length", request.raw)
        self.assertEqual(request.masked_preview["method"], "GET")

    def test_build_demo_request_reuses_tlv_wrapper_contract(self):
        key = base64.b64encode(bytes(32)).decode()
        with tempfile.TemporaryDirectory() as directory:
            devices = Path(directory) / "devices.json"
            devices.write_text(
                json.dumps(
                    {
                        "devices": [
                            {
                                "device_id": 1001,
                                "bearer_token": "b" * 48,
                                "hmac_key_b64": key,
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            args = Namespace(
                devices_file=devices,
                device_id=1001,
                sequence=42,
                latitude=51.5,
                longitude=-0.1,
                battery_mv=3900,
                accuracy_m=8,
                satellites=9,
                url="https://example.com/functions/v1/ingest-position",
                supabase_apikey=None,
            )

            device_id, request = build_demo_request(args)

        self.assertEqual(device_id, 1001)
        body = json.loads(request.body.decode())
        self.assertEqual(body["format"], "tlv")
        self.assertEqual(body["ingest_path"], "cellular_direct")
        self.assertEqual(body["link_type"], "lte")
        self.assertIn("payload_b64", body)

    def test_summarizes_modem_http_response_json_body(self):
        raw = '+QSSLRECV: 38\r\nHTTP/1.1 201 Created\r\nContent-Type: application/json\r\n\r\n{"accepted":true}\r\nOK\r\n'

        summary = summarize_http_response(raw)

        self.assertEqual(summary["http_status"], 201)
        self.assertEqual(summary["body"], {"accepted": True})


if __name__ == "__main__":
    unittest.main()
