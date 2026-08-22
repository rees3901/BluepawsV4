# VPS telemetry simulators

The standard-library Python tools make several provisioned test collars send
independent HTTPS position messages to the Supabase `ingest-position` Edge
Function. It is suitable for an Ubuntu VPS and requires Python 3.10 or newer.
By default it generates movement around Sandhurst, Gloucestershire, with one
fleet update cycle every ten seconds.

- `vps_position_simulator.py` preserves the legacy JSON debugging contract.
- `tlv_telemetry_simulator.py` sends the primary authenticated v1.1 binary TLV
  packet inside the locked LTE or LoRa JSON transport wrapper.
- `tlv_simulator_gui.py` is the desktop packet-builder and manual HTTPS test
  console. It uses the pinned packages in `requirements-gui.txt`. Run it on
  Windows or a Linux desktop, not a headless VPS. See `TLV_SIMULATOR_GUI.md`.

## Credentials

Each device has a 16-bit identifier (`1..65535`) and its own random bearer
token. The identifier routes data; the token authenticates the HTTPS sender.
Supabase stores only the SHA-256 token hash. TLV devices also have a random
32-byte HMAC key. The simulator stores it as Base64, while Supabase stores its
encrypted copy in Vault.

Plaintext tokens are issued once during provisioning and must be kept in the
private `devices.json` credentials bundle. Its `devices` and `gateways`
arrays keep the two identity types distinct. It cannot be recovered from the
Supabase dashboard after provisioning; if both private copies are lost, rotate
the affected identity to a newly generated token and replace its stored hash.

Copy the separately supplied `devices.json` to the VPS beside the repository
and protect it:

```bash
chmod 600 tools/devices.json
```

The real file is Git-ignored. Existing flat device arrays remain supported.

To generate a fresh bundle with cryptographically secure bearer tokens and
32-byte HMAC keys, plus matching one-time Supabase provisioning SQL:

```bash
python3 tools/generate_tlv_credentials.py \
  --count 5 \
  --start-device-id 1001 \
  --household-id '<TEST-HOUSEHOLD-UUID>' \
  --gateway-guid16 0016
```

The command refuses to overwrite either output unless `--force` is explicitly
provided. Both outputs are Git-ignored and created with owner-only permissions
where the operating system supports them. The SQL contains bearer hashes rather
than bearer plaintext, but necessarily carries each HMAC once into Supabase
Vault; run it once and securely remove it afterward.

## Run a bounded test

From the repository root:

```bash
python3 tools/vps_position_simulator.py --device-count 5 --iterations 10 --interval 2
```

For the primary TLV contract:

```bash
python3 tools/tlv_telemetry_simulator.py --device-count 5 --iterations 10 --interval 2
```

For LoRa, a bundle containing exactly one gateway is selected automatically:

```bash
python3 tools/tlv_telemetry_simulator.py --transport lora_gateway --device-count 1 --iterations 5 --interval 2
```

If the bundle contains several gateways, add `--gateway-guid16 0016`. The
`--gateway-token` and `BLUEPAWS_GATEWAY_TOKEN` overrides remain available for
temporary testing, but the token is normally loaded from the private bundle.

For the original continuous Sandhurst simulation defaults, run:

```bash
python3 tools/vps_position_simulator.py --device-count 5
```

Successful new messages return HTTP `201`; deliberate safe retries return `200`
with `"duplicate":true`. The simulator defaults to a 5% retry rate to exercise
idempotency. Stop an unbounded run with Ctrl+C.

Before a longer simulation, run the bounded contract smoke test:

```bash
python3 tools/smoke_test_ingest.py
```

It checks method restrictions, authentication, validation, a new write, an
identical retry, a conflicting retry, and an unprovisioned device.

Useful overrides:

```bash
export BLUEPAWS_DEVICE_FILE=/secure/path/devices.json
export BLUEPAWS_INGEST_URL=https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/ingest-position
python3 tools/vps_position_simulator.py --iterations 100 --interval 1 --seed 42
```

The legacy simulator starts `message_id` from the current Unix time. The TLV
simulator uses the protocol's rolling 16-bit sequence and an 8-byte truncated
HMAC-SHA256 authentication tag. The backend canonically deduplicates identical
TLV packets with its own full SHA-256 payload hash and returns HTTP `409` if a
different packet reuses the same device, sequence, and packet timestamp.

## Request contract (version 1)

```json
{
  "schema_version": 1,
  "device_id": 1001,
  "message_id": 1786276800,
  "latitude": 51.5074,
  "longitude": -0.1278,
  "battery": 87,
  "recorded_at": "2026-08-09T12:00:00.000Z"
}
```

Send it as `application/json` with `Authorization: Bearer <device-token>`. Never
put the Supabase service-role key on a VPS, collar, hub, browser, or in Git.

See `docs/TLV_INGESTION_RUNBOOK.md` for HMAC key provisioning, LoRa gateway
credentials, migration/deployment steps, and validation queries.
