# BluepawsProtocol shared library

This folder contains the shared protocol code used by the collar and hub firmware.

## Canonical TLV specification

The current canonical packet specification is:

```text
docs/TLV_PROTOCOL_V1_1.md
```

That document supersedes older notes that referred to:

- 29-byte headers
- 33-byte TLV sections
- 32-bit message sequence IDs
- JSON as a production telemetry payload

## Current v1.1 packet shape

```text
[32-byte fixed header][0-24 bytes TLV][8-byte auth tag]
```

Packet size:

```text
minimum = 40 bytes
maximum = 64 bytes
```

## Current v1.1 header summary

| Offset | Size | Field | Type |
|---:|---:|---|---|
| 0 | 1 | `ver` | u8 |
| 1 | 2 | `device_guid16` | u16 |
| 3 | 2 | `msg_seq_id` | u16 |
| 5 | 4 | `time_unix` | u32 |
| 9 | 1 | `state` | u8 |
| 10 | 1 | `flags` | u8 |
| 11 | 1 | `tx_reason` | u8 |
| 12 | 4 | `lat_e7` | i32 |
| 16 | 4 | `lon_e7` | i32 |
| 20 | 2 | `batt_mV` | u16 |
| 22 | 2 | `acc_m` | u16 |
| 24 | 2 | `fix_age_s` | u16 |
| 26 | 1 | `sat_count` | u8 |
| 27 | 4 | `hdr_rsvd` | u8[4] |
| 31 | 1 | `tlv_len` | u8 |

## Implementation warning

Keep protocol constants, encoder, decoder, README files, hub parser, collar transmitter, cloud parser and simulator payloads aligned with `docs/TLV_PROTOCOL_V1_1.md`.

If this library still contains legacy constants or packet helpers, update the implementation before treating firmware output as v1.1 compliant.

## Boot and wake-check-in semantics

`tx_reason = BOOT` and `tx_reason = WAKE_CHECKIN` use the unchanged v1.1 header. Do not add new header flag meanings for boot diagnostics. Use TLVs such as `firmware_version`, `reset_reason`, and `uptime_s`.

No-GNSS boot and wake-check-in packets are valid presence/diagnostic reports. Cloud ingestion should update last-seen/presence while preserving the last known valid coordinates.
