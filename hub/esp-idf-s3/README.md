# BluePaws ESP32-S3 compact Home Hub

This is a separate ESP-IDF target for the GUITION `JC3248W535C` / closely
related `JC3248W535EN` 3.5-inch display board. It deliberately does not alter
the ESP32-P4 firmware.

Confirmed on the connected unit:

- ESP32-S3 revision 0.2
- 8 MB embedded octal PSRAM
- native USB Serial/JTAG

Vendor and hardware-reference configuration used by this target:

- 16 MB QIO flash
- AXS15231B 320 x 480 QSPI LCD, used as 480 x 320 landscape
- QSPI: SCK 47, CS 45, D0..D3 21/48/40/39, backlight 1
- touch: AXS15231B I2C address 0x3b, SDA 4, SCL 8
- microSD: one-bit SDMMC, CLK 12, CMD 11, D0 13

The LVGL 9 adapter deliberately follows the vendor LVGL 8 display pipeline:
LVGL renders a complete 480 x 320 RGB565 frame in PSRAM, then the adapter
software-rotates it into ten DMA-capable native-portrait strips. Those strips
are sent sequentially from native row zero so the AXS15231B QSPI driver uses
one `RAMWR` followed by `RAMWRC` continuations. Partial dirty-rectangle writes
are not used because this panel driver omits row-window programming.

The S3 target now reuses the hardware-independent Home Hub services from the P4
firmware:

- persistent NVS settings and Home / Portable / Off-Grid mode selection
- native S3 Wi-Fi with primary/secondary network fallback and Off-Grid AP
- the offline web dashboard, HTTP APIs and `http://bluepaws.local/` mDNS
- native NimBLE Home beaconing and the user-requested five-second BLE scan
- Supabase reporting, collar snapshot sync and generic command acknowledgement
- simulated hub GNSS/battery telemetry until the radio/GNSS daughterboard exists

The mode safety policy is shared with the P4: `BLUEPAWS_HOME` advertising is
allowed only in Home mode. Portable and Off-Grid explicitly stop advertising;
Off-Grid BLE discovery remains off until the user requests a short scan.

The compact native UI now boots to the SD-card road map with an eight-collar
scrolling drawer, touch pan/zoom, a mode-confirmation dialog, live Wi-Fi and BLE
status, and a settings overlay. The settings tabs cover Wi-Fi scanning and
manual credentials, hub controls, display brightness, collar summaries and
diagnostics. The offline web dashboard remains available from the same SPIFFS
assets as the P4. A battery percentage is deliberately omitted from the native
header because this board target has no validated battery-voltage measurement.
The P4-only MIPI-CSI camera, QR reader and hardware JPEG decoder have no
equivalent configured on this board. Audio and screen rotation controls are
also omitted until their hardware paths are validated.

Cloud synchronization is compiled in but requires an untracked
`main/home_hub_secrets.h` based on `main/home_hub_secrets.example.h`. Without a
provisioned bearer token the local Wi-Fi, AP, dashboard, mDNS, BLE and simulator
continue to operate, but the hub does not report to Supabase.

Build and flash with ESP-IDF 5.5.4:

```powershell
cd hub/esp-idf-s3
idf.py set-target esp32s3
idf.py build
idf.py -p COM11 app-flash
```

If automatic download mode does not engage, hold **BOOT**, tap **RESET**, then
release **BOOT** before running the flash command.

On this board, opening the native USB serial port for a monitor can reset it
into download mode. After flashing, release it into normal boot by briefly
asserting then releasing RTS with DTR low (or power-cycle the board without
holding BOOT). Avoid leaving a serial monitor open while checking the display.
