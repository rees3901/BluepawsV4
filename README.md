# Bluepaws V4

Unified firmware repository for the Bluepaws animal tracker system. Both the transmitting collar and receiving home hub share a single codebase with a common TLV binary protocol and configuration library.

## System Overview

| Component | MCU | Radio | Connectivity |
|-----------|-----|-------|-------------|
| **Collar** | nRF52840 | SX1262 LoRa | Sequans Monarch 2 GM02SP (LTE-M/NB-IoT + GNSS), BLE |
| **Home Hub** | ESP32-S3 | SX1262 LoRa | WiFi, BLE beacon |

**Data paths:**
- Collar -> LoRa -> Home Hub -> log + display -> WiFi -> Cloud
- Collar -> LTE-M/NB-IoT -> Cloud via REST POST

**Battery target:** 30+ days at 10-minute wake intervals.

## Hardware

| Part | Role | Key Specs |
|------|------|-----------|
| **nRF52840** (Seeed XIAO BLE Sense) | Collar MCU | ARM Cortex-M4F, BLE 5.0, 256KB RAM |
| **SX1262** | LoRa transceiver | 150 MHz-960 MHz, +22 dBm, LoRa/FSK |
| **[Sequans Monarch 2 GM02SP](https://sequans.com/products/monarch-2-gm02s/)** | Cellular + GNSS | LTE Cat M1/NB-IoT, integrated GNSS, 1uA deep sleep, 23 dBm TX, EAL5+ secure enclave, iSIM, single 2.2V rail, global bands |
| **ESP32-S3** (Seeed XIAO) | Hub MCU | Dual-core, WiFi, BLE 5.0, 512KB SRAM |

The Sequans GM02SP replaces the previous BG77 + L76K combination. A single module handles both cellular IoT and GPS positioning, simplifying the collar BOM and reducing power draw.

## Repository Structure

```text
BluepawsV4/
├── README.md                         # Repository overview
├── docs/
│   └── TLV_PROTOCOL_V1_1.md           # Canonical TLV packet specification
├── platformio.ini                    # Multi-environment build config
├── shared/lib/BluepawsProtocol/      # Shared protocol & config
│   ├── README.md                     # Protocol implementation notes
│   ├── library.json                  # PlatformIO library manifest
│   ├── bp_protocol.h                 # Shared protocol implementation header
│   └── bp_config.h                   # LoRa params, profiles, timing
├── collar/                           # nRF52840 collar firmware
│   ├── src/main.cpp
│   └── include/collar_pins.h
├── hub/                              # ESP32-S3 home hub firmware
│   ├── src/main.cpp
│   ├── include/hub_pins.h
│   └── data/                         # LittleFS web GUI (HTML/CSS/JS)
│       ├── index.html
│       ├── style.css
│       └── app.js
├── tools/
│   ├── mock-server.js                # Node.js mock hub for local GUI dev
│   └── vps_position_simulator.py     # External HTTPS multi-collar simulator
├── web/                              # Next.js + TypeScript customer dashboard
│   ├── src/app/                      # App Router entry point
│   ├── src/components/               # React dashboard and Leaflet map
│   ├── src/data/                     # Typed development telemetry
│   └── src/lib/telemetry.ts          # Future Supabase adapter boundary
└── mock_server.py                    # Python mock server (legacy)
```

## Web GUI

The hub serves a real-time tracking dashboard over WiFi, built with Leaflet.js and Server-Sent Events (SSE).

**Device Cards** - each tracked animal gets a card in the left sidebar showing:
- Unique emoji avatar with colour-coded ring (auto-assigned per device)
- Last GPS coordinates in monospace
- Status badge (Home / Out / Lost / Error)
- Telemetry grid: profile, battery %, signal strength, GPS accuracy, distance from home, last seen
- **Jump** - centres map on the animal at zoom 17
- **Follow** - auto-pans the map as new positions arrive
- **Trail** - toggles breadcrumb polyline on/off per device
- **Cmd** - opens command modal

**Map Features:**
- Three base layers: Street (OSM), Satellite (Esri), Topographic
- Per-device coloured trail lines with dashed polylines
- Lost-mode markers pulse red
- Measurement tool
- Dark / light theme toggle

**Connection Monitoring:**
- Server sends SSE heartbeat every 5 seconds
- Client watchdog flips to "No heartbeat" if 10 seconds pass without any event
- Status banner shows Connected or Disconnected

## Mock Server

Simulates animals with live SSE telemetry for local GUI development:

```bash
node tools/mock-server.js
# http://localhost:3000
# Streams position updates every 2s + heartbeat every 5s
```

## Customer Web App

`web/` contains the Vercel-ready Next.js and TypeScript refactor of the hub dashboard. It preserves the embedded GUI in `hub/data/`, reads the latest live positions from Supabase by default, and confines mock telemetry to tutorial mode.

[![Deploy with Vercel](https://vercel.com/button)](https://vercel.com/new/clone?repository-url=https%3A%2F%2Fgithub.com%2Frees3901%2FBluepawsV4&root-directory=web)

```bash
cd web
npm install
npm run dev
```

For Vercel, import this repository and select `web` as the project **Root Directory**. Leave the Install, Build, and Output settings at their detected Next.js defaults. See `web/README.md` for the exact deployment settings and the planned HTTPS Edge Function -> Supabase -> Realtime data path.

The cloud ingestion schema, device registry, and authenticated Edge Function are under `supabase/`. See `tools/VPS_SIMULATOR.md` for the Ubuntu VPS test client and versioned request contract.

## TLV Protocol v1.1

The canonical protocol document is:

```text
docs/TLV_PROTOCOL_V1_1.md
```

The system is moving away from production JSON telemetry. JSON remains useful for debugging, logs, exports and admin APIs, but LoRa and cellular telemetry should use the compact binary TLV packet.

Current packet shape:

```text
[32-byte fixed header][0-30 bytes TLV][2-byte CRC16]
```

Packet size:

```text
minimum = 34 bytes
maximum = 64 bytes
```

### Fixed header summary

| Offset | Size | Field | Type | Notes |
|---:|---:|---|---|---|
| 0 | 1 | `ver` | u8 | Protocol version |
| 1 | 4 | `device_guid32` | u32 | Immutable collar identity |
| 5 | 2 | `msg_seq_id` | u16 | Per-device sequence |
| 7 | 4 | `time_unix` | u32 | UTC timestamp |
| 11 | 1 | `state` | u8 | Packed status + power profile |
| 12 | 1 | `flags` | u8 | Core bitfield |
| 13 | 4 | `lat_e7` | i32 | Latitude x 10000000 |
| 17 | 4 | `lon_e7` | i32 | Longitude x 10000000 |
| 21 | 2 | `batt_mV` | u16 | Battery voltage |
| 23 | 2 | `acc_m` | u16 | Accuracy in metres |
| 25 | 2 | `dist_home_m` | u16 | Distance from home |
| 27 | 1 | `tx_reason` | u8 | 8-value reason enum |
| 28 | 1 | `tlv_len` | u8 | 0-30 TLV bytes |
| 29 | 3 | `hdr_rsvd` | u8[3] | Reserved, set 0 |

Key changes from the older v2 notes:

- `device_guid32` replaces the previous 16-bit device ID.
- `msg_seq_id` is now 16-bit to save bytes.
- No boot ID is included yet.
- `state` packs `status` and `power_profile` into one byte.
- `flags` is now one byte, not two.
- Absolute latitude and longitude remain i32 e7 values.
- `dist_home_m` is now in the header.
- `sat_count`, `hdop_x10`, `course_deg` and `fix_age_s` are optional TLVs.
- RESTful HTTPS POST is the v1 cloud transport.

CRC-16/CCITT-FALSE is appended at the end of the packet and calculated over the header plus TLVs.

## Message Flow

```text
Collar --LoRa--> Hub --REST POST--> Cloud
Collar --LTE-M/Cat-M1 REST POST--> Cloud
```

Both paths preserve the original `(device_guid32, msg_seq_id)` from the TLV header. The hub or puck should relay received packets upstream without changing the collar observation. Redundancy is intentional for reliability.

Deduplication is handled in the cloud using:

```text
(device_guid32, msg_seq_id)
```

Example dedup key:

```text
device_guid32 = 0x00A7F134
msg_seq_id    = 10542
key           = 00A7F134:10542
```

Hub CSV or debug log format should be derived from decoded TLV fields, not treated as the production on-air format.

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
# Build both targets
pio run

# Build collar only
pio run -e collar

# Build hub only
pio run -e hub

# Upload to connected device
pio run -e collar -t upload
pio run -e hub -t upload
```

## Operating Profiles

| Profile | TX Power | Interval | Cellular | Remarks |
|---------|----------|----------|----------|---------|
| **Normal** | 19 dBm | 10 min | 1:10 | Daily tracking, balanced power and update frequency |
| **PowerSave** | 10 dBm | 30 min | 1:30 | At home or battery conservation, reduced TX power and longer sleep |
| **Active Find** | 19 dBm | 1 min | 1:5 | Cat is outside geofence or user is actively searching |
| **Emergency Lost** | 22 dBm | 30 s | 1:3 | User needs frequent updates for retrieval, then fallback to Active Find |
