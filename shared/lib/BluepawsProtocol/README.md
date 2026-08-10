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
- 16-bit device IDs
- 32-bit message sequence IDs
- JSON as a production telemetry payload

## Current v1.1 packet shape

```text
[32-byte fixed header][0-30 bytes TLV][2-byte CRC16]
```

Packet size:

```text
minimum = 34 bytes
maximum = 64 bytes
```

## Current v1.1 header summary

| Offset | Size | Field | Type |
|---:|---:|---|---|
| 0 | 1 | `ver` | u8 |
| 1 | 4 | `device_guid32` | u32 |
| 5 | 2 | `msg_seq_id` | u16 |
| 7 | 4 | `time_unix` | u32 |
| 11 | 1 | `state` | u8 |
| 12 | 1 | `flags` | u8 |
| 13 | 4 | `lat_e7` | i32 |
| 17 | 4 | `lon_e7` | i32 |
| 21 | 2 | `batt_mV` | u16 |
| 23 | 2 | `acc_m` | u16 |
| 25 | 2 | `dist_home_m` | u16 |
| 27 | 1 | `tx_reason` | u8 |
| 28 | 1 | `tlv_len` | u8 |
| 29 | 3 | `hdr_rsvd` | u8[3] |

## Implementation warning

Keep protocol constants, encoder, decoder, README files, hub parser, collar transmitter, cloud parser and simulator payloads aligned with `docs/TLV_PROTOCOL_V1_1.md`.

If this library still contains legacy constants or packet helpers, update the implementation before treating firmware output as v1.1 compliant.
