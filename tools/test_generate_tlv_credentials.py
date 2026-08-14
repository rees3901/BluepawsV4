"""Tests for local Bluepaws device credential provisioning."""

from __future__ import annotations

import base64
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import uuid

from generate_tlv_credentials import (
    credential_bundle,
    generate_device,
    generate_gateway,
    main,
    provisioning_sql,
)
from tlv_packet_codec import load_credential_bundle


class CredentialGeneratorTests(unittest.TestCase):
    def test_generated_device_secrets_match_wire_contract(self) -> None:
        first = generate_device(1001)
        second = generate_device(1002)

        self.assertEqual(len(base64.b64decode(first.hmac_key_b64, validate=True)), 32)
        self.assertGreaterEqual(len(first.bearer_token), 32)
        self.assertNotEqual(first.hmac_key_b64, second.hmac_key_b64)
        self.assertNotEqual(first.bearer_token, second.bearer_token)

    def test_sql_hashes_bearers_and_stages_hmacs_for_vault(self) -> None:
        device = generate_device(1001)
        gateway = generate_gateway("0016", "Test Hub")
        household_id = uuid.UUID("6e799f91-3027-4c8f-b239-09531939e79e")

        sql = provisioning_sql([device], household_id, 1, gateway)

        self.assertNotIn(device.bearer_token, sql)
        self.assertNotIn(gateway.bearer_token, sql)
        self.assertIn(hashlib.sha256(device.bearer_token.encode()).hexdigest(), sql)
        self.assertIn(hashlib.sha256(gateway.bearer_token.encode()).hexdigest(), sql)
        self.assertIn(device.hmac_key_b64, sql)
        self.assertIn("vault.create_secret", sql)
        self.assertIn("values (22,", sql)
        self.assertNotIn("set household_id = excluded.household_id", sql)

    def test_cli_outputs_loadable_private_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            json_path = Path(directory) / "devices.json"
            sql_path = Path(directory) / "provision.sql"
            result = main(
                [
                    "--count",
                    "3",
                    "--start-device-id",
                    "2001",
                    "--household-id",
                    "6e799f91-3027-4c8f-b239-09531939e79e",
                    "--gateway-guid16",
                    "0016",
                    "--output",
                    str(json_path),
                    "--sql-output",
                    str(sql_path),
                ]
            )

            self.assertEqual(result, 0)
            parsed = json.loads(json_path.read_text(encoding="utf-8"))
            self.assertEqual(parsed["schema_version"], 1)
            bundle = load_credential_bundle(json_path)
            self.assertEqual([item.device_id for item in bundle.devices], [2001, 2002, 2003])
            self.assertEqual(bundle.gateways[0].gateway_guid16, "0016")
            self.assertTrue(sql_path.read_text(encoding="utf-8").endswith("\n"))

    def test_bundle_has_explicit_empty_gateways_array(self) -> None:
        bundle = credential_bundle([generate_device(1001)], None)
        self.assertEqual(bundle["gateways"], [])


if __name__ == "__main__":
    unittest.main()
