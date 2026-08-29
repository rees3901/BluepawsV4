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

The first hardware proof used OS Open Zoomstack vector MBTiles rendered locally
with QGIS LTR. It remains a useful reference pack, but its Outdoor cartography
is not the intended BluePaws visual style.

The preferred road-map source is now a bounded Protomaps PMTiles extract made
from current OpenStreetMap data, with the open-source Protomaps `light` style.
The PC renders that vector source to the same hardware-friendly JPEG XYZ layout;
the P4 does not need to parse PMTiles or run a map server. Do not bulk-download
`tile.openstreetmap.org`, whose usage policy prohibits offline prefetching.

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

## OpenStreetMap-style Gloucestershire pack

Large source archives and rendered packs belong under the ignored
`home-hub/maps/work/` directory. Using the current Protomaps CLI, extract the
county rather than downloading the approximately 120 GB planet archive:

```powershell
pmtiles extract https://build.protomaps.com/20260829.pmtiles `
  home-hub/maps/work/sources/gloucestershire-20260829.pmtiles `
  --bbox=-2.72,51.55,-1.62,52.15 --maxzoom=15 --download-threads=8
pmtiles verify home-hub/maps/work/sources/gloucestershire-20260829.pmtiles
pmtiles serve home-hub/maps/work/sources --interface=127.0.0.1 --port=8077 `
  --public-url=http://127.0.0.1:8077
```

In another terminal, generate the QGIS-compatible style and render a fixture:

```powershell
npm --prefix tools/protomaps-style install
node tools/protomaps-style/build-style.mjs `
  '--output=home-hub/maps/work/styles/osm-light.json' `
  '--tile-url=http://127.0.0.1:8077/gloucestershire-20260829/{z}/{x}/{y}.mvt'

& 'C:\Program Files\QGIS 3.44.13\bin\python-qgis-ltr.bat' `
  tools/build_home_hub_map_pack.py `
  '--tile-url=http://127.0.0.1:8077/gloucestershire-20260829/{z}/{x}/{y}.mvt' `
  --mapbox-style home-hub/maps/work/styles/osm-light.json `
  --output home-hub/maps/work/osm-fixture/tiles `
  --manifest home-hub/maps/work/osm-fixture/map_manifest.json `
  --profile fixture
```

After visual approval, change the output directory and use
`--profile gloucestershire`. That profile renders county-wide z10-16 plus z17
around the Gloucester/Cheltenham corridor. OpenStreetMap attribution must stay
in the manifest and the eventual map information panel.

## Aerial layer

The preferred close-detail aerial source is the Environment Agency Vertical
Aerial Photography collection. It is Open Government Licence data, supplied as
5 km British National Grid ECW downloads at roughly 10-50 cm resolution where
surveys exist. QGIS/GDAL can mosaic and reproject selected Gloucester coverage
before rendering it to a separate JPEG XYZ layer.

Copernicus Sentinel-2 true-colour imagery is the open gap-filler where no EA
survey exists. Its best RGB bands are 10 m per pixel, so it is useful for broad
landscape context but not cat-scale or house-scale detail. Google, Bing and
similar consumer basemaps are not offline-pack sources. Road and aerial tiles
will remain separate layer directories so switching layers never requires
duplicating cat markers or interaction overlays.

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

The interaction testbed prepares one extra tile around every visible edge and
retains up to 36 decoded RGB565 tiles in PSRAM. Landscape uses a 515 x 350 map
viewport and portrait uses 444 x 436; the visible tile count therefore follows
the actual pixel viewport rather than a hard-coded tile grid. The overscan is
clipped, so it exists only to make short drags immediate. Cache misses are
still loaded synchronously in this proof and should move to a worker task
before large packs and free-form navigation are treated as production-ready.
