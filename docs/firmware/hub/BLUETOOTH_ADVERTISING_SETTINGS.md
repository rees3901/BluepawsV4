# Hub Bluetooth advertising settings

The Vercel Home Hub Bluetooth button and beacon icon open a settings dialog.
Apply saves advertising on/off and power together; closing discards edits.

| Preset | Advertising transmit power |
| --- | --- |
| Lowest | -12 dBm |
| Low | -6 dBm |
| Medium (default) | +3 dBm |
| High | +6 dBm |
| Highest | +9 dBm |

These are conservative supported presets, not universal chip hardware limits.
They affect BLE advertising only, not Wi-Fi, LoRa or BLE scanning. The Home
Wi-Fi/mode gate still controls whether advertising actually runs. Power can be
saved while advertising is off and used when it resumes.

`desired_ble_tx_power_dbm` is the saved preference. `ble_tx_power_dbm` is the
power accepted/read back by the controller. The backend leaves the latter null
for old firmware. The GUI disables power control until a supported power is
reported; advertising on/off remains available. `ble_power_steps=5` enables all
five presets. Older three-level firmware keeps Low/High disabled; omitted capability
with a non-null power report means three levels, never five. Confirmation requires the
settings revision, enabled preference and power to match the request.

Arduino ESP32 hub firmware persists the preference in its existing NVS namespace
and applies it in the BLE task. The personal T190 uses this same implementation.
The separate ESP-IDF P4/S3 hubs do not implement adjustable power in this change
and must keep the power selector disabled.

Rollout: review the migration and existing Family-scoped RLS; apply the migration,
then deploy ingest-position, update the selected hub firmware, and release the
web application. Release of these operations was authorized on 7 October 2026.
Older firmware reports remain accepted. Roll back the web/firmware independently;
retain the additive columns/RPC until no new sender depends on them.

Vendor references:
- https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-reference/bluetooth/controller_vhci.html
- https://raw.githubusercontent.com/espressif/esp-idf/v4.4.7/components/bt/include/esp32c3/include/esp_bt.h
