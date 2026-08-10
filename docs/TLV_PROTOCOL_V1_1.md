# Bluepaws Cat Tracker TLV Telemetry Protocol v1.1

Status: Draft implementation specification
Last updated: 2026-08-10
Scope: Collar, LoRa home puck or hub, LTE-M or Cat-M1 direct uplink, REST cloud ingestion

## Purpose

Bluepaws V4 is moving away from JSON telemetry for over-the-air packets. JSON is useful for debugging because it is human-readable, but it is inefficient on constrained radio links because it repeats field names, uses ASCII numbers, includes punctuation, and commonly carries floats and strings.

The selected telemetry format is a compact binary packet:

```text
[32-byte fixed header][0-30 bytes optional TLVs][2-byte CRC16]
```

The packet is variable length, but capped at 64 bytes.

```text
total_len = 32 + tlv_len + 2
minimum   = 34 bytes
maximum   = 64 bytes
```

The fixed header is deliberately strong enough to be useful even when `tlv_len = 0`. TLVs carry optional enrichment, diagnostics, link metadata, and command or acknowledgement detail.

## Transport model

The TLV packet is the payload. It is not the transport.

Typical paths:

```text
Collar -> LoRa -> Home puck or hub -> HTTPS REST POST -> Cloud
Collar -> LTE-M or Cat-M1 -> HTTPS REST POST -> Cloud
```

Preferred cloud upload:

```http
POST /api/v1/telemetry
Content-Type: application/octet-stream
Authorization: Bearer <device_token>

<raw TLV packet bytes>
```

Debug-friendly alternative:

```json
{
  "payload_b64": "<base64 encoded TLV packet>",
  "ingest_path": "puck_relay",
  "gateway_guid32": "00000016",
  "rx_rssi_dbm": -92,
  "rx_snr_db": 6.25
}
```

For production, raw binary POST is preferred. JSON wrappers are only for debugging, admin tooling, exports, and manual testing.

## Byte order and encoding rules

All multi-byte integer fields are little-endian.

No floats, strings, JSON keys, or human-readable timestamps are sent in the radio payload.

| Human value | Encoded field | Example |
|---|---|---|
| Latitude | `lat_e7 = latitude x 10000000` | `51.9058165` becomes `519058165` |
| Longitude | `lon_e7 = longitude x 10000000` | `-2.2394678` becomes `-22394678` |
| Battery voltage | `batt_mV` in millivolts | `3.700 V` becomes `3700` |
| Accuracy | `acc_m` in metres | `12 m` becomes `12` |
| Distance from home | `dist_home_m` in metres | `3571 m` becomes `3571` |
| Speed | `speed_cms` in centimetres per second | `0.62 m/s` becomes `62` |
| HDOP | `hdop_x10 = HDOP x 10` | `1.2` becomes `12` |
| SNR | `lora_snr_x4 = SNR x 4` | `6.25 dB` becomes `25` |
| Temperature | `temp_c_x10 = degrees C x 10` | `23.5 C` becomes `235` |

## Fixed header, 32 bytes

| Offset | Size | Field | Type | Required | Description |
|---:|---:|---|---|---|---|
| 0 | 1 | `ver` | u8 | Yes | Protocol version. v1.1 uses value `1` unless firmware elects to bump this later. |
| 1 | 4 | `device_guid32` | u32 | Yes | Immutable 32-bit collar identity. Replaces earlier 16-bit device ID. |
| 5 | 2 | `msg_seq_id` | u16 | Yes | Per-device message sequence. Increments once for each new observation. |
| 7 | 4 | `time_unix` | u32 | Yes | UTC timestamp in seconds. Usually fix time or packet creation time. |
| 11 | 1 | `state` | u8 | Yes | Packed status and power profile. Lower nibble is status, upper nibble is power profile. |
| 12 | 1 | `flags` | u8 | Yes | Core bitfield. |
| 13 | 4 | `lat_e7` | i32 | Yes | Latitude x 10000000. |
| 17 | 4 | `lon_e7` | i32 | Yes | Longitude x 10000000. |
| 21 | 2 | `batt_mV` | u16 | Yes | Battery voltage in millivolts. |
| 23 | 2 | `acc_m` | u16 | Yes | Estimated horizontal accuracy in metres. |
| 25 | 2 | `dist_home_m` | u16 | Yes | Distance from configured home location in metres. Saturate if over range. |
| 27 | 1 | `tx_reason` | u8 | Yes | 8-value enum explaining why this packet was sent. |
| 28 | 1 | `tlv_len` | u8 | Yes | Number of TLV bytes following. Must be 0 to 30. |
| 29 | 3 | `hdr_rsvd` | u8[3] | Yes | Reserved. Set all three bytes to `0x00`. Receivers must ignore. |

Header size check:

```text
1 + 4 + 2 + 4 + 1 + 1 + 4 + 4 + 2 + 2 + 2 + 1 + 1 + 3 = 32 bytes
```

## Header field notes

### `device_guid32`

A 32-bit immutable collar identity. This replaces the earlier 16-bit identifier because 65,536 values is too tight for commercial scaling once prototypes, test units, replacements, returned units, future product lines, and reserved ranges are considered.

Example:

```text
device_guid32 = 0x00A7F134
```

The backend should register each GUID during provisioning.

### `msg_seq_id`

A 16-bit per-device sequence counter. It increments once per new observation. It does not increment per transport path.

Current decision: no `boot_id` in the header yet. This saves bytes. A future version may add stronger reset disambiguation if needed.

### `state`

The state byte packs two 4-bit enums into one byte.

```text
bits 0-3 = status
bits 4-7 = power_profile
```

Packing formula:

```text
state = (power_profile << 4) | status
```

Unpacking formula:

```text
status        = state & 0x0F
power_profile = (state >> 4) & 0x0F
```

Example:

```text
status = OUT = 1
power_profile = NORMAL = 1
state = (1 << 4) | 1 = 0x11
```

### `lat_e7` and `lon_e7`

Signed 32-bit absolute coordinates. These remain 32-bit even though GNSS may only be accurate to around 2 metres. Reducing absolute latitude and longitude to 16 bits would reduce coordinate resolution to hundreds of metres globally, which is not acceptable.

### `dist_home_m`

Distance from configured home location in metres. This replaces keeping HDOP in the header because it is more immediately useful for product behaviour and display.

| Encoded value | Meaning |
|---:|---|
| 0 | At home, or exactly at home position |
| 25 | 25 m from home |
| 3571 | 3.571 km from home |
| 65535 | Saturated, over range, or unknown depending on flags and policy |

## Status enum

`status` is stored in the lower nibble of `state`.

| Value | Name | Meaning |
|---:|---|---|
| 0 | `HOME` | Cat or collar is considered at home. |
| 1 | `OUT` | Cat is away from home but not lost. |
| 2 | `LOST` | Cat is in lost state. |
| 3 | `ERROR` | Collar or telemetry is in an error state. |
| 4-15 | `RESERVED` | Reserved for future use. |

## Power profile enum

`power_profile` is stored in the upper nibble of `state`.

| Value | Name | Meaning |
|---:|---|---|
| 0 | `POWER_SAVE` | Lowest update rate, conserve battery. |
| 1 | `NORMAL` | Standard tracking behaviour. |
| 2 | `ACTIVE` | Higher frequency tracking. |
| 3 | `LOST_ALERT` | Urgent lost-alert tracking behaviour. |
| 4-15 | `RESERVED` | Reserved for future use. |

Examples:

```text
state = 0x11 -> status OUT, power profile NORMAL
state = 0x32 -> status LOST, power profile LOST_ALERT
state = 0x00 -> status HOME, power profile POWER_SAVE
```

## Flags bitfield

`flags` is a one-byte bitfield.

| Bit | Mask | Name | Meaning |
|---:|---:|---|---|
| 0 | `0x01` | `GNSS_VALID` | Latitude and longitude are valid. |
| 1 | `0x02` | `FIX_3D` | GNSS fix is 3D. |
| 2 | `0x04` | `LOW_BATTERY` | Battery is below configured threshold. |
| 3 | `0x08` | `HOME_BEACON_SEEN` | BLE or home beacon was detected this cycle. |
| 4 | `0x10` | `GEOFENCE_BREACHED` | Device is outside the configured geofence. |
| 5 | `0x20` | `CHARGING` | Device is charging. |
| 6 | `0x40` | `STALE_FIX` | Location was not freshly acquired. Include `fix_age_s` TLV if useful. |
| 7 | `0x80` | `ERROR_PRESENT` | An error exists. Include error TLV for detail if available. |

Example:

```text
GNSS_VALID + FIX_3D + GEOFENCE_BREACHED = 0x01 + 0x02 + 0x10 = 0x13
```

## TX reason enum

`tx_reason` is one byte, but only values 0 to 7 are valid in this version.

| Value | Name | Meaning |
|---:|---|---|
| 0 | `TELEMETRY` | Normal scheduled location or status update. |
| 1 | `ACK` | Acknowledgement or response to a command. |
| 2 | `PING` | Response to a user, app or cloud ping. |
| 3 | `INTERRUPT` | Triggered by motion, geofence, button, sensor or similar interrupt. |
| 4 | `BOOT` | Cold start, reboot, brownout recovery or first report after boot. |
| 5 | `ALERT` | Important event such as lost alert, low battery, or error. |
| 6 | `CONFIG` | Config or settings changed, applied or reported. |
| 7 | `RESERVED` | Reserved for future use. |

Detail should be added by TLV rather than expanding this enum.

## TLV section

TLV means Type, Length, Value.

Each TLV is:

```text
[type:u8][len:u8][value:len bytes]
```

Rules:

1. TLVs are optional.
2. TLVs may appear in any order.
3. Unknown TLV types must be skipped using the length byte.
4. A sender must not exceed 30 total TLV bytes.
5. A receiver must bounds-check every TLV before reading it.
6. A receiver must reject the packet if `tlv_len > 30`.
7. A receiver must reject the packet if CRC fails.

## TLV type index

### GNSS and motion TLVs

| Type | Name | Length | Value type | Example value | JSON mapping | Notes |
|---:|---|---:|---|---|---|---|
| `0x01` | `sat_count` | 1 | u8 | `8` | `gnss.sat_count` | Demoted from header to save space. |
| `0x02` | `hdop_x10` | 1 | u8 | `12` means 1.2 | `gnss.hdop` | Optional diagnostic. Header has `acc_m`. |
| `0x03` | `speed_cms` | 2 | u16 | `62` means 0.62 m/s | `motion.speed_mps` | Optional because backend can infer rough movement. |
| `0x04` | `course_deg` | 2 | u16 | `180` | `motion.course_deg` | Optional direction of travel. |
| `0x05` | `fix_type` | 1 | u8 | `2` means 3D | `gnss.fix_type` | 0 none, 1 2D, 2 3D. |
| `0x06` | `alt_m` | 2 | i16 | `80` | `location.alt_m` | Optional altitude. |
| `0x08` | `ttff_s` | 2 | u16 | `35` | `gnss.ttff_s` | Time to first fix. |
| `0x09` | `fix_age_s` | 2 | u16 | `900` | `gnss.fix_age_s` | Use when `STALE_FIX` flag is set. |

`0x07` is reserved.

### LoRa and relay TLVs

| Type | Name | Length | Value type | Example value | JSON mapping | Notes |
|---:|---|---:|---|---|---|---|
| `0x10` | `lora_rssi_dbm` | 1 | i8 | `-92` | `radio.lora.rssi_dbm` | Usually added by receiver or puck. |
| `0x11` | `lora_snr_x4` | 1 | i8 | `25` means 6.25 dB | `radio.lora.snr_db` | Divide by 4 when decoding. |
| `0x12` | `gateway_guid32` | 4 | u32 | `0x00000016` | `relay.gateway_guid32` | Puck or hub identity. |
| `0x13` | `ingest_path` | 1 | u8 | `2` means puck relay | `ingest.path` | 0 collar LoRa, 1 collar cell, 2 puck relay, 3 hub WiFi relay. |
| `0x14` | `lora_profile` | 3 | bytes | `[8,0,5]` | `radio.lora.profile` | SF, BW index, CR denominator. |
| `0x15` | `tx_power_dbm` | 1 | i8 | `12` | `radio.lora.tx_power_dbm` | Radio output setting, not EIRP. |
| `0x16` | `gateway_rx_time_unix` | 4 | u32 | `1762647218` | `relay.gateway_rx_time_unix` | When relay received LoRa packet. |

### Device health TLVs

| Type | Name | Length | Value type | Example value | JSON mapping | Notes |
|---:|---|---:|---|---|---|---|
| `0x20` | `reset_reason` | 1 | u8 | `1` means watchdog | `device.reset_reason` | Use with `tx_reason = BOOT` or error events. |
| `0x21` | `uptime_s` | 4 | u32 | `86400` | `device.uptime_s` | Seconds since boot. |
| `0x22` | `fw_ver` | 2 | u16 | `0x0103` | `device.fw_ver` | Suggested encoding: `(major << 8) | minor`. |
| `0x23` | `cfg_rev` | 2 | u16 | `42` | `device.cfg_rev` | Increment when settings change. |
| `0x24` | `err_flags` | 2 | u16 | `0x0002` | `device.err_flags` | Extended errors beyond header flag. |
| `0x25` | `temp_c_x10` | 2 | i16 | `235` means 23.5 C | `device.temp_c` | Optional temperature. |
| `0x26` | `vcc_mV` | 2 | u16 | `3300` | `device.vcc_mV` | Internal rail measurement. |

Reset reason enum for TLV `0x20`: `0 NORMAL`, `1 WATCHDOG`, `2 BROWNOUT`, `3 HARDFAULT`, `4 SOFTWARE_RESET`, `255 UNKNOWN`.

### Behaviour and command detail TLVs

| Type | Name | Length | Value type | Example value | JSON mapping | Notes |
|---:|---|---:|---|---|---|---|
| `0x30` | `interrupt_source` | 1 | u8 | `1` means motion | `event.interrupt_source` | Detail for `tx_reason = INTERRUPT`. |
| `0x31` | `alert_reason` | 1 | u8 | `2` means lost | `event.alert_reason` | Detail for `tx_reason = ALERT`. |
| `0x32` | `sleep_s` | 2 | u16 | `600` | `power.next_sleep_s` | Next intended sleep duration. |
| `0x50` | `acked_msg_seq_id` | 2 | u16 | `10541` | `ack.msg_seq_id` | Sequence being acknowledged. |
| `0x51` | `ack_status` | 1 | u8 | `0` means OK | `ack.status` | Command response status. |
| `0x52` | `command_id` | 2 | u16 | `77` | `ack.command_id` | Optional command identifier. |

### Cellular TLVs

| Type | Name | Length | Value type | Example value | JSON mapping | Notes |
|---:|---|---:|---|---|---|---|
| `0x40` | `cell_rsrp_dbm` | 2 | i16 | `-104` | `radio.cell.rsrp_dbm` | LTE signal strength. |
| `0x41` | `cell_rsrq_db_x10` | 1 | i8 | `-95` means -9.5 dB | `radio.cell.rsrq_db` | Divide by 10. |
| `0x42` | `cell_sinr_db_x2` | 1 | i8 | `14` means 7 dB | `radio.cell.sinr_db` | Divide by 2. |
| `0x43` | `cell_id_u32` | 4 | u32 | `12345678` | `radio.cell.cell_id` | Cell ID. |
| `0x44` | `tac_u16` | 2 | u16 | `3321` | `radio.cell.tac` | Tracking area code. |
| `0x45` | `earfcn_u16` | 2 | u16 | `6300` | `radio.cell.earfcn` | Channel number. |
| `0x46` | `lte_band` | 1 | u8 | `20` | `radio.cell.band` | LTE band. |

## CRC16

Use CRC-16/CCITT-FALSE.

| Parameter | Value |
|---|---|
| Width | 16 bits |
| Polynomial | `0x1021` |
| Initial value | `0xFFFF` |
| Reflected input | No |
| Reflected output | No |
| Final XOR | `0x0000` |

CRC covers header plus TLVs. CRC does not include the CRC bytes themselves. Append the CRC as little-endian u16.

If CRC fails, discard the packet.

## Deduplication model

The backend should treat `(device_guid32, msg_seq_id)` as the observation deduplication key.

Example:

```text
device_guid32 = 0x00A7F134
msg_seq_id    = 10542

dedup_key = 00A7F134:10542
```

If the same observation arrives from direct cellular and puck relay, both should preserve the same `device_guid32` and `msg_seq_id`. The backend should store one observation and optionally add metadata showing that it was seen through multiple paths.

Important limitation: `msg_seq_id` is currently 16-bit. This is a deliberate byte-saving decision. Reset handling may be improved later with a `boot_id`, session ID, or backend reset-detection logic.

## Parsing procedure

Decoder rules:

1. Check total length is at least 34 bytes and at most 64 bytes.
2. Read the 32-byte header.
3. Verify `ver` is supported.
4. Verify `tlv_len <= 30`.
5. Verify `total_len == 32 + tlv_len + 2`.
6. Verify CRC16 over bytes `0` to `total_len - 3`.
7. Decode header fields.
8. Parse TLVs until exactly `tlv_len` bytes have been consumed.
9. Ignore unknown TLV types.
10. Reject malformed TLVs where `pos + 2 + len` exceeds the TLV boundary.

Pseudocode:

```text
if total_len < 34 or total_len > 64: reject
header = bytes[0:32]
tlv_len = header[28]
if tlv_len > 30: reject
expected_len = 32 + tlv_len + 2
if total_len != expected_len: reject
if crc16(bytes[0:expected_len-2]) != rx_crc: reject

pos = 32
end = 32 + tlv_len
while pos < end:
    if pos + 2 > end: reject
    type = bytes[pos]
    len  = bytes[pos + 1]
    pos += 2
    if pos + len > end: reject
    value = bytes[pos:pos+len]
    decode_or_skip(type, len, value)
    pos += len
```

## Encoding procedure

Sender rules:

1. Populate the 32-byte header.
2. Set reserved header bytes to zero.
3. Build any optional TLVs.
4. Ensure total TLV length is 0 to 30 bytes.
5. Set `tlv_len` in the header.
6. Compute CRC16 over header plus TLVs.
7. Append CRC16 little-endian.
8. Transmit `32 + tlv_len + 2` bytes only. Do not pad to 64 bytes.

## Minimum packet JSON view

For a packet with `tlv_len = 0`:

```json
{
  "protocol_version": 1,
  "device_guid32": "00A7F134",
  "msg_seq_id": 10542,
  "dedup_key": "00A7F134:10542",
  "time_unix": 1762647216,
  "status": "OUT",
  "power_profile": "NORMAL",
  "tx_reason": "TELEMETRY",
  "flags": ["GNSS_VALID", "FIX_3D", "GEOFENCE_BREACHED"],
  "location": {
    "lat": 51.9058165,
    "lon": -2.2394678,
    "accuracy_m": 12,
    "distance_from_home_m": 3571
  },
  "battery": {
    "mV": 3700,
    "volts": 3.7
  },
  "tlvs": {}
}
```

Packet size:

```text
32 header + 0 TLV + 2 CRC = 34 bytes
```

## Enriched packet JSON view

Example TLVs: sat count, HDOP, course, uptime, reset reason, LoRa RSSI and LoRa SNR.

```json
{
  "protocol_version": 1,
  "device_guid32": "00A7F134",
  "msg_seq_id": 10542,
  "dedup_key": "00A7F134:10542",
  "time_unix": 1762647216,
  "status": "OUT",
  "power_profile": "NORMAL",
  "tx_reason": "TELEMETRY",
  "flags": ["GNSS_VALID", "FIX_3D", "GEOFENCE_BREACHED"],
  "location": {
    "lat": 51.9058165,
    "lon": -2.2394678,
    "accuracy_m": 12,
    "distance_from_home_m": 3571
  },
  "battery": {
    "mV": 3700,
    "volts": 3.7
  },
  "gnss": {
    "sat_count": 8,
    "hdop": 1.2
  },
  "motion": {
    "course_deg": 180
  },
  "device": {
    "uptime_s": 86400,
    "reset_reason": "NORMAL"
  },
  "radio": {
    "lora": {
      "rssi_dbm": -92,
      "snr_db": 6.25
    }
  }
}
```

With 25 bytes of TLVs, total size is:

```text
32 + 25 + 2 = 59 bytes
```

## Example byte breakdown

Header values:

```text
ver            = 1
device_guid32  = 0x00A7F134
msg_seq_id     = 10542
time_unix      = 1762647216
state          = 0x11
flags          = 0x13
lat_e7         = 519058165
lon_e7         = -22394678
batt_mV        = 3700
acc_m          = 12
dist_home_m    = 3571
tx_reason      = 0 TELEMETRY
tlv_len        = 25
hdr_rsvd       = 00 00 00
```

Little-endian header bytes before TLVs:

```text
01
34 F1 A7 00
2E 29
B0 DC 0F 69
11
13
F5 32 F0 1E
CA 48 AA FE
74 0E
0C 00
F3 0D
00
19
00 00 00
```

Example TLV bytes:

```text
01 01 08              # sat_count = 8
02 01 0C              # hdop_x10 = 12
04 02 B4 00           # course_deg = 180
21 04 80 51 01 00     # uptime_s = 86400
20 01 00              # reset_reason = normal
10 01 A4              # lora_rssi_dbm = -92 as i8
11 01 19              # lora_snr_x4 = 25, meaning 6.25 dB
```

CRC16 is appended after the TLVs.

## Database mapping

Suggested table fields for the main observation table:

| Column | Source |
|---|---|
| `device_guid32` | Header |
| `msg_seq_id` | Header |
| `dedup_key` | Derived |
| `time_unix` | Header |
| `received_at` | Backend ingest time |
| `status` | Header state lower nibble |
| `power_profile` | Header state upper nibble |
| `flags` | Header |
| `lat` | Header `lat_e7 / 1e7` |
| `lon` | Header `lon_e7 / 1e7` |
| `batt_mV` | Header |
| `acc_m` | Header |
| `dist_home_m` | Header |
| `tx_reason` | Header |
| `tlv_json` | Parsed optional TLVs |
| `ingest_path` | Wrapper or TLV |
| `gateway_guid32` | Wrapper or TLV |

Unique constraint:

```sql
UNIQUE (device_guid32, msg_seq_id)
```

## Implementation checklist

Encoder checklist:

- [ ] Fill all header fields.
- [ ] Pack state from status and power profile.
- [ ] Encode lat/lon as signed e7 integers.
- [ ] Encode battery in millivolts.
- [ ] Encode distance from home in metres.
- [ ] Add optional TLVs only where useful.
- [ ] Set `tlv_len` correctly.
- [ ] Set `hdr_rsvd` bytes to zero.
- [ ] Compute CRC16 over header plus TLVs.
- [ ] Send only actual bytes, not padded 64-byte buffers.

Decoder checklist:

- [ ] Check packet length between 34 and 64 bytes.
- [ ] Validate `tlv_len <= 30`.
- [ ] Validate total length equals `32 + tlv_len + 2`.
- [ ] Validate CRC16.
- [ ] Decode state into status and power profile.
- [ ] Decode flags to named booleans.
- [ ] Decode location, battery, accuracy and distance.
- [ ] Parse TLVs with strict bounds checks.
- [ ] Ignore unknown TLVs safely.
- [ ] Deduplicate using `(device_guid32, msg_seq_id)`.

## Current record of decisions

- Unified binary TLV packet replaces JSON over LoRa and cellular.
- Packet is variable length, capped at 64 bytes.
- Fixed header is exactly 32 bytes.
- TLV section is 0 to 30 bytes.
- CRC16 is 2 bytes.
- Device identity is now `device_guid32` as u32.
- Message sequence is now `msg_seq_id` as u16 to save bytes.
- No boot ID in v1.1.
- State byte packs status and power profile.
- Flags reduced to u8.
- Absolute lat/lon stay as i32 e7 values.
- `dist_home_m` is in the header.
- `sat_count`, `hdop_x10`, `course_deg` and `fix_age_s` are optional TLVs.
- `tx_reason` is an 8-value enum.
- RESTful HTTPS POST is the v1 cloud transport.
- MQTT is not required for v1.
