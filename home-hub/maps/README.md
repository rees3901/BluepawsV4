# Home Hub offline map assets

The prototype SD-card layout is:

```text
/bluepaws/maps/map_manifest.json
/bluepaws/maps/tiles/{z}/{x}/{y}.jpg
```

Copy `map_manifest.example.json` to the SD card as `map_manifest.json` and
change its region metadata when the first test tile pack is prepared. Raster
tiles use conventional XYZ addressing and 256-pixel JPEG images.

Do not commit regional tile packs, PMTiles archives or MBTiles databases to
Git. A later tool will build a tiny redistributable fixture of 9-25 tiles once
the test location and map-data licence/attribution are selected.

## Gloucester pilot pack

The first real-card proof is deliberately bounded to roughly 4 GB:

- UK/Great Britain overview at zoom levels 5-11.
- A roughly 40 km square centred on Gloucester (`51.8642, -2.2382`) at zoom
  levels 12-17.
- 256 x 256 JPEG XYZ tiles at approximately quality 85.
- Land/water, roads, footpaths, buildings from zoom 15, parks/woodland and
  place/road labels. Cats, trails and geofences stay as live LVGL overlays.

The preferred source is OS Open Zoomstack vector MBTiles rendered locally with
QGIS LTR. Its approximately 2.6 GB download provides Great Britain coverage
down to street level and permits offline use under the OS OpenData terms. It
does not cover Northern Ireland; a coarse UK outline may use a separately
licensed open dataset, and a later full-UK pack will need an explicit NI source.
Do not bulk-download `tile.openstreetmap.org`, whose usage policy prohibits
offline prefetching.

The FAT32 volume is about 31.24 GiB. Keep normal map payloads below 20-24 GiB
to leave room for update staging, indexes, telemetry and filesystem headroom.
The 4 GB pilot therefore fits comfortably. QGIS is a preparation tool on the
Windows PC only; the hub will read pre-rendered tiles directly from SD and does
not run a map server.

On Windows, render a quick fixture through the installed QGIS LTR environment:

```powershell
& 'C:\Program Files\QGIS 3.44.13\bin\python-qgis-ltr.bat' `
  tools\build_home_hub_map_pack.py `
  --mbtiles 'C:\map-work\OS_Open_Zoomstack.mbtiles' `
  --style 'C:\map-work\OS-Open-Zoomstack-Stylesheets\Vector Tiles\QGIS Stylesheets (QML)\Outdoor style.qml' `
  --output 'C:\map-work\fixture\tiles' `
  --manifest 'C:\map-work\fixture\map_manifest.json' `
  --profile fixture
```

Use `--profile pilot` only after the small fixture has rendered and displayed
correctly. The renderer refuses to write into a non-empty output directory so
an accidental rerun cannot silently mix or overwrite packs.

The first generated fixture uses the official June 2026 OS Open Zoomstack
Vector Tiles database (2,852,712,448 bytes, SHA-256
`2AE12B1BAA7F582C37A02B189A52865E09AF9A4E162CCC61DC4E11D882047A0A`).
It covers approximately 6 km around central Gloucester at zoom 12-17 and
contains 1,580 JPEG tiles totalling 11,093,618 bytes. Source and card copies
were count-, size- and sample-hash verified before the first device test.

The initial LVGL Tiny JPEG streaming path proved the files and coordinates but
took about five seconds to redraw six tiles. The production-direction test path
now keeps the same compact JPEG pack, decodes a newly visible tile with the
ESP32-P4 hardware JPEG engine directly into RGB565 PSRAM, and presents that
buffer to LVGL as a memory image. On the first device boot each of the six
visible tiles decoded in approximately 1 ms; SD reads and UI setup brought the
whole initial map preparation to roughly 100 ms.
