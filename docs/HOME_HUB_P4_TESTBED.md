# ESP32-P4 Home Hub testbed

Status: first portable firmware slice; hardware binding pending.

## Decisions in force

The Home Hub target is an ESP32-P4 application processor with ESP32-C6
connectivity, a roughly four-inch touch display, SD storage, fully offline map
operation and LoRa collar communication. The implementation brief's suggestion
to defer the physical LoRa daughterboard does not remove LoRa from the product
architecture: simulation is only the bring-up input until the radio wiring is
known.

The existing `hub/` firmware remains the working Heltec ESP32-S3 LoRa/off-grid
testbed. The P4 work lives under `home-hub/` and will reuse the shared BluePaws
TLV protocol and the proven Home/Portable/Off-Grid policy without coupling the
touchscreen to the current web UI or Arduino monolith.

## Still required from the selected board

Do not choose a BSP, pin map or display driver until these are recorded from the
board label, schematic and vendor example:

- Manufacturer, exact product/SKU and board revision.
- Native panel resolution and display interface/controller.
- Touch controller, bus, reset and interrupt pins.
- Flash and PSRAM sizes and interfaces.
- SDMMC/SPI mode and pins.
- P4-to-C6 transport and the vendor's supported ESP-IDF/ESP-Hosted version.
- Free expansion bus/pins for the SX1262, plus radio reset, busy and DIO1.
- Backlight control, USB/debug route and power budget.

Photographs of both sides and a link or copy of the vendor schematic are enough
to begin this discovery pass.

Two current Waveshare products match the description but are not electrically
interchangeable:

- `ESP32-P4-WIFI6-Touch-LCD-4B` (SKU 31416) is a 4.0-inch, 720 x 720 MIPI-DSI
  board with GT911 touch, 32 MB PSRAM, 32 MB flash, SDIO-connected ESP32-C6 and
  an SDIO 3.0 TF slot.
- `ESP32-P4-WIFI6-Touch-LCD-4.3` (SKU 33874; `33875` adds a camera) is a
  4.3-inch, 480 x 800 MIPI-DSI/ST7701 board with GT911 touch, the same headline
  memory and C6 arrangement, an SDIO 3.0 TF slot and a 40-pin expansion header.

Official references: [4B documentation](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4B)
and [4.3 documentation](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3).
The board silkscreen/SKU must decide between them; the implementation brief's
4.3-inch wording alone is not confirmation.

## First implemented boundary

`home-hub/firmware/components/bluepaws_core` is the shared application model for
the touchscreen, local web service and telemetry transports. It uses bounded
arrays for eight cat records and 36 visible tile placements, so tile layout and
marker updates do not allocate memory during interaction.

```text
Simulator ─┐
SX1262 TLV ├──> CatStore ──> map marker layer / cat list / local API
Cloud      │
SD restore ┘

Touch drag ──> Viewport ──> XYZ tile requests ──> async SD loader ──> LVGL pool
```

The native test covers projection round trips, tile selection, pan direction,
fit-all padding, last-known-position retention, eight simulated cats and fixed
capacity. It can be verified without pretending that display, touch, SD, C6 or
LoRa hardware has been brought up.

## Next hardware-backed slice

Once the exact board is known:

1. Pin its recommended ESP-IDF release and vendor BSP.
2. Build the untouched vendor display/touch/SD/C6 example.
3. Record a bring-up report and measured PSRAM/free heap.
4. Add the P4 project shell and board adapter.
5. Bind LVGL to a reusable 3 x 3 raster tile pool and a marker overlay.
6. Load a 9-tile licensed test fixture from SD in a background task.
7. Feed the current simulator through `CatStore`, then port the existing
   SX1262 TLV receive path behind the same API.
