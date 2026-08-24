# Bluepaws V4 Home Hub Runtime Decisions

Date: 2026-08-24

## Naming

Use **Home Hub** as the standard term.

Older or informal terms including **LoRa hub**, **base station**, **receiving base station**, **Home Hub LoRa relay**, and **LoRa relay** all refer to the same device in this project: the always-on unit that receives raw collar TLV packets over LoRa and relays them to Supabase when cloud connectivity is available.

## Role

The Home Hub is an always-on ESP32-S3 + SX1262 gateway. It does not sleep during normal operation.

Its primary duties are:

1. Keep LoRa receive active.
2. Receive raw binary TLV v1.1 packets from collars.
3. Parse TLV locally for hub state, local diagnostics, and future off-grid UI.
4. Preserve the original collar TLV payload unchanged.
5. Base64-wrap that unchanged payload in an HTTPS JSON envelope for Supabase.
6. Advertise the BLE Home beacon in Home mode.
7. Support future local/off-grid tracking and collar command delivery.

No JSON is sent over the LoRa radio path.

## Communications profiles

The hub profile is explicit user state. It must not change automatically just because Wi-Fi or cloud access temporarily fails.

| Profile | Meaning | Wi-Fi/AP behaviour | Cloud relay |
|---|---|---|---|
| `HOME` | Normal fixed home installation. | Connect to configured home Wi-Fi. Hub AP off unless provisioning is enabled. | Allowed when Wi-Fi/cloud are healthy. |
| `PORTABLE` | User deliberately takes the hub roaming, usually with a phone hotspot or other temporary Wi-Fi. | Connect to configured/selected uplink. AP is not the normal primary interface. | Best effort when uplink is healthy. |
| `OFF_GRID` | Local-only search mode with no assumed internet. | Hub AP on and local UI becomes primary. | Disabled by design. |

Provisioning mode is separate from the profile. It may temporarily enable the hub AP in Home mode for first-time setup or recovery.

## FreeRTOS architecture

The firmware uses separate tasks so cloud or UI activity cannot block LoRa reception:

- `loraTask`: SX1262 receive/transmit and command retry checks.
- `cloudTask`: consumes a relay queue and POSTs accepted LoRa packets to Supabase.
- `webTask`: serves local hub UI/API and status endpoints.
- `bleTask`: advertises the Home beacon in Home mode or scans for Lost Alert BLE find beacons in Portable/Off-Grid modes.

LoRa RX should remain active regardless of Wi-Fi, cloud, or local UI state.

## Supabase relay wrapper

The Home Hub sends the unchanged collar TLV payload to Supabase as:

```json
{
  "format": "tlv",
  "payload_b64": "<base64 raw collar TLV>",
  "ingest_path": "lora_gateway",
  "gateway_guid16": "0016",
  "gateway_rx_time_unix": 1786537811,
  "link_type": "lora",
  "link_rssi_dbm": -97,
  "link_snr_db": 8.2
}
```

The current Edge Function accepts `lora_gateway` and normalizes it to the backend storage enum `lora_hub`.

`gateway_rx_time_unix` should use the hub's NTP-synced clock when available. During early boot, before NTP is ready, the hub may temporarily fall back to the collar packet timestamp so the wrapper remains well-formed.

## Current prototype target

The current Home Hub prototype target is:

- Heltec Wireless Tracker V2 / HTIT-Tracker V2.x
- ESP32-S3
- SX1262 LoRa
- USB-C serial upload/monitor
- Local V3-derived LoRa pin map: NSS 8, SCK 9, MOSI 10, MISO 11, RST 12, BUSY 13, DIO1 14

The legacy BluePawz V3 receiver repo remains read-only reference material for board bring-up/pinout only. Bluepaws V4 uses the TLV v1.1 protocol and V4 LoRa profile.
