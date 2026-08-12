# Bluepaws Cat Tracker TLV Telemetry Protocol v1.1

Status: locked packet-header decision, TLV section still under review  
Last updated: 2026-08-12  
Scope: collar telemetry packet, LoRa relay path, LTE-M/Cat-M1 direct path, Supabase Edge Function ingestion

## 1. Purpose

Bluepaws V4 uses a compact binary telemetry packet instead of JSON for the collar's over-the-air and cellular observation payload. The goal is to keep the packet small enough for LoRa while still carrying the minimum data required to identify, authenticate, validate, store and display a collar location fix.

The packet produced by the collar is an immutable observation packet. The same byte sequence should be sent whether the packet travels:

```text
Collar -> LoRa -> Home Hub / Relay -> HTTPS -> Supabase Edge Function
Collar -> LTE-M / Cat-M1 -> HTTPS -> Supabase Edge Function
```

The relay must not alter the original collar packet. Relay-specific metadata such as RSSI, SNR, gateway ID, ingress path and gateway receive time belongs outside the collar packet in the REST wrapper or in separate database metadata.

## 2. Current packet structure

The locked v1.1 packet structure is:

```text
[32-byte fixed header][0-24 byte collar TLV section][8-byte auth_tag_8]
```

Packet size:

```text
minimum = 32 header + 0 TLV  + 8 auth tag = 40 bytes
maximum = 32 header + 24 TLV + 8 auth tag = 64 bytes
```

There is no CRC16 in this version. CRC16 has been replaced by an 8-byte truncated HMAC-SHA256 authentication tag.

## 3. Authentication tag

The authentication tag is:

```text
auth_tag_8 = first 8 bytes of HMAC-SHA256(device_key, header + TLVs)
```

The HMAC covers:

```text
32-byte fixed header + TLV bytes
```

It does not cover itself.

Reserved header bytes must be set to zero before calculating the HMAC. If reserved bytes differ between transmission paths, the auth tag will differ and duplicate detection by raw packet hash will be weakened.

The authentication tag provides integrity and authenticity. It detects tampering and unauthorised packet injection. It does not encrypt the packet contents.

## 4. Byte order and primitive types

All multi-byte fields are little-endian.

| Type | Meaning |
|---|---|
| `u8` | unsigned 8-bit integer |
| `u16` | unsigned 16-bit integer, little-endian |
| `u32` | unsigned 32-bit integer, little-endian |
| `i32` | signed 32-bit integer, little-endian two's complement |
| `u8[n]` | fixed byte array |

No floats, strings, JSON keys or human-readable timestamps are transmitted inside the collar packet.

## 5. Locked fixed header layout, 32 bytes

`tlv_len` is deliberately the final byte of the fixed header. The next byte after it is the first byte of the TLV section if `tlv_len > 0`.

| Offset | Size | Field | Type | Description |
|---:|---:|---|---|---|
| 0 | 1 | `ver` | `u8` | Protocol version. Use `1` for this v1.1 layout unless the firmware explicitly bumps it. |
| 1 | 2 | `device_guid16` | `u16` | Immutable 16-bit collar identity. Provisioned and registered in the backend. |
| 3 | 2 | `msg_seq_id` | `u16` | Per-device message sequence. Used with `device_guid16` for deduplication. |
| 5 | 4 | `time_unix` | `u32` | UTC packet/fix timestamp in seconds. |
| 9 | 1 | `state` | `u8` | Packed status and power profile. Lower nibble is status, upper nibble is power profile. |
| 10 | 1 | `flags` | `u8` | Core boolean flags. |
| 11 | 1 | `tx_reason` | `u8` | 8-value enum explaining why the packet was sent. |
| 12 | 4 | `lat_e7` | `i32` | Latitude multiplied by 10,000,000. |
| 16 | 4 | `lon_e7` | `i32` | Longitude multiplied by 10,000,000. |
| 20 | 2 | `batt_mV` | `u16` | Battery voltage in millivolts. |
| 22 | 2 | `acc_m` | `u16` | Estimated horizontal GNSS accuracy in metres. |
| 24 | 2 | `fix_age_s` | `u16` | Age of GNSS fix in seconds. Use `65535` for unknown/not applicable. |
| 26 | 1 | `sat_count` | `u8` | Number of satellites used or visible, depending on GNSS module policy. Use `255` for unknown. |
| 27 | 4 | `hdr_rsvd` | `u8[4]` | Reserved for future fixed-header fields. Must be transmitted as all zeroes. |
| 31 | 1 | `tlv_len` | `u8` | Number of TLV bytes following the header. Valid range is `0..24`. |

Header size check:

```text
1 + 2 + 2 + 4 + 1 + 1 + 1 + 4 + 4 + 2 + 2 + 2 + 1 + 4 + 1 = 32 bytes
```

Active header fields currently use 28 bytes. Four bytes remain reserved for future fixed-header expansion.

## 6. Header field decisions

### `device_guid16`

The device identity is deliberately 16-bit in v1.1.

```text
0..65535 possible values
```

This keeps the collar packet compact. Exhausting 65,000 collar IDs would be a later-stage scaling problem that can be handled with a future protocol version, provisioning namespace, tenant prefix, or database migration.

### `msg_seq_id`

`msg_seq_id` is also 16-bit. It increments once per new collar observation, not once per transmission path. If the same observation goes out over LoRa and LTE, both paths must carry the same `msg_seq_id`.

Current deduplication key:

```text
(device_guid16, msg_seq_id)
```

No boot ID is included in the fixed header yet. Reset handling can be improved later if testing proves it is needed.

### `state`

`state` packs two nibbles:

```text
bits 0-3 = status
bits 4-7 = power_profile
```

Packing:

```text
state = (power_profile << 4) | status
```

Unpacking:

```text
status        = state & 0x0F
power_profile = (state >> 4) & 0x0F
```

### `lat_e7` and `lon_e7`

Latitude and longitude remain signed 32-bit absolute coordinates. They are not compressed to 16-bit because 16-bit absolute global coordinates would give hundreds-of-metres resolution, which is not acceptable for pet tracking.

Example:

```text
lat = 51.9058165  -> lat_e7 = 519058165
lon = -2.2394678 -> lon_e7 = -22394678
```

### Removed from fixed header

`dist_home_m` is intentionally not in the immutable collar packet. Distance from home/search hub is derived intelligence and should be calculated by the hub, cloud, or app using the current reference point. This avoids stale or misleading distance data if the hub moves or becomes portable during a search.

## 7. Status enum

Stored in the lower nibble of `state`.

| Value | Name | Meaning |
|---:|---|---|
| 0 | `HOME` | Cat/collar is considered at home. |
| 1 | `OUT` | Cat/collar is away from home but not lost. |
| 2 | `LOST` | Lost mode/state is active. |
| 3 | `ERROR` | Device or telemetry error state. |
| 4-15 | `RESERVED` | Reserved. |

## 8. Power profile enum

Stored in the upper nibble of `state`.

| Value | Name | Meaning |
|---:|---|---|
| 0 | `POWER_SAVE` | Reduced reporting and low power operation. |
| 1 | `NORMAL` | Standard tracking behaviour. |
| 2 | `ACTIVE` | Higher frequency tracking. |
| 3 | `LOST_ALERT` | Urgent lost-alert behaviour. |
| 4-15 | `RESERVED` | Reserved. |

Examples:

```text
0x11 = OUT + NORMAL
0x32 = LOST + LOST_ALERT
0x00 = HOME + POWER_SAVE
```

## 9. Flags bitfield

`flags` is a one-byte bitfield.

| Bit | Mask | Name | Meaning |
|---:|---:|---|---|
| 0 | `0x01` | `GNSS_VALID` | `lat_e7` and `lon_e7` are valid. |
| 1 | `0x02` | `FIX_3D` | GNSS fix is 3D. |
| 2 | `0x04` | `LOW_BATTERY` | Battery is below configured threshold. |
| 3 | `0x08` | `HOME_BEACON_SEEN` | BLE/home beacon detected this cycle. |
| 4 | `0x10` | `GEOFENCE_BREACHED` | Collar is outside the configured geofence. |
| 5 | `0x20` | `CHARGING` | Collar is charging. |
| 6 | `0x40` | `STALE_FIX` | Location was not freshly acquired. Interpret `fix_age_s`. |
| 7 | `0x80` | `ERROR_PRESENT` | Error exists. Detail can be in TLV or backend metadata. |

Example:

```text
GNSS_VALID + FIX_3D + GEOFENCE_BREACHED = 0x01 + 0x02 + 0x10 = 0x13
```

## 10. TX reason enum

`tx_reason` is one byte, but only values `0..7` are valid in v1.1.

| Value | Name | Meaning |
|---:|---|---|
| 0 | `TELEMETRY` | Normal scheduled location/status update. |
| 1 | `ACK` | Acknowledgement or response to a command. |
| 2 | `PING` | Response to a user, app, cloud or hub ping. |
| 3 | `INTERRUPT` | Triggered by motion, geofence, button, sensor or similar interrupt. |
| 4 | `BOOT` | Cold start, reboot, brownout recovery or first report after boot. |
| 5 | `ALERT` | Important event such as lost alert, low battery or error. |
| 6 | `CONFIG` | Config/settings changed, applied or reported. |
| 7 | `RESERVED` | Reserved for future use. |

Do not keep expanding `tx_reason`. Use TLVs for detail.

## 11. TLV section, 0 to 24 bytes

TLV means Type, Length, Value.

```text
[type:u8][len:u8][value:len bytes]
```

Rules:

1. `tlv_len` is the final byte of the fixed header at offset 31.
2. Valid `tlv_len` range is `0..24`.
3. Unknown TLV types must be skipped using the length byte.
4. The parser must bounds-check every TLV before reading its value.
5. The sender must not pad TLV bytes. Transmit only actual TLV bytes.
6. The HMAC is appended immediately after the last TLV byte.
7. Transport-specific metadata must not be added inside the immutable collar TLV section.

## 12. Transport metadata separation

The collar packet must remain identical across LoRa relay and LTE direct transmission. Do not include path-specific information in the collar packet.

Do not put these inside the immutable packet:

| Metadata | Correct location |
|---|---|
| LoRa RSSI | REST wrapper or `observation_paths` table |
| LoRa SNR | REST wrapper or `observation_paths` table |
| gateway/hub ID | REST wrapper or `observation_paths` table |
| ingress path | REST wrapper or `observation_paths` table |
| gateway receive time | REST wrapper or `observation_paths` table |
| LTE RSRP/RSRQ/SINR | wrapper, diagnostics event, or separate metadata table |
| cell ID/TAC/band | wrapper, diagnostics event, or separate metadata table |

Example relay wrapper:

```json
{
  "ingest_path": "hub_lora_relay",
  "gateway_guid": "hub-000016",
  "rx_rssi_dbm": -92,
  "rx_snr_db": 6.25,
  "gateway_rx_time_unix": 1786537811,
  "payload_b64": "BASE64_OF_UNMODIFIED_COLLAR_PACKET"
}
```

Example LTE direct wrapper:

```json
{
  "ingest_path": "collar_lte_direct",
  "payload_b64": "SAME_BASE64_OF_UNMODIFIED_COLLAR_PACKET"
}
```

## 13. Supabase ingestion model

The Supabase Edge Function should:

1. Receive raw packet bytes directly or decode `payload_b64` from a wrapper.
2. Check total length is between 40 and 64 bytes.
3. Read the 32-byte header.
4. Validate `tlv_len <= 24`.
5. Verify `packet_len == 32 + tlv_len + 8`.
6. Look up the device key using `device_guid16`.
7. Verify `auth_tag_8` using HMAC-SHA256 over header + TLVs.
8. Decode header fields.
9. Parse TLVs.
10. Insert into `observations` using unique key `(device_guid16, msg_seq_id)`.
11. Store path metadata separately in `observation_paths` if provided.

Suggested observation unique constraint:

```sql
UNIQUE (device_guid16, msg_seq_id)
```

Suggested tables:

```text
observations
- device_guid16
- msg_seq_id
- time_unix
- status
- power_profile
- flags
- tx_reason
- lat
- lon
- batt_mV
- acc_m
- fix_age_s
- sat_count
- tlv_json
- payload_hash
- received_at

observation_paths
- device_guid16
- msg_seq_id
- ingest_path
- gateway_guid
- rx_rssi_dbm
- rx_snr_db
- gateway_rx_time_unix
- received_at
```

## 14. Decoder procedure

```text
if packet_len < 40 or packet_len > 64: reject
header = packet[0:32]
tlv_len = header[31]
if tlv_len > 24: reject
expected_len = 32 + tlv_len + 8
if packet_len != expected_len: reject
body = packet[0:32 + tlv_len]
rx_tag = packet[32 + tlv_len : 32 + tlv_len + 8]
expected_tag = first_8_bytes(HMAC_SHA256(device_key, body))
if rx_tag != expected_tag: reject
parse header
parse TLVs from packet[32 : 32 + tlv_len]
```

## 15. Example minimum packet decoded to JSON

A packet with no TLVs still carries the full minimum observation data.

```json
{
  "protocol_version": 1,
  "device_guid16": "04A7",
  "msg_seq_id": 10542,
  "dedup_key": "04A7:10542",
  "time_unix": 1786537810,
  "status": "OUT",
  "power_profile": "NORMAL",
  "flags": ["GNSS_VALID", "FIX_3D", "GEOFENCE_BREACHED"],
  "tx_reason": "TELEMETRY",
  "location": {
    "lat": 51.9058165,
    "lon": -2.2394678,
    "accuracy_m": 12,
    "fix_age_s": 4,
    "sat_count": 8
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
32 header + 0 TLV + 8 auth tag = 40 bytes
```

## 16. Example header byte layout

Example values:

```text
ver            = 1
device_guid16  = 0x04A7
msg_seq_id     = 10542
time_unix      = 1786537810
state          = 0x11    # OUT + NORMAL
flags          = 0x13    # GNSS_VALID + FIX_3D + GEOFENCE_BREACHED
tx_reason      = 0       # TELEMETRY
lat_e7         = 519058165
lon_e7         = -22394678
batt_mV        = 3700
acc_m          = 12
fix_age_s      = 4
sat_count      = 8
hdr_rsvd       = 00 00 00 00
tlv_len        = 0
```

Little-endian header bytes, before auth tag:

```text
01
A7 04
2E 29
12 34 7D 6A
11
13
00
F5 32 F0 1E
CA 48 AA FE
74 0E
0C 00
04 00
08
00 00 00 00
00
```

## 17. Current locked decisions

- Fixed header is 32 bytes.
- Active header fields use 28 bytes.
- Four reserved header bytes remain.
- `tlv_len` is the final byte of the header at offset 31.
- TLV budget is 0 to 24 bytes.
- 8-byte truncated HMAC-SHA256 replaces CRC16.
- Packet size is 40 to 64 bytes.
- Device identity is `device_guid16`, not `device_guid32`.
- Message sequence is `msg_seq_id` as u16.
- Header includes `fix_age_s` and `sat_count`.
- Header does not include `dist_home_m`.
- Transport metadata must remain outside the immutable collar packet.
- Duplicate observations are identified by `(device_guid16, msg_seq_id)`.
