# BluePaws ESP32-P4 Home Hub firmware

This ESP-IDF project targets the GUITION `JC4880P443C_I_W`: ESP32-P4 plus
ESP32-C6, a 4.3-inch 480 x 800 ST7701 MIPI-DSI panel, GT911 touch, 16 MB flash
and 32 MB PSRAM. It does not replace the current `hub/` Heltec ESP32-S3
LoRa/off-grid testbed.

The initial on-device application starts the screen in 800 x 480 landscape,
runs eight deterministic simulated cats through `bluepaws_core`, displays their
positions and telemetry in LVGL, and exposes a touch-operated **Fit all** action.
The grid is a temporary map layer; SD tile decoding is the next firmware slice.

## Toolchain

- ESP-IDF `5.5.4` (the component manifest accepts only the 5.5 release line).
- LVGL `9.5.0`, resolved by ESP-IDF Component Manager.
- The component manager also resolves Espressif's ST7701 1.1.3, GT911 1.2.1
  and `esp_lvgl_port` 2.7.2 components.

From an ESP-IDF 5.5.4 shell:

```powershell
cd home-hub/firmware
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Do not copy a generated `sdkconfig` between unrelated P4 boards. The checked-in
`sdkconfig.defaults` and `guition_jc4880p443c` adapter are specific to this SKU.
The adapter deliberately uses the factory-proven ST7701 command sequence and
34 MHz 480x800 timing; replacing it with a generic or newer preset can leave
revision 1.3 hardware with only a grey backlit panel.

## Architecture boundary

- `components/bluepaws_core` is portable C++17: Web Mercator/XYZ calculations,
  fixed-capacity tile placement, an eight-cat state store and simulator.
- `components/guition_jc4880p443c` owns only this board's display, backlight and
  touch initialization.
- `main` is the temporary LVGL testbed UI. Generated LVGL 9 pages should move
  into their own component rather than accumulating here.
- SD, ESP32-C6 networking and SX1262 LoRa will be adapters around `CatStore`.

Arduino-ESP32 supports the P4 and GUITION supplies Arduino examples. If an
Arduino-only library becomes useful, add Arduino as an ESP-IDF component; do
not convert the board adapter or GUI model into a monolithic sketch.

Run the portable host verification from the repository root:

```powershell
node tools/test_home_hub_core.mjs
```

Board provenance, package checksum, confirmed pins and bring-up checkpoints are
recorded in `docs/HOME_HUB_P4_TESTBED.md`.
