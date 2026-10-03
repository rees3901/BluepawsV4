# Personal Heltec Wireless Tracker V2 hub

This standalone build compiles `../platformio/src` and packages
`../platformio/data` unchanged from canonical main. There is no OLED/native UI.
The only additions are a pinned build configuration and local private overrides.

Run from the repository root:

```powershell
pio run -d hub/personal-heltec
pio run -d hub/personal-heltec -t buildfs
```

`collar/personal-esp32s3/provision.py` prepares `.secrets/hub_secrets.h` with the
verified live endpoint, distinct gateway credential, hub ID 0030 (48 decimal),
and the open `Reesnet Guest` Wi-Fi SSID. No existing hub registration is replaced.
The build fails without that header. All existing Home/Portable/Off-Grid browser,
BLE, relay and command behaviour comes from canonical firmware.

Before flashing, back up the physical hub's complete 8 MB flash and save SHA256.
The initial personal hub is COM17, ESP32-S3 MAC `44:1b:f6:f8:ec:bc`.
Monitor at 115200. Flash both firmware and filesystem. Old saved settings can
override compiled credentials, so use the fresh bundled filesystem after backup.
This deliberately replaces the old local settings/cache on this personal hub;
the backup preserves them for rollback. No server-side historical data is erased.
When assigning a new hub identity, also reset the backed-up device's NVS partition:
old `bp-hub-self` revision numbers can otherwise suppress the new identity's cloud
settings. In this verified partition table NVS is `0x9000`, size `0x5000`.
Do this only for the initial identity transition, not routine firmware updates.

```powershell
pio run -d hub/personal-heltec -t upload --upload-port COM17
pio run -d hub/personal-heltec -t uploadfs --upload-port COM17
```

For initial rollout a combined esptool write is preferable: write the bootloader,
partition table, OTA initializer, firmware and LittleFS in a single invocation,
then reset once. Read addresses from the built partition table; do not assume
another hub's offsets. This build's LittleFS begins at `0x670000`.

Verify on serial that identity is 0030, Wi-Fi connects, SX1262 starts, and cloud
requests authenticate. Verify the local browser shows the new collar IDs and the
unchanged cloud app receives real positions. Never print a gateway token in logs.

Restore the full backup at offset zero to roll back both firmware and filesystem.
Preserve `.secrets` separately before deleting or archiving this worktree.
