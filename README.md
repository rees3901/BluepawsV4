# Bluepaws V4

Unified firmware repository for the Bluepaws cat tracker system. Both the transmitting collar and receiving home hub share a single codebase with common protocol and configuration libraries.

## System Overview

| Component | MCU | Radio | Connectivity |
|-----------|-----|-------|-------------|
| **Collar** | nRF52840 | SX1262 LoRa | NB-IoT (BG77), GNSS (L76K), BLE |
| **Home Hub** | ESP32-S3 | SX1262 LoRa | WiFi, BLE beacon |

**Data paths:**
- Collar → LoRa → Home Hub → WiFi → Cloud (Supabase)
- Collar → NB-IoT → REST POST → Cloud (1:10 ratio with LoRa)

**Battery target:** 30+ days at 10-minute wake intervals.

## Repository Structure

```
BluepawsV4/
├── platformio.ini                    # Multi-environment build config
├── shared/lib/BluepawsProtocol/      # Shared protocol & config
│   ├── bp_protocol.h                 # TLV binary protocol v2
│   └── bp_config.h                   # LoRa params, profiles, timing
├── collar/                           # nRF52840 collar firmware
│   ├── src/main.cpp
│   └── include/collar_pins.h
├── hub/                              # ESP32-S3 home hub firmware
│   ├── src/main.cpp
│   ├── include/hub_pins.h
│   └── data/                         # LittleFS web GUI (HTML/CSS/JS)
└── README.md
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
