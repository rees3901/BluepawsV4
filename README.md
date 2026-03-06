# Bluepaws V4

Unified firmware repository for the Bluepaws animal tracker system. Both the transmitting collar and receiving home hub share a single codebase with a common TLV binary protocol and configuration library.

## System Overview

| Component | MCU | Radio | Connectivity |
|-----------|-----|-------|-------------|
| **Collar** | nRF52840 | SX1262 LoRa | Sequans Monarch 2 GM02SP (LTE-M/NB-IoT + GNSS), BLE |
| **Home Hub** | ESP32-S3 | SX1262 LoRa | WiFi, BLE beacon |

**Data paths:**
- Collar → LoRa → Home Hub → WiFi → Cloud (Supabase)
- Collar → LTE-M/NB-IoT → REST POST → Cloud (1:10 ratio with LoRa)

**Battery target:** 30+ days at 10-minute wake intervals.

## Hardware

| Part | Role | Key Specs |
|------|------|-----------|
| **nRF52840** (Seeed XIAO BLE Sense) | Collar MCU | ARM Cortex-M4F, BLE 5.0, 256KB RAM |
| **SX1262** | LoRa transceiver | 150 MHz–960 MHz, +22 dBm, LoRa/FSK |
| **[Sequans Monarch 2 GM02SP](https://sequans.com/products/monarch-2-gm02s/)** | Cellular + GNSS | LTE Cat M1/NB-IoT, integrated GNSS, 1µA deep sleep, 23 dBm TX, EAL5+ secure enclave, iSIM, single 2.2V rail, global bands |
| **ESP32-S3** (Seeed XIAO) | Hub MCU | Dual-core, WiFi, BLE 5.0, 512KB SRAM |

The Sequans GM02SP replaces the previous BG77 + L76K combination — a single module handles both cellular IoT and GPS positioning, simplifying the collar BOM and reducing power draw.

## Repository Structure

```
BluepawsV4/
├── platformio.ini                    # Multi-environment build config
├── shared/lib/BluepawsProtocol/      # Shared protocol & config
│   ├── library.json                  # PlatformIO library manifest
│   ├── bp_protocol.h                 # TLV binary protocol v2
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
│   └── mock-server.js                # Node.js mock hub for local GUI dev
└── mock_server.py                    # Python mock server (legacy)
```

## Web GUI

The hub serves a real-time tracking dashboard over WiFi, built with Leaflet.js and Server-Sent Events (SSE).

**Device Cards** — each tracked animal gets a card in the left sidebar showing:
- Unique emoji avatar with colour-coded ring (auto-assigned per device)
- Last GPS coordinates in monospace
- Status badge (Home / Out / Lost)
- Telemetry grid: profile, battery %, signal strength, GPS accuracy, fix age, last seen
- **Jump** — centres map on the animal at zoom 17
- **Follow** — auto-pans the map as new positions arrive (green when active)
- **Trail** — toggles breadcrumb polyline on/off per device (amber when active)
- **Cmd** — opens command modal (change mode: Normal / Active / PowerSave / Lost)

**Map Features:**
- Three base layers: Street (OSM), Satellite (Esri), Topographic
- Per-device coloured trail lines with dashed polylines
- Lost-mode markers pulse red
- Measurement tool (click to measure distances)
- Dark / light theme toggle (persisted to localStorage)

**Connection Monitoring:**
- Server sends SSE heartbeat every 5 seconds
- Client watchdog flips to "No heartbeat" if 10 seconds pass without any event
- Status banner shows Connected (green) or Disconnected (red, pulsing)

## Mock Server

Simulates 5 animals with live SSE telemetry for local GUI development:

```bash
node tools/mock-server.js
# → http://localhost:3000
# Streams position updates every 2s + heartbeat every 5s
```

## TLV Protocol v2

64-byte maximum packet: 29-byte fixed header + up to 33 bytes TLV + 2-byte CRC-16.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | Protocol version (0x02) |
| 1-2 | 2 | Device ID |
| 3-6 | 4 | Message sequence |
| 7-10 | 4 | Unix timestamp |
| 11 | 1 | Status |
| 12-13 | 2 | Flags (pkt type + feature bits) |
| 14-17 | 4 | Latitude x10^7 |
| 18-21 | 4 | Longitude x10^7 |
| 22-23 | 2 | Battery (mV) |
| 24-25 | 2 | GPS accuracy (m) |
| 26-27 | 2 | Fix age (s) |
| 28 | 1 | TLV payload length |

AES-128 encryption via RadioLib. CRC-16/CCITT-FALSE integrity check.

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

| Profile | TX Power | Interval | Use Case |
|---------|----------|----------|----------|
| Normal | 19 dBm | 10 min | Daily tracking |
| Powersave | 10 dBm | 20 min | At home / battery conservation |
| Active | 19 dBm | 1 min | Active monitoring |
| Lost | 22 dBm | 30 s | Emergency search (2h max) |
