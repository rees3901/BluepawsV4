# Bluepaws Cat Tracker TLV Telemetry Protocol v1.2

Status: locked packet-header, addressing, TLV set and authentication decisions
Last updated: 2026-08-26
Scope: collar telemetry, hub commands/ACKs, LoRa collar-to-hub path, LTE-M direct path, hub relay path, Supabase ingestion

## 1. Purpose

Bluepaws V4 uses a compact authenticated binary packet for collar and hub traffic.

The application packet answers:

```text
Who originated this message?
Who is the logical recipient?
What message is this?
What did the sender report or request?
```

Transport metadata remains separate and answers how the packet reached the cloud and what the link quality was.

## 2. Locked packet size

```text
[32-byte fixed header][0-24 byte TLV section][8-byte auth_tag_8]
```

```text
minimum = 32 + 0  + 8 = 40 bytes
maximum = 32 + 24 + 8 = 64 bytes
```

There is no application CRC16. Radio-layer PHY CRC may still be enabled independently.

## 3. Authentication

```text
auth_tag_8 = first 8 bytes of HMAC-SHA256(device_key, header + TLVs)
```

The HMAC covers the complete 32-byte header, including `source_id16`, `destination_id16`, the two remaining reserved bytes, and every actual TLV byte. It does not cover itself.

The HMAC therefore authenticates both sender identity and intended recipient. An intermediary cannot retarget an authenticated packet without invalidating the tag.

## 4. Byte order

All multi-byte fields are little-endian.

| Type | Meaning |
|---|---|
| `u8` | unsigned 8-bit integer |
| `u16` | unsigned 16-bit integer |
| `u32` | unsigned 32-bit integer |
| `i32` | signed 32-bit two's-complement integer |
| `u8[n]` | fixed byte array |

## 5. Fixed header layout, 32 bytes

`tlv_len` remains the final header byte at offset 31.

| Offset | Size | Field | Type | Description |
|---:|---:|---|---|---|
| 0 | 1 | `ver` | `u8` | On-wire layout version. Use `2` for v1.2. |
| 1 | 2 | `source_id16` | `u16` | Physical/logical identity that originated the packet. |
| 3 | 2 | `destination_id16` | `u16` | Logical intended recipient. |
| 5 | 2 | `msg_seq_id` | `u16` | Per-source message sequence. |
| 7 | 4 | `time_unix` | `u32` | UTC packet/fix timestamp in seconds. |
| 11 | 1 | `state` | `u8` | Lower nibble status, upper nibble power profile. |
| 12 | 1 | `flags` | `u8` | Core boolean flags. |
| 13 | 1 | `tx_reason` | `u8` | Reason/class of transmission. |
| 14 | 4 | `lat_e7` | `i32` | Latitude x 10,000,000. |
| 18 | 4 | `lon_e7` | `i32` | Longitude x 10,000,000. |
| 22 | 2 | `batt_mV` | `u16` | Battery voltage in millivolts. |
| 24 | 2 | `acc_m` | `u16` | Horizontal GNSS accuracy in metres. |
| 26 | 2 | `fix_age_s` | `u16` | GNSS fix age. `65535` means unknown/not applicable. |
| 28 | 1 | `sat_count` | `u8` | Satellite count. `255` means unknown. |
| 29 | 2 | `hdr_rsvd` | `u8[2]` | Reserved. Must be `00 00`. |
| 31 | 1 | `tlv_len` | `u8` | TLV byte count, `0..24`. |

Header size:

```text
1+2+2+2+4+1+1+1+4+4+2+2+2+1+2+1 = 32 bytes
```

The destination field consumes two of the four bytes reserved in v1.1. Two reserved bytes remain. Packet size and TLV budget do not change.

## 6. On-wire versioning

The document revision is v1.2, but the on-wire `ver` byte is `2`.

This is required because inserting `destination_id16` shifts `msg_seq_id` and all subsequent fields by two bytes. A v1.1 decoder would otherwise silently misinterpret the packet.

Receivers must reject or explicitly route unsupported versions to the correct legacy decoder.

## 7. Source identity

`source_id16` is the canonical protocol name.

For an existing collar:

```text
v1.1 device_guid16 == v1.2 source_id16
```

No second identity is provisioned. The numeric value remains unchanged.

Backend/storage fields called `device_id`, `device_uid`, or historical `device_guid16` may remain temporarily where renaming them would create unnecessary migration risk. They are storage aliases for the same identity. New packet builders, decoders, logs and protocol documentation should use `source_id16`.

## 8. Destination identity

`destination_id16` identifies the logical end recipient, not necessarily the immediate radio/network next hop.

This distinction preserves multipath packet identity.

A cloud-bound observation can travel either path:

```text
Collar -> LTE -> Cloud
Collar -> LoRa -> Hub -> HTTPS -> Cloud
```

In both cases the logical destination is the cloud/backend. The hub relays the authenticated inner packet unchanged and must not rewrite `destination_id16`.

For local/off-grid traffic, commands and ACKs, the destination can be a specific hub or collar.

## 9. Reserved destination values

```text
0x0000 = CLOUD / backend logical destination
0xFFFF = BROADCAST
```

Neither value may be provisioned as the physical source identity of a collar or hub.

## 10. Physical ID role architecture

Physical IDs use a simple numeric role rule:

```text
Hub ID    = non-zero, non-broadcast ID divisible by 16
Collar ID = non-zero, non-broadcast ID not divisible by 16
```

Equivalent test:

```text
is_hub = id != 0x0000 && id != 0xFFFF && (id & 0x000F) == 0
```

Examples:

```text
16 decimal = 0x0010 = HUB
32 decimal = 0x0020 = HUB
48 decimal = 0x0030 = HUB

14 decimal = 0x000E = COLLAR
15 decimal = 0x000F = COLLAR
17 decimal = 0x0011 = COLLAR
30 decimal = 0x001E = COLLAR
31 decimal = 0x001F = COLLAR
33 decimal = 0x0021 = COLLAR
```

Important: this is only a role classification. It does not mean a hub owns the surrounding 15 IDs and it does not impose a 15-collar capacity limit.

A hub can manage 20, 30, 40 or more collars. Provisioning simply skips numeric IDs reserved for hubs.

Namespace capacity after reserving `0x0000` and `0xFFFF`:

```text
4095 hub IDs
61439 collar IDs
```

## 11. Message sequence and deduplication

`msg_seq_id` remains a 16-bit per-source sequence.

For a collar observation sent over multiple paths, the same logical observation uses the same sequence number.

Canonical identity semantics:

```text
(source_id16, msg_seq_id)
```

A backend may additionally use timestamp/payload hash to detect sequence reuse or identity conflicts.

## 12. ACK and command semantics

Source/destination addressing does not replace message correlation.

Example command:

```text
source_id16      = 0x0010   hub 16
destination_id16 = 0x04A7   collar
msg_seq_id       = 4711
tx_reason        = CONFIG
```

Example ACK:

```text
source_id16      = 0x04A7   collar
destination_id16 = 0x0010   hub 16
msg_seq_id       = 10542
tx_reason        = ACK
TLV 0x20         = acked_msg_seq_id = 4711
```

The addresses identify the participants. `acked_msg_seq_id` identifies the exact message being acknowledged.

## 13. State, flags and TX reason

`state` remains packed as:

```text
bits 0-3 = status
bits 4-7 = power_profile
state = (power_profile << 4) | status
```

Status values:

| Value | Name |
|---:|---|
| 0 | `HOME` |
| 1 | `OUT` |
| 2 | `LOST` |
| 3 | `ERROR` |
| 4-15 | reserved |

Power profiles:

| Value | Name |
|---:|---|
| 0 | `POWER_SAVE` |
| 1 | `NORMAL` |
| 2 | `ACTIVE` |
| 3 | `LOST_ALERT` |
| 4 | `DEBUG` where firmware permits |
| 5-15 | reserved |

Flags remain:

| Bit | Mask | Name |
|---:|---:|---|
| 0 | `0x01` | `GNSS_VALID` |
| 1 | `0x02` | `FIX_3D` |
| 2 | `0x04` | `LOW_BATTERY` |
| 3 | `0x08` | `HOME_BEACON_SEEN` |
| 4 | `0x10` | `GEOFENCE_BREACHED` |
| 5 | `0x20` | `CHARGING` |
| 6 | `0x40` | `STALE_FIX` |
| 7 | `0x80` | `ERROR_PRESENT` |

TX reasons remain:

| Value | Name |
|---:|---|
| 0 | `TELEMETRY` |
| 1 | `ACK` |
| 2 | `PING` |
| 3 | `INTERRUPT` |
| 4 | `BOOT` |
| 5 | `ALERT` |
| 6 | `CONFIG` |
| 7 | `WAKE_CHECKIN` |

## 14. TLV section

TLV encoding remains:

```text
[type:u8][len:u8][value:len bytes]
```

Rules:

1. `tlv_len` is at offset 31.
2. Valid range is `0..24`.
3. Unknown TLVs are skipped by their length.
4. Bounds-check every TLV.
5. Do not pad.
6. Append HMAC immediately after final TLV byte.

Selected TLVs remain:

| Type | Name | Value bytes | Total cost |
|---:|---|---:|---:|
| `0x04` | `fw_ver` | 2 | 4 |
| `0x06` | `reset_reason` | 1 | 3 |
| `0x10` | `uptime_s` | 4 | 6 |
| `0x13` | `activity_score` | 1 | 3 |
| `0x20` | `acked_msg_seq_id` | 2 | 4 |
| `0xF1` | `profile` | 1 | 3 |

The five telemetry/diagnostic TLVs through `0x20` cost 20 bytes together,
leaving 4 bytes of telemetry headroom. `profile` is a command/ACK TLV and is
not normally combined with that complete telemetry set.

### 14.1 Power-profile command

A Home Hub profile command uses:

```text
source_id16      = provisioned hub ID (multiple of 16)
destination_id16 = target collar ID
tx_reason        = CONFIG
TLV 0xF1         = requested power profile u8
```

The collar acknowledgement uses:

```text
source_id16      = collar ID
destination_id16 = originating hub ID
tx_reason        = ACK
TLV 0x20         = acknowledged command msg_seq_id
TLV 0xF1         = resulting power profile u8 (where included)
```

The 1.2 addresses identify the participants; TLV `0x20` correlates the exact
command. Prototype firmware currently performs structural routing and ACK
testing without verifying downlink authentication. This remains explicitly
non-production until a hub-to-collar key/proof model is locked and implemented.

## 15. Transport separation

Transport-specific metadata remains outside the authenticated application packet.

Examples include LoRa RSSI/SNR, LTE RSRP/RSRQ/SINR, ingress path, gateway receive time, cell ID and band.

The wrapper may still contain a gateway identifier to describe which hub relayed the packet. That transport gateway identity is not a replacement for `destination_id16`.

Example LoRa relay wrapper:

```json
{
  "ingest_path": "lora_hub",
  "link_type": "lora",
  "gateway_guid16": "0010",
  "gateway_rx_time_unix": 1786537811,
  "link_rssi_dbm": -92,
  "link_snr_db": 6.25,
  "payload_b64": "BASE64_OF_UNMODIFIED_PACKET"
}
```

## 16. Decoder procedure

```text
packet = base64_decode(payload_b64)
if packet_len < 40 or packet_len > 64: reject

header = packet[0:32]
if header[0] != 2: reject or route to legacy decoder

tlv_len = header[31]
if tlv_len > 24: reject
if packet_len != 32 + tlv_len + 8: reject

source_id16      = read_u16_le(header[1:3])
destination_id16 = read_u16_le(header[3:5])
msg_seq_id       = read_u16_le(header[5:7])

if source_id16 in {0x0000, 0xFFFF}: reject
if header[29:31] != 00 00: reject

body = packet[0:32+tlv_len]
rx_tag = packet[32+tlv_len:32+tlv_len+8]
key = lookup_device_key(source_id16)
expected = first_8_bytes(HMAC-SHA256(key, body))
if rx_tag != expected: reject

parse header and TLVs
apply source/destination routing policy
store observation and transport path
```

## 17. Example v1.2 header

```text
ver              = 2
source_id16      = 0x04A7
destination_id16 = 0x0000
msg_seq_id       = 10542
time_unix        = 1786537810
state            = 0x11
flags            = 0x03
tx_reason        = 0
lat_e7           = 519058165
lon_e7           = -22394678
batt_mV          = 3700
acc_m            = 12
fix_age_s        = 3
sat_count        = 8
hdr_rsvd         = 00 00
tlv_len          = 20
```

Bytes:

```text
02
A7 04
00 00
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
00 00
14
```

## 18. v1.1 to v1.2 migration

```text
v1.1 offset 1  device_guid16      -> v1.2 offset 1 source_id16
v1.2 offset 3  destination_id16   -> new 2-byte field
v1.1 msg_seq_id and later fields  -> shifted by +2 bytes
v1.1 hdr_rsvd[4]                  -> v1.2 hdr_rsvd[2]
header size                        -> still 32 bytes
TLV capacity                       -> still 24 bytes
HMAC tag                           -> still 8 bytes
maximum packet                     -> still 64 bytes
```

Existing v1.1 packets are not byte-layout compatible with v1.2. The version byte prevents silent mis-decoding.

## 19. Locked v1.2 summary

```text
Fixed header              32 bytes
TLV area                  0..24 bytes
HMAC tag                  8 bytes truncated HMAC-SHA256
Minimum packet            40 bytes
Maximum packet            64 bytes
source_id16               2 bytes
destination_id16          2 bytes
remaining header reserve  2 bytes
cloud destination         0x0000
broadcast destination     0xFFFF
hub rule                  physical ID % 16 == 0
on-wire version           2
```
