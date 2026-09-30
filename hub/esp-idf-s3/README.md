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

The first milestone is intentionally a hardware-safe port foundation: display,
touch, dimming, SD detection and the portable BluePaws cat store/simulator. It
provides the compact overview interaction on real hardware before native S3
Wi-Fi/BLE, cloud sync and the local dashboard are enabled. The P4-only MIPI-CSI
camera and hardware JPEG decoder have no equivalent on this board.

Build and flash with ESP-IDF 5.5.4:

```powershell
cd hub/esp-idf-s3
idf.py set-target esp32s3
idf.py build
idf.py -p COM11 flash monitor
```

If automatic download mode does not engage, hold **BOOT**, tap **RESET**, then
release **BOOT** before running the flash command.
