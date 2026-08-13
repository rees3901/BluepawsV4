# Bluepaws TLV desktop test console

`tlv_simulator_gui.py` is a two-stage desktop tool for manually constructing
and sending Bluepaws v1.1 telemetry. It uses only Python's standard library and
shares its packet codec with the VPS command-line simulator.

Use the GUI for exploratory and negative-path testing. Keep using
`tlv_telemetry_simulator.py` for headless VPS runs and load tests.

## Start it

On Windows, from the repository root:

```powershell
py -3 tools\tlv_simulator_gui.py
```

On a Linux desktop:

```bash
sudo apt install python3-tk
python3 tools/tlv_simulator_gui.py
```

An Ubuntu VPS normally has no desktop display. Run the GUI on the Windows PC
instead; it still exercises the public Supabase endpoint over the internet.
The import-only check is safe on a headless machine:

```bash
python3 tools/tlv_simulator_gui.py --check
```

## Credentials

Choose **File -> Load credentials** and select the private
`vps_devices.json`. Selecting a device fills its ID, HMAC key and HTTPS bearer
token. Both secrets stay masked and are not copied into the JSON preview or
response log.

The credentials file is Git-ignored. Do not commit it, send screenshots with
the secret fields revealed, or use a Supabase service-role key in this tool.

For `cellular_direct`, use the selected device's bearer token. For `lora_hub`,
replace it with the separately provisioned gateway bearer token and provide the
gateway's four-digit hexadecimal GUID.

## Tab 1: TLV Packet Builder

1. Select a provisioned device.
2. Set the fixed-header fields and flags.
3. Enable any of the five selected v1.1 TLVs, or add unknown TLVs as raw hex.
4. Choose a valid, deliberately corrupted, or custom 8-byte HMAC tag.
5. Select **Build and validate packet**.

The console enforces the deployed Edge Function's current contract:

- one 32-byte fixed header;
- device ID `1..65535`;
- accepted status, power-profile and TX-reason enum values;
- zero-filled reserved bytes;
- known TLV sizes and no duplicate known TLV types;
- a maximum 24-byte TLV section;
- one 8-byte truncated HMAC-SHA256 tag;
- a total decoded packet size of 40 to 64 bytes.

The output includes packet Base64, readable hex, transmitted tag, packet size,
TLV budget and the packet's full SHA-256 deduplication hash.

## Tab 2: HTTPS Wrapper & Send

Choose LTE direct or LoRa home-hub relay. The irrelevant metadata fields are
disabled and omitted rather than sent as null values. **Refresh preview** shows
the exact JSON body; the Authorization header is deliberately excluded.

Use **Send** for one request, or set a count and interval. With **Advance
sequence and timestamp** enabled, each request is a newly authenticated packet.
With it disabled, every request repeats the exact same payload, which is useful
for verifying idempotent duplicate handling.

Expected responses include:

- `201`: new observation accepted;
- `200` with `duplicate: true`: identical packet retry accepted safely;
- `400`: malformed wrapper or packet;
- `401`: invalid device/gateway bearer token or HMAC;
- `409`: sequence/timestamp identity conflict with different packet content;
- `503`: ingestion service or database-stage failure.

The deliberately corrupted/custom HMAC controls are intended only for test
devices. A negative test should be rejected and should not move a live marker.

## Files

- `tlv_packet_codec.py`: reusable packet, HMAC, wrapper and HTTP validation.
- `tlv_simulator_gui.py`: Tk/ttk desktop interface.
- `tlv_telemetry_simulator.py`: headless multi-device CLI simulator.
- `test_tlv_packet_codec.py`: locked-contract unit tests.
- `../docs/TLV_PROTOCOL_V1_1.md`: canonical protocol specification.
