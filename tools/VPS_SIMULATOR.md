# VPS position simulator

This standard-library Python tool makes several provisioned test collars send
independent HTTPS position messages to the Supabase `ingest-position` Edge
Function. It is suitable for an Ubuntu VPS and requires Python 3.10 or newer.
By default it generates movement around Sandhurst, Gloucestershire, with one
fleet update cycle every ten seconds.

## Credentials

Each device has a 16-bit identifier (`1..65534`) and its own random bearer
token. The identifier routes data; the token authenticates the sender. Supabase
stores only the SHA-256 token hash.

The plaintext token is issued once during device provisioning and must be kept
in the private `vps_devices.json` file. It cannot be recovered from the
Supabase dashboard after provisioning; if both private copies are lost, rotate
the device to a newly generated token and replace its stored hash.

Copy the separately supplied `vps_devices.json` to the VPS beside the repository
and protect it:

```bash
chmod 600 tools/vps_devices.json
```

The real file is Git-ignored. `vps_devices.example.json` documents its format
without containing valid secrets.

## Run a bounded test

From the repository root:

```bash
python3 tools/vps_position_simulator.py --device-count 5 --iterations 10 --interval 2
```

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
export BLUEPAWS_DEVICE_FILE=/secure/path/vps_devices.json
export BLUEPAWS_INGEST_URL=https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/ingest-position
python3 tools/vps_position_simulator.py --iterations 100 --interval 1 --seed 42
```

The simulator starts `message_id` from the current Unix time and increments it
per device. The database treats `(device_id, message_id)` as the retry key:
repeating the identical message is accepted once, while reusing that key for a
different position returns HTTP `409`.

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
