# Heltec Tracker V2 V4 Home Hub Relay

This is the first V4 Home Hub substitute firmware for the Heltec Wireless Tracker V2.

Its purpose is to prove the middle leg of the Bluepaws system:

```text
RAK4631 collar testbed -> raw TLV over LoRa -> Heltec Tracker V2 hub -> HTTPS JSON wrapper -> Supabase
```

The Heltec receives the collar TLV unchanged, validates the TLV v1.1 structure, base64-encodes the raw packet, and relays it to the Supabase `ingest-position` Edge Function as a gateway packet.

## Hardware

Board:

- Heltec Wireless Tracker V2 / HTIT-Tracker V2.3
- ESP32-S3FN8, 8 MB flash, no PSRAM
- Built-in SX1262 LoRa radio

LoRa pins are taken from the legacy BluePawzReceiver V3 project:

| Signal | GPIO |
|---|---:|
| NSS | 8 |
| SCK | 9 |
| MOSI | 10 |
| MISO | 11 |
| RST | 12 |
| BUSY | 13 |
| DIO1 | 14 |

V3 JSON and V3 radio settings are read-only reference. This V4 firmware uses the locked V4 LoRa profile in `shared/lib/BluepawsProtocol/bp_config.h`.

## Build and upload

From the BluepawsV4 repo root:

```powershell
cd "C:\Users\reesMiniPC\Documents\Codex\2026-08-07\prior-conversation-with-codex-conversation-role\work\BluepawsV4"

& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e heltec_tracker_v2_home_hub
```

Upload, using the COM port Windows assigns to the Heltec Tracker V2. Historically this was `COM7`:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e heltec_tracker_v2_home_hub -t upload --upload-port COM7
```

Monitor:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --port COM7 --baud 115200
```

## Runtime configuration

Secrets are not committed. Configure the hub over the serial monitor. The ESP32 stores these values in NVS flash.

```text
ssid YourWifiName
pass YourWifiPassword
url https://ykcdaonkvwemedotdpdr.supabase.co/functions/v1/ingest-position
token YourGatewayBearerToken
connect
show
```

Other serial commands:

```text
clear
help
```

## Local status page

The firmware starts a local Wi-Fi AP:

```text
SSID: BluePaws-Hub-V4
Pass: bluepaws4
```

Open the AP IP shown in serial, usually:

```text
http://192.168.4.1
```

The current local page is intentionally minimal. It shows relay status, received packet counts, cloud POST counts, last device/sequence, and last HTTP response code.

## Supabase wrapper

For valid TLV packets, the hub sends:

```json
{
  "format": "tlv",
  "payload_b64": "<base64 raw collar TLV>",
  "ingest_path": "lora_gateway",
  "gateway_guid16": "0016",
  "link_type": "lora",
  "link_rssi_dbm": -105,
  "link_snr_db": 7.5
}
```

`gateway_rx_time_unix` is deliberately omitted until the hub has real NTP-backed wall-clock time. Supabase still records its own receive time.

The HTTP request uses:

```text
Authorization: Bearer <gateway bearer token>
Content-Type: application/json
```

The Supabase `ingest-position` Edge Function has JWT verification disabled and performs gateway bearer-token validation inside the function.

## Current limitations

- Local AP GUI is a simple status page, not the final off-grid map UI.
- No command downlink is implemented in this minimal relay firmware yet.
- BLE Home beacon advertising is enabled with the configured V4 name, but beacon authentication/rotation is future work.
- Gateway receive time is omitted until NTP is added.
- Production gateway secrets still need a proper provisioning/onboarding flow.
