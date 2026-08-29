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
  the vendor's Apache-2.0 BSP, plus the board's LDO-powered four-bit SDMMC
  interface, without its camera, audio and demo bulk.
- `main`: a responsive 800 x 480 landscape / 480 x 800 portrait LVGL test
  screen displaying eight simulated cats through the same `CatStore` intended
  for LoRa and restored telemetry.
- Touch map navigation with continuous drag panning, **Home**, **Fit**, **+/-**
  zoom, two-finger pinch zoom and an in-app orientation control. All actions
  consume the portable viewport/state layer rather than maintaining a second
  GUI-only model.

```text
Simulator ─┐
SX1262 TLV ├──> CatStore ──> LVGL pages / map markers / local API
Cloud      │
SD restore ┘

GT911 touch ──> LVGL ──> Viewport ──> XYZ tile requests ──> SD loader
```

The grid remains as a diagnostic overlay beneath real, licensed OS Open
Zoomstack Gloucester tiles loaded from SD. It helps reveal missing or misplaced
tiles during map-engine development.

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

The display/touch validation's 785,312-byte application image has SHA-256
`130CB50BDDD636D7E81E37FE98EAB4D7E577A2014F399F395D9D523B8D408182`.
COM27 programming and read-back verification completed successfully. The boot
log reports ESP32-P4 revision 1.3, 16 MB flash, 32 MB PSRAM and a passing PSRAM
test, with no panic or reset loop. The panel displays the 800x480 landscape
BluePaws map testbed, eight simulated cats and the nearby-cat table; GT911
touch and **Fit all** were exercised successfully. C6 networking, SD maps and
LoRa remain outside this first physical validation.

## SDMMC bring-up result — 2026-08-29

The inserted card mounted successfully from its first FAT32 partition using
SDMMC slot 0, four-bit width and a measured 40.00 MHz bus clock. The boot log
reported:

- Product name `APPSD`, type SDHC and 512-byte sectors.
- Physical capacity 515,396,075,520 bytes (480 GiB).
- FAT volume 33,537,654,784 bytes (about 31.24 GiB).
- Free space 33,537,622,016 bytes at the time of the test.

The LVGL status line now reports approximately `SD 31/480 GiB`. The board
continued running the display, touch and simulator without a panic or reset
loop. The SD-enabled 872,624-byte application image has SHA-256
`B599D8CEE6B342956A946F6D2C4CF5659DEDA16620860033B731FDE3ED273680`.
Only the application region was overwritten, and the factory backup remains
untouched. This completes the physical SD interface check; JPEG decoding,
tile caching and filesystem error handling were left for the map fixture phase
below.

## Offline tile fixture preparation — 2026-08-29

QGIS LTR 3.44.13 and GDAL 3.13.2 rendered the official June 2026 OS Open
Zoomstack Vector Tiles with Ordnance Survey's Outdoor QML style. The quick
Gloucester fixture contains 1,580 standard 256 x 256 JPEG XYZ tiles over zoom
levels 12-17, totalling 11,093,618 bytes. All tiles and the corrected fixture
manifest were copied to `/bluepaws/maps` on the FAT32 partition; source and SD
counts, byte totals, manifest hash and a central tile hash matched.

The first corresponding firmware starts at Gloucester (`51.8642, -2.2382`),
computes the visible tile grid, verifies each SD path and places available JPEG
tiles under the cat markers. Its 882,160-byte application binary has SHA-256
`DEF85546486E76119DCA8AB5426552D55DDB9F37527DEC69ADAF6BDE8AC05D42`.
Physical testing displayed all six expected tiles with correct coordinates,
clipping, colours and marker overlay, and **Fit all** selected the new viewport.
The LVGL Tiny JPEG streaming decoder nevertheless took about five seconds to
redraw, visibly filling the raster from top to bottom.

The follow-up build replaces that streaming path with the ESP32-P4 hardware
JPEG engine. Each newly visible JPEG is read once, decoded directly into an
RGB565 PSRAM buffer and retained for LVGL redraws. The 907,904-byte application
binary has SHA-256
`4906ABDFA653D67D46E002F052FC73C5E0E2284E983D200125AB06CB7F658F63`.
App-only flashing at `0x10000` completed with hash verification; the factory
backup and all other flash regions remain untouched. On the first monitored
boot the SD card mounted at 40 MHz in four-bit mode and all six tiles hardware
decoded in approximately 1 ms each, with roughly 100 ms elapsed across SD reads
and map preparation.

The next interaction build adds a clipped viewport, one-tile overscan and a
36-entry decoded-tile LRU in PSRAM. The map can be dragged continuously in both
axes and exposes web-style overlay buttons for **Home**, **Fit**, **+**, **-**
and rotation. The pilot pack constrains the viewport to zoom levels 12-17, and
the same viewport centre survives orientation changes between 800x480 and
480x800.

The GT911 was physically confirmed to report two simultaneous contacts with
stable track IDs 0 and 1. Espressif's pinned `esp_lvgl_port` 2.7.2 nevertheless
passes a fixed two-record array to LVGL's recognizer even when only one record
is valid; the zero-filled release record cancels the real contact. The board
adapter therefore measures the squared distance between the two raw GT911
contacts directly. A 20 percent separation change emits a discrete zoom step
and rebases the reference distance, allowing several steps in one continuous
pinch without orientation-dependent coordinate transforms.

The final 915,696-byte interaction application has SHA-256
`3766143D85760A3A579990B635210E1446768406D7C5113FB5A8FE6D47731925`.
It was flashed only to the application region at `0x10000`; esptool verified
the written hash. Its monitored boot again reports revision 1.3, 16 MB flash,
32 MB PSRAM, the 480 GiB physical card with the FAT32 volume mounted at 40 MHz,
and 1 ms hardware JPEG decodes without panic or reset looping. Physical testing
confirmed two-axis panning, **Home**, **Fit**, **+/-**, landscape/portrait
rotation with aligned touch, and multi-step pinch zoom in both directions.
These controls are an engineering proof rather than the intended final visual
design; smooth animated zoom still depends on asynchronous tile loading and a
later presentation layer.

## LVGL app-shell architecture

The hub behaves like a small appliance launcher, not one ever-growing map
screen. It now boots to an LVGL home tile grid, with a navigation manager that
owns screen transitions. Initial app modules are:

- **Live Map**: positions, trails, geofences, layers and map tools.
- **Cat Summary**: last report, age, battery, radio quality and alert state.
- **Settings**: primary/secondary SSIDs, fallback AP identity, regional and
  display preferences, and update controls.
- **Diagnostics**: C6, LoRa, SD, GNSS/time, memory and firmware health.

These are LVGL screens/components inside one firmware image, not independent
executables. Shared services own `CatStore`, settings persistence, networking,
radio and storage; app pages observe those services and must not initialize
hardware themselves.

The reusable header, Apps/rotation controls and launcher tiles live in
`main/app_shell.cpp`; page-specific rendering remains in the testbed entry
point while its interfaces settle. **Live Map** retains the proven SD tile
cache, drag, pinch, Home, Fit and zoom behaviour. **Cat Summary** and
**Diagnostics** update once per second from the existing shared store and board
state. **Settings** intentionally labels its values as a non-persistent preview
until the C6 networking and settings-storage service is implemented. Rotation
rebuilds the current page without recreating the shared cat, SD, JPEG or display
services.

The first app-shell application is 921,056 bytes with SHA-256
`5220BF6B2411EF51B23B9AEE909F4064AEFF2C5D85E9C32135F2C86ABB7D2A3E`.
It was flashed app-only at `0x10000` with esptool hash verification. The
monitored boot again reported revision 1.3, 16 MB flash, 32 MB PSRAM and the
mounted SD volume without a panic or reset loop. The hardware touch trace
confirmed launcher-to-map navigation, hardware decoding of the cached map
tiles, and return to the launcher through the shell navigation control.

The follow-up visual pass replaces text-only navigation with the selected Home,
rotate, night-mode, map, settings, diagnostics and zoom artwork. The launcher
defaults to a dark, compact appliance-style app grid and can rebuild every page
in a light theme without restarting shared services. Source PNGs live under
`firmware/assets/icons`; the reproducible converter emits alpha-only LVGL masks
for monochrome artwork and ARGB8888 data for the coloured map icon.

Live Map now consumes the full content area in both orientations. Its hamburger
control slides a themed nearby-cat drawer above the existing map object; opening
and closing the drawer does not recreate the viewport, invalidate the decoded
tile cache or request another SD read. Map-centre, Fit and zoom remain separate
overlay controls, while the translucent header Home icon returns to the app
grid. The raster pack itself remains the licensed daylight OS style in both UI
themes; only application chrome changes until a separately rendered night tile
pack is available.

The icon/drawer build is 956,576 bytes with SHA-256
`CA2F6626692DB0ED9791184036B4ECACBC1B0529FD62F20996A84078422B5AD5`.
It was flashed app-only at `0x10000` with esptool hash verification. Its first
monitored boot again mounted the card and completed display/touch startup with
no panic or reset loop.

The next display-control pass adds the supplied brightness and zoom-out artwork.
Brightness opens a themed vertical 10-100 percent slider and drives the existing
5 kHz PWM backlight directly; retaining a 10 percent floor prevents an accidental
black screen during testbed use. The selected value survives page and orientation
rebuilds for the current boot. The light application palette is now warm grey
instead of full white, while the map's text minus control is replaced by the
matching zoom-out image.

That application is 959,904 bytes with SHA-256
`9A69035C64BF0EF5BEF3943C3880E390DC799822A55397970AF2E60870AB820A`.
It was flashed app-only at `0x10000`; esptool verified the written hash. The
monitored clean reset reported ESP32-P4 revision 1.3, 16 MB flash, 32 MB PSRAM,
800x480 display, GT911 touch and the 32 GB FAT volume on the 480 GB physical card,
then started the UI without a panic or reset loop.
