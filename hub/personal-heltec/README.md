# Personal Heltec Wireless Tracker V2 hub

This standalone build compiles the canonical hub services from `../platformio/src`
and packages `../platformio/data` unchanged. A personal-only adapter adds a local
display without changing canonical sources, protocol contracts, or browser routes.

## Display and user button

Wireless Tracker V2 has a 160x80 ST7735 colour TFT (often described as an OLED).
Tap USER to advance through eight pages; hold it for 800 ms to return home:

1. Hub identity, mode, Wi-Fi, last cloud success, Home beacon and LoRa status.
2. Network name, IP addresses, cloud failures, uptime and free heap.
3. Collar 3001: state/profile, last reception, signal, GPS age, cloud result, command.
4. Collar 3002, with the same fields.
5. Collar 3003, with the same fields.
6. Collar 3004, with the same fields.
7. Hub's own GPS position and fix age.
8. Radio settings and relay, command and storage queue depths.

Pages refresh once per second and only changed rows are redrawn. No reports means
"No reports received"; records loaded from storage are explicitly labelled stored.
Radio reception and cloud acceptance are separate indicators. Battery measurement
is not implemented on this board and is labelled unmeasured. The display stays on.

USER is GPIO0, also the boot strap: use it after normal startup, and do not hold it
while resetting unless you intend to enter the ROM bootloader. RESET is not a page
button. Navigation never changes radio, Wi-Fi, BLE or collar settings.

The display uses FSPI (SCK41, MOSI42, CS38, DC40, reset39, backlight21); LoRa keeps
its existing HSPI bus. Shared Vext/GNSS power on GPIO3 remains owned by canonical
firmware. UI reads take short state snapshots and release locks before drawing.

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
Monitor at 115200. For the initial identity transition, flash both firmware and
filesystem. For routine updates, including the display upgrade, upload firmware
only to preserve settings, credentials and the user's BLE selection. Old saved settings can
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
