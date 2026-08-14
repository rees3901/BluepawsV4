# Bluepaws Cat Tracker TLV Telemetry Protocol v1.1

Status: locked v1.1 packet header, TLV set and transport wrapper contract; implementation validation remains active
Last updated: 2026-08-13  
Scope: collar telemetry packet, LoRa collar-to-hub path, LTE-M/Cat-M1 direct path, hub relay path, Supabase Edge Function ingestion

## 1. Purpose

Bluepaws V4 uses a compact binary telemetry packet instead of JSON for the collar's immutable observation payload. The aim is to keep the collar packet small enough for LoRa while still carrying the minimum data required to identify, authenticate, validate, store, deduplicate and display a location fix.

The collar-generated packet is deliberately separated from transport metadata.

The collar packet answers:

```text
What did the collar report?
```

The transport wrapper answers:

```text
How did this packet reach the cloud, and what was the link quality?
```

This separation is critical because the same collar observation may arrive through more than one path:

```text
Collar -> LoRa -> Home Hub / Relay -> HTTPS -> Supabase Edge Function
Collar -> LTE-M / Cat-M1 -> HTTPS -> Supabase Edge Function
```

The inner collar packet must remain byte-for-byte identical across both paths.

## 2. Current packet structure

The locked v1.1 collar packet structure is:

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

Reserved header bytes must be set to zero before calculating the HMAC. If reserved bytes differ between transmission paths, the authentication tag will differ and the raw packet hash will no longer match.

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
| 0 | 1 | `ver` | `u8` | Protocol version. Use `1` for this v1.1 layout unless firmware explicitly bumps it. |
| 1 | 2 | `device_guid16` | `u16` | Immutable 16-bit collar identity. Provisioned and registered in the backend. |
| 3 | 2 | `msg_seq_id` | `u16` | Per-device rolling message sequence. It is diagnostic identity, not the sole deduplication key. |
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

The counter wraps after `65535`. It therefore cannot be a permanent globally
unique key. The backend uses a SHA-256 hash of the complete authenticated
packet as its canonical retry/delivery deduplication key. The tuple below is
retained as a collision/anomaly check within the device timeline:

```text
(device_guid16, msg_seq_id, time_unix)
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

`dist_home_m` is intentionally not in the immutable collar packet. Distance from home or search hub is derived intelligence and should be calculated by the hub, cloud, or app using the current reference point. This avoids stale or misleading distance data if the hub moves or becomes portable during a search.

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

`tx_reason` is one byte, but only values `0..6` are assigned in v1.1. Value
`7` is reserved and receivers must reject it until a later contract assigns it.

| Value | Name | Meaning |
|---:|---|---|
| 0 | `TELEMETRY` | Normal scheduled location/status update containing the standard fix/status data. |
| 1 | `ACK` | Acknowledgement or response to a command/message. Use `acked_msg_seq_id` TLV when relevant. |
| 2 | `PING` | Response to a user, app, cloud or hub ping. |
| 3 | `INTERRUPT` | Triggered by motion, geofence, button, sensor or similar interrupt. |
| 4 | `BOOT` | Cold start, reboot, brownout recovery or first report after boot. |
| 5 | `ALERT` | Important event such as lost alert, low battery or error. |
| 6 | `CONFIG` | Config/settings changed, applied or reported. |
| 7 | `WAKE_CHECKIN` | Lightweight wake-up presence check-in. Used when the collar wakes, may see the BLE home beacon, reports last-seen/presence, then opens a short command receive window. |

`WAKE_CHECKIN` is intended for low-cost presence and command-poll behaviour, not full diagnostic telemetry. It tells Supabase that the collar is alive and reachable, updates last-seen status, and allows the backend/user app to queue configuration commands for the receive window.

Do not keep expanding `tx_reason` without a protocol version bump. Use TLVs for detail where possible.

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

## 12. Current selected collar TLVs

These TLVs are collar-generated and may be included inside the authenticated collar packet.

| Type | Name | Length | Value type | Total TLV cost | Purpose |
|---:|---|---:|---|---:|---|
| `0x04` | `fw_ver` | 2 | `u16` | 4 bytes | Firmware version, suggested encoding `(major << 8) | minor`. |
| `0x06` | `reset_reason` | 1 | `u8` | 3 bytes | Reset/cold-start diagnostic. Most useful with `tx_reason = BOOT`. |
| `0x10` | `uptime_s` | 4 | `u32` | 6 bytes | Seconds since boot. Useful for reset detection and diagnostics. |
| `0x13` | `activity_score` | 1 | `u8` | 3 bytes | Simple activity/movement score from accelerometer or motion logic. |
| `0x20` | `acked_msg_seq_id` | 2 | `u16` | 4 bytes | Sequence number of command/message being acknowledged. |

Current selected TLV total if all are sent together:

```text
4 + 3 + 6 + 3 + 4 = 20 bytes
```

Current remaining TLV headroom:

```text
24 - 20 = 4 bytes
```

`command_id` was deliberately omitted. The collar should normally ACK the sequence number of the command/message it received. Supabase already knows what command was associated with that sequence number.

`speed_cms` and `ack_status` were also omitted to keep the selected set under budget. Speed can be estimated by backend position deltas. ACK status is unnecessary for the first version because the ACK mainly confirms receipt, and the next telemetry packet confirms whether the state actually changed.

## 13. TLVs omitted from the collar packet for now

| Omitted TLV | Reason |
|---|---|
| `speed_cms` | Useful, but backend can infer rough speed from position changes. |
| `course_deg` | Nice to have, but not essential. |
| `hdop_x10` | Header already includes `acc_m`, `fix_age_s` and `sat_count`. |
| `cfg_rev` | Useful later, but not required in current selected set. |
| `err_flags` | Useful later, but reset reason plus `ERROR_PRESENT` flag are enough for v1.1. |
| `temp_c_x10` | Nice diagnostic, not core telemetry. |
| `vcc_mV` | Nice power diagnostic, but `batt_mV` is already in the header. |
| `sleep_s` | Useful power/profile diagnostic, but not essential. |
| `gnss_ttff_s` | Useful GNSS diagnostic, but not essential. |
| `ack_status` | Omitted. `tx_reason = ACK` plus `acked_msg_seq_id` is enough for receipt acknowledgement. |
| `command_id` | Omitted. Supabase can map the ACKed sequence ID to the original command. |
| `interrupt_source` | Could be useful later, but `tx_reason = INTERRUPT` covers the broad event. |
| `alert_reason` | Could be useful later, but `tx_reason`, `state` and `flags` cover the broad event. |

## 14. Transport metadata separation

The collar packet must remain identical across LoRa relay and LTE direct transmission. Do not include path-specific information in the collar packet.

Do not put these inside the immutable packet:

| Metadata | Correct location |
|---|---|
| LoRa RSSI | REST wrapper or `observation_paths` table |
| LoRa SNR | REST wrapper or `observation_paths` table |
| gateway/hub ID | REST wrapper or `observation_paths` table |
| ingress path | REST wrapper or `observation_paths` table |
| gateway receive time | REST wrapper or `observation_paths` table |
| LTE RSSI | REST wrapper or `observation_paths` table |
| LTE SNR/SINR | REST wrapper or `observation_paths` table |
| LTE RSRP/RSRQ | REST wrapper or `observation_paths` table |
| cell ID/TAC/band | REST wrapper or `observation_paths` table |

These fields describe the link used to deliver the packet. They are measurements made by the receiving hub, the LTE modem, or the cloud ingestion path. They are not part of the collar's immutable observation.

## 15. Transport formats

### 15.1 Collar to hub over LoRa

The LoRa payload is raw binary only:

```text
[32-byte header][0-24 byte TLVs][8-byte auth_tag_8]
```

There is no JSON and no Base64 over LoRa.

The SX1262/LoRa layer may add its own radio-layer preamble, sync word, FEC, LoRa header and PHY CRC depending on radio configuration. Those are not part of this application payload.

### 15.2 Hub to Supabase over HTTPS

The hub sends a JSON wrapper because it needs to include relay metadata as well as the original binary packet.

The binary packet is Base64-encoded only because JSON is text and cannot safely carry arbitrary binary bytes directly.

Example LoRa relay wrapper:

```json
{
  "ingest_path": "lora_hub",
  "link_type": "lora",
  "gateway_guid16": "0016",
  "gateway_rx_time_unix": 1786537811,
  "link_rssi_dbm": -92,
  "link_snr_db": 6.25,
  "payload_b64": "BASE64_OF_UNMODIFIED_COLLAR_PACKET"
}
```

### 15.3 Collar direct to Supabase over LTE

The LTE path also uses a JSON wrapper with `payload_b64`. This keeps the Supabase Edge Function input shape consistent with the hub relay path.

Example LTE direct wrapper:

```json
{
  "ingest_path": "cellular_direct",
  "link_type": "lte",
  "link_rssi_dbm": -104,
  "link_snr_db": 7.0,
  "cell_rsrp_dbm": -104,
  "cell_rsrq_db": -9.5,
  "cell_sinr_db": 7.0,
  "payload_b64": "BASE64_OF_UNMODIFIED_COLLAR_PACKET"
}
```

The LTE RF fields serve the same dashboard role as LoRa RSSI/SNR, but they describe the cellular link rather than the LoRa link.

### 15.4 Why Base64 is used in wrappers

Base64 is not part of the TLV protocol. It is only a wrapper encoding.

It is used because arbitrary binary bytes are not safe to embed directly inside JSON strings. The cost is about 33 percent expansion of the encoded packet string, but that cost is acceptable on Wi-Fi/LTE HTTPS wrapper paths and avoids having separate raw-binary and JSON ingestion endpoints.

Do not use Base64 over LoRa.

## 16. Supabase ingestion model

The Supabase Edge Function should:

1. Receive JSON wrapper.
2. Read `payload_b64`.
3. Decode `payload_b64` to raw packet bytes.
4. Check total decoded packet length is between 40 and 64 bytes.
5. Read the 32-byte header.
6. Validate `tlv_len <= 24`.
7. Verify `packet_len == 32 + tlv_len + 8`.
8. Look up the device key using `device_guid16`.
9. Verify `auth_tag_8` using HMAC-SHA256 over header + TLVs.
10. Decode header fields.
11. Parse TLVs.
12. Hash the complete authenticated packet with SHA-256 and insert into
    `observations` using that payload hash as the canonical deduplication key.
13. Store wrapper/link metadata separately in `observation_paths`.
14. Treat a reused `(device_guid16, msg_seq_id, time_unix)` containing a
    different payload hash as an identity conflict rather than overwriting it.

Observation constraints:

```sql
UNIQUE (payload_hash)
UNIQUE (device_guid16, msg_seq_id, time_unix)
```

Suggested `observations` table:

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
```

Suggested `observation_paths` table:

```text
observation_paths
- device_guid16
- msg_seq_id
- ingest_path
- link_type
- gateway_guid16
- gateway_rx_time_unix
- link_rssi_dbm
- link_snr_db
- cell_rsrp_dbm
- cell_rsrq_db
- cell_sinr_db
- received_at
- payload_hash
```

This allows a single observation to have multiple arrival paths.

Example:

```text
Observation 04A7:10542
- LoRa hub path: RSSI -92 dBm, SNR 6.25 dB
- LTE direct path: RSSI -104 dBm, SINR 7.0 dB
```

## 17. Decoder procedure

```text
wrapper = parse_json(request_body)
packet = base64_decode(wrapper.payload_b64)

if packet_len < 40 or packet_len > 64: reject

header = packet[0:32]
tlv_len = header[31]

if tlv_len > 24: reject

expected_len = 32 + tlv_len + 8
if packet_len != expected_len: reject

body = packet[0 : 32 + tlv_len]
rx_tag = packet[32 + tlv_len : 32 + tlv_len + 8]

device_guid16 = read_u16_le(header[1:3])
device_key = lookup_device_key(device_guid16)

expected_tag = first_8_bytes(HMAC_SHA256(device_key, body))
if rx_tag != expected_tag: reject

parse header
parse TLVs from packet[32 : 32 + tlv_len]
payload_hash = SHA256(packet)
insert-or-find observation by payload_hash
reject a different payload reusing (device_guid16, msg_seq_id, time_unix)
insert observation path metadata
```

## 18. Example minimum packet decoded to JSON

A packet with no TLVs still carries the full minimum observation data.

```json
{
  "protocol_version": 1,
  "device_guid16": "04A7",
  "msg_seq_id": 10542,
  "message_identity": "04A7:10542:1786537810",
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

## 19. Example header byte layout

Example values:

```text
ver            = 1
device_guid16  = 0x04A7
msg_seq_id     = 10542
time_unix      = 1786537810
state          = 0x11
flags          = 0x03
tx_reason      = TELEMETRY = 0
lat_e7         = 519058165
lon_e7         = -22394678
batt_mV        = 3700
acc_m          = 12
fix_age_s      = 3
sat_count      = 8
hdr_rsvd       = 00 00 00 00
tlv_len        = 20
```

Header bytes:

```text
01
A7 04
2E 29
D2 F8 7C 6A
11
03
00
F5 32 F0 1E
CA 48 AA FE
74 0E
0C 00
03 00
08
00 00 00 00
14
```

## 20. Example selected TLV section

Example selected TLVs:

```text
04 02 03 01                TLV 0x04, len 2, fw_ver = 0x0103

10 04 80 51 01 00          TLV 0x10, len 4, uptime_s = 86400

13 01 2A                   TLV 0x13, len 1, activity_score = 42

20 02 2D 29                TLV 0x20, len 2, acked_msg_seq_id = 10541

06 01 01                   TLV 0x06, len 1, reset_reason = POWER_ON
```

TLV length:

```text
20 bytes
```

Full application packet size:

```text
32 header + 20 TLV + 8 auth tag = 60 bytes
```

## 21. Dashboard interpretation

The dashboard should not assume that RSSI/SNR always means LoRa. It should use
the authenticated wrapper `ingest_path` for the customer-facing route badge,
while the backend continues to validate the corresponding `link_type` pair.

Suggested display logic:

```text
ingest_path = lora_hub       -> show RF beside the signal quality
ingest_path = cellular_direct -> show 4G beside the signal quality
ingest_path absent            -> show a neutral unknown marker
```

For each path:

```text
LoRa: RSSI -92 dBm, SNR 6.25 dB
LTE:  RSSI -104 dBm, SINR 7.0 dB
```

If both paths deliver the same observation, the UI can show the deduped location once and show both delivery paths as diagnostics.

## 22. Current locked decisions

- Fixed header is 32 bytes.
- Header active fields use 28 bytes.
- Four header bytes remain reserved.
- `tlv_len` is the final byte of the fixed header.
- TLV budget is 0 to 24 bytes.
- Authentication tag is 8 bytes.
- CRC16 is removed.
- Device ID is 16-bit.
- Message sequence ID is 16-bit.
- No boot ID in v1.1.
- Latitude and longitude remain signed 32-bit e7 values.
- `dist_home_m` is removed from collar packet.
- `fix_age_s` and `sat_count` are in the fixed header.
- Selected collar TLVs: `fw_ver`, `reset_reason`, `uptime_s`, `activity_score`, `acked_msg_seq_id`.
- `command_id` is omitted.
- ACKs identify the received command/message using `acked_msg_seq_id`.
- `WAKE_CHECKIN` is TX reason value 7 for lightweight wake-up presence checks and command receive windows.
- LoRa RSSI/SNR and LTE RF stats are wrapper metadata, not collar TLVs.
- LoRa collar-to-hub uses raw binary.
- Hub-to-Supabase uses JSON wrapper plus `payload_b64`.
- LTE direct-to-Supabase also uses JSON wrapper plus `payload_b64`.
- Supabase canonically deduplicates identical authenticated packets by their
  backend-computed SHA-256 payload hash.
- `(device_guid16, msg_seq_id, time_unix)` is an anomaly/conflict identity so
  the 16-bit message sequence may safely wrap without overwriting observations.
