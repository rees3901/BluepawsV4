# BluePaws ESP32-P4 Home Hub firmware

This ESP-IDF project targets the GUITION `JC4880P443C_I_W`: ESP32-P4 plus
ESP32-C6, a 4.3-inch 480 x 800 ST7701 MIPI-DSI panel, GT911 touch, 16 MB flash
and 32 MB PSRAM. It does not replace the current `hub/` Heltec ESP32-S3
LoRa/off-grid testbed.

The initial on-device application starts the screen in 800 x 480 landscape,
runs eight deterministic simulated cats through `bluepaws_core`, displays their
positions and telemetry in LVGL, and exposes touch-operated map controls.
The board mounts the first FAT32 microSD partition through four-bit SDMMC
without ever auto-formatting it. The testbed resolves visible XYZ tile IDs,
reads their compact 256-pixel JPEG files and uses the ESP32-P4 hardware JPEG
engine to decode each newly visible tile into RGB565 PSRAM. LVGL then draws the
memory-backed tiles below the live cat markers. The interactive testbed now
supports continuous one-finger panning, **Home**, **Fit**, integer **+/-** zoom,
direct GT911 two-finger pinch zoom and in-app landscape/portrait rotation. A one-tile
overscan and 36 fixed PSRAM screen slots keep each LVGL image object permanently
paired with its own descriptor and decoded buffer. Moving SD reads to a
background loader remains appropriate
before this becomes the production GUI. These controls are hardware proofs;
the product UI will place the map inside an app-launcher navigation shell.

Sub-tile pans now move the existing LVGL image objects without detaching their
sources. When the overscan grid crosses a tile boundary, decoded XYZ identities
are permuted into their new fixed slots and only the newly exposed edge is read
from SD. The map status reports that work as `new N`, making SD-load stalls
separate from ordinary redraw latency.

The Live Map's top-right **MAP** control opens a right-hand layer drawer without
moving the map. It selects the Street pack at `/bluepaws/maps/tiles`, Satellite
at `/bluepaws/maps/layers/satellite/tiles`, or Aerial at
`/bluepaws/maps/layers/aerial/tiles`. Missing SD directories are disabled. A
selection keeps the current centre, clamps zoom to that pack's supported range,
invalidates the decoded screen slots and redraws the same cat overlays. Each
slot identity includes the selected layer as well as the XYZ ID;
missing tiles remain blank rather than silently showing imagery from a different
layer.

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
- `components/guition_jc4880p443c` owns only this board's display, backlight,
  touch and SDMMC initialization.
- `main` is the temporary LVGL testbed UI and interactive hardware-decoded SD
  tile proof. Generated LVGL 9 pages and the eventual asynchronous tile loader
  should move into their own components rather than accumulating here.
- The SD tile loader, ESP32-C6 networking and SX1262 LoRa will be adapters
  around `CatStore`.

The SD adapter mounts `/sdcard` at 40 MHz in four-bit mode, uses the board's
on-chip LDO channel 4 and selects the first FAT partition. A mount failure is
reported in the boot log and on the LVGL status line; firmware does not format
or modify an unreadable card. The present card can therefore keep a FAT32 first
partition for the hub and an optional PC-only second partition in another
format.

Arduino-ESP32 supports the P4 and GUITION supplies Arduino examples. If an
Arduino-only library becomes useful, add Arduino as an ESP-IDF component; do
not convert the board adapter or GUI model into a monolithic sketch.

Run the portable host verification from the repository root:

```powershell
node tools/test_home_hub_core.mjs
```

Board provenance, package checksum, confirmed pins and bring-up checkpoints are
recorded in `docs/HOME_HUB_P4_TESTBED.md`.
