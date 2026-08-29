# BluePaws ESP32-P4 Home Hub firmware

This directory is the start of the ESP32-P4 + ESP32-C6 touchscreen Home Hub.
It does not replace the current `hub/` Heltec ESP32-S3 testbed.

The first component, `bluepaws_core`, is portable C++17 and contains:

- Web Mercator and XYZ tile calculations.
- A fixed-capacity map tile layout for an 800 x 480-class display.
- Touch-pan and fit-all-marker calculations.
- A source-independent eight-cat state store.
- Deterministic simulated collar telemetry.

Both simulated packets and future SX1262/BluePaws TLV packets update the same
`CatStore`. LVGL, SD card, ESP32-C6 connectivity and LoRa tasks will be adapters
around this core after the exact board and peripheral wiring are confirmed.

Run the host verification from the repository root:

```powershell
node tools/test_home_hub_core.mjs
```

The component has an ESP-IDF `CMakeLists.txt`, but a complete target project is
intentionally not declared yet. Selecting an ESP-IDF target, BSP and pin map
before identifying the board revision would create a misleading build.
