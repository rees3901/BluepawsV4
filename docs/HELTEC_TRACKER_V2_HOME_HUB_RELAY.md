# Heltec Tracker V2 V4 Home Hub Relay

This is the first V4 Home Hub substitute firmware for the Heltec Wireless Tracker V2.

Its purpose is to prove the middle leg of the Bluepaws system:

```text
RAK4631 collar testbed -> raw TLV over LoRa -> Heltec Tracker V2 hub -> HTTPS JSON wrapper -> Supabase
```

The Heltec receives the collar TLV unchanged, validates the TLV v1.1 structure, base64-encodes the raw packet, and relays it to the Supabase `ingest-position` Edge Function as a gateway packet.

The hub is an always-on FreeRTOS application. There is no hub sleep state: LoRa receive, Wi-Fi management, cloud relay, serial/profile control, and the local web status page run as separate tasks.

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

For the current bench setup, the default STA Wi-Fi network is:

```text
SSID: Reesnet Guest
Password: <blank/open network>
```

You can still override it over serial:

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
profile home
profile portable
profile offgrid
home
portable
offgrid
provision on
provision off
clear
help
```

## Hub communication profiles

The relay has three high-level profiles. These are intentionally coarse for now; they give us the skeleton needed for the later full Home Hub behaviour.

| Profile | LoRa RX | Local AP/web | STA Wi-Fi/cloud relay | BLE Home beacon |
|---|---:|---:|---:|---:|
| Home | On | Off unless provisioning is needed/enabled | On when configured/connected | On |
| Portable | On | Off unless provisioning is needed/enabled | On when configured/connected | Off |
| Off-grid | On | On | Off | Off |

Rationale:

- `Home` means the hub is acting as the fixed home base. It advertises the BLE Home beacon so collars can decide they are safely at home.
- `Portable` means the hub can travel with the user and can relay through a hotspot or router, but it must not impersonate the fixed home BLE beacon.
- `Off-grid` means local-only search/diagnostic operation. It keeps receiving LoRa and serving its AP status page, but deliberately does not relay to Supabase.
- `provision on` deliberately exposes the hub AP for first-time setup, Wi-Fi changes, or recovery. A fresh/unconfigured hub also exposes the AP automatically until Wi-Fi/cloud credentials are present.
- `provision off` returns Home/Portable mode to the quieter customer-facing behaviour where the hub joins an external Wi-Fi network without broadcasting its own setup AP.

## Local status page

When provisioning or off-grid mode is active, the firmware starts a local Wi-Fi AP:

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
  "gateway_rx_time_unix": 1787486400,
  "link_type": "lora",
  "link_rssi_dbm": -105,
  "link_snr_db": 7.5
}
```

Until the hub has NTP-backed wall-clock time, `gateway_rx_time_unix` uses the collar TLV packet timestamp as a valid bench-time placeholder. Supabase still records its own authoritative receive time.

The HTTP request uses:

```text
Authorization: Bearer <gateway bearer token>
Content-Type: application/json
```

The Supabase `ingest-position` Edge Function has JWT verification disabled and performs gateway bearer-token validation inside the function.

## Current limitations

- Local AP GUI is a simple status page, not the final off-grid map UI.
- No command downlink is implemented in this minimal relay firmware yet.
- BLE Home beacon advertising is profile-controlled; authentication/rotation is future work.
- Gateway receive time is omitted until NTP is added.
- Production gateway secrets still need a proper provisioning/onboarding flow.
