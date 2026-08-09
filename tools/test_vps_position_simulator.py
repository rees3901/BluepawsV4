import json
import random
import tempfile
import unittest
from pathlib import Path

from vps_position_simulator import (
    DeviceCredential,
    DeviceState,
    build_payload,
    load_credentials,
)


class SimulatorTests(unittest.TestCase):
    def test_load_credentials_rejects_duplicate_device_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "devices.json"
            path.write_text(
                json.dumps(
                    [
                        {"device_id": 1001, "token": "a" * 32},
                        {"device_id": 1001, "token": "b" * 32},
                    ]
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "duplicated"):
                load_credentials(path)

    def test_payload_uses_wire_contract_and_increments_message_id(self):
        state = DeviceState(
            credential=DeviceCredential(1001, "a" * 32),
            latitude=51.5,
            longitude=-0.1,
            battery=80,
            message_id=10,
        )
        payload = build_payload(state, random.Random(1), 0)
        self.assertEqual(payload["schema_version"], 1)
        self.assertEqual(payload["device_id"], 1001)
        self.assertEqual(payload["message_id"], 11)
        self.assertEqual(payload["latitude"], 51.5)
        self.assertEqual(payload["longitude"], -0.1)
        self.assertTrue(payload["recorded_at"].endswith("Z"))


if __name__ == "__main__":
    unittest.main()
