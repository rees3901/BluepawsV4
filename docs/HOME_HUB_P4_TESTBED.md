# ESP32-P4 Home Hub testbed

Status: GUITION hardware target confirmed; the first display/touch application
is running on the physical revision 1.3 board.

## Confirmed target

The testbed board is the GUITION/Shenzhen Jingcai Intelligent
`JC4880P443C_I_W`. The `_Y` suffix used in one mechanical drawing denotes the
version supplied with an enclosure; it is not a different electrical target.

The vendor download linked from the product listing was reconstructed from its
seven published shards and verified before inspection:

- Download: `http://pan.jczn1688.com/directlink/1/HMI%20display/JC4880P443C_I_W.zip`
- Archive size: 309,232,242 bytes.
- SHA-256: `7CEA2154667033A639B62A42D1952066CA55C78E187846351F5FACB0C3F5232F`.
- All shard hashes in the download manifest passed.
- No executable from the package was run.

The public [GUITION P4-series repository](https://github.com/guitionofficial/P4-series)
contains the same product family, but its checked-in ESP-IDF example is older
than the download package. The archive's `ESP-IDF_5.5.4` example and modified
BSP 5.2.3 are therefore the implementation baseline.

## Hardware facts in force

| Function | Confirmed configuration |
| --- | --- |
| Application processor | ESP32-P4 revision 1.3, dual-core RISC-V |
| Connectivity companion | On-module ESP32-C6 for Wi-Fi/Bluetooth |
| Display | 4.3-inch IPS, native portrait 480 x 800 |
| Display controller/bus | ST7701S over two-lane MIPI-DSI |
| Touch | GT911 capacitive touch on I2C |
| Memory | 16 MB flash and 32 MB PSRAM |
| Backlight/reset | PWM GPIO23; panel reset GPIO5 |
| Shared I2C | SDA GPIO7; SCL GPIO8; 400 kHz in the vendor BSP |
| Touch control pins | Reset and interrupt are `GPIO_NUM_NC` in the vendor BSP |
| microSD | 4-bit SDMMC: D0 39, D1 40, D2 41, D3 42, CMD 44, CLK 43 |
| Audio I2S | MCLK 13, BCLK 12, WS 10, DOUT 9, DIN 48, amplifier enable 11 |
| Supply | 5 V; vendor typical current approximately 320 mA without added LoRa |

The active display area is 93.60 x 56.16 mm and the bare board is approximately
117.01 x 69.41 mm. The board also exposes USB, UART, RS-485, camera, speaker,
battery and 2 x 13 expansion connections. These extra interfaces are not yet
claimed by BluePaws.

## Framework decision

ESP-IDF 5.5.4 is the foundation, with LVGL pinned to 9.5.0. GUITION ships both
Arduino and ESP-IDF examples, and current Arduino-ESP32 supports ESP32-P4, but
an all-Arduino firmware is not the best ownership boundary for this product.
MIPI-DSI, SDMMC map I/O, PSRAM, ESP-Hosted and radio task scheduling need direct
IDF control.

This does not prevent Arduino libraries later. Arduino can be included as an
ESP-IDF component if a library materially helps. Generated LVGL screens must be
kept in a separate UI component and target LVGL 9; they should not call the
board adapter or transport tasks directly. The risk to generated pages is an
LVGL v8/v9 mismatch, not the choice of Arduino-style `setup()`/`loop()`.

The dependency lock also pins `esp_lvgl_port` 2.7.2. Version 2.9.0 expects a
DPI callback newer than the ESP-IDF 5.5.4 baseline, so allowing an unbounded
2.x upgrade would make an otherwise reproducible checkout fail to compile.

The delivered board identifies as ESP32-P4 revision 1.3. ESP-IDF treats the
pre-v3 and v3 P4 hardware families as mutually exclusive, so the checked-in
defaults explicitly select pre-v3 support with revision 1.0 as the minimum.
LVGL's optional fast-memory placement is disabled to stay within the smaller
pre-v3 internal RAM map. Do not use this firmware image on a revision 3.x P4
board.

The factory application uses GUITION's ST7701 command sequence, a 34 MHz pixel
clock and its 480x800 porch values. The newer package's 28 MHz/default command
combination produced only a backlit grey panel on this revision 1.3 unit.
BluePaws keeps the registry component pristine and applies the factory-proven
values in the board adapter. LVGL draw and rotation buffers live in the 32 MB
PSRAM; synchronous copying is retained until DMA2D is separately validated on
pre-v3 ESP32-P4 silicon.

## Implemented boundary

`home-hub/firmware` is now a complete ESP-IDF project rather than only a
portable library. It contains:

- `bluepaws_core`: bounded Web Mercator, tile layout, cat state and simulator.
- `guition_jc4880p443c`: a focused ST7701/GT911/backlight adapter derived from
  the vendor's Apache-2.0 BSP, without its camera, audio and demo bulk.
- `main`: a landscape 800 x 480 LVGL test screen displaying eight simulated
  cats through the same `CatStore` intended for LoRa and restored telemetry.
- A touch-operated **Fit all** action, proving the GUI consumes the portable
  map/state layer rather than maintaining a second model.

```text
Simulator ─┐
SX1262 TLV ├──> CatStore ──> LVGL pages / map markers / local API
Cloud      │
SD restore ┘

GT911 touch ──> LVGL ──> Viewport ──> XYZ tile requests ──> SD loader
```

The first screen deliberately uses a map-grid placeholder. It proves display,
touch, landscape rotation and live state flow without embedding unlicensed map
artwork or pretending SD tile loading exists.

## LoRa boundary

LoRa remains required. The intended radio is an SX1262-class transceiver using
the existing BluePaws TLV protocol. No expansion-header pin assignment has been
committed yet. The schematic exposes several P4 GPIOs, but the exact header
orientation, boot-strapping constraints and any board-revision differences
must be checked on the delivered unit before choosing SPI SCK/MOSI/MISO, NSS,
reset, busy and DIO1. Display, touch, SDMMC, audio and C6 pins listed above are
reserved and must not be reused.

## Bring-up sequence

1. Install ESP-IDF 5.5.4 and build the project without changing component pins.
2. Flash through the high-speed USB connector and record the P4 revision,
   boot log, PSRAM size, free heap and display/touch result.
3. Mount a FAT32 microSD card and validate the six documented SDMMC signals.
4. Record the C6 firmware version and exercise the vendor Wi-Fi scan before
   adding BluePaws networking.
5. Photograph both sides of the delivered board and continuity-check the
   expansion header before assigning the SX1262.
6. Add a licensed nine-tile SD fixture and an asynchronous tile loader.
7. Port the proven SX1262 TLV receive path behind `CatStore`; simulation remains
   selectable as a deterministic bench mode.

Host-side core verification remains available with:

```powershell
node tools/test_home_hub_core.mjs
```

## Physical bring-up result — 2026-08-29

Before programming, the complete 16 MB factory flash was retained as
`JC4880P443C_I_W_factory_2026-08-29.bin` with SHA-256
`12B6E82FC39CBC936101DD5EE048E7E12F00B0AD0D2E38396882611260CAA1B7`.
Only the application region at `0x10000` was overwritten during the final
diagnostic iterations.

The final 785,312-byte application image has SHA-256
`130CB50BDDD636D7E81E37FE98EAB4D7E577A2014F399F395D9D523B8D408182`.
COM27 programming and read-back verification completed successfully. The boot
log reports ESP32-P4 revision 1.3, 16 MB flash, 32 MB PSRAM and a passing PSRAM
test, with no panic or reset loop. The panel displays the 800x480 landscape
BluePaws map testbed, eight simulated cats and the nearby-cat table; GT911
touch and **Fit all** were exercised successfully. C6 networking, SD maps and
LoRa remain outside this first physical validation.
