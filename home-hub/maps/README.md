# Home Hub offline map assets

The prototype SD-card layout is:

```text
/bluepaws/maps/map_manifest.json
/bluepaws/maps/tiles/{z}/{x}/{y}.jpg
```

The August 2026 multi-layer test card extends that without breaking the current
firmware's hard-coded road path:

```text
/bluepaws/maps/map_manifest.json             active OSM road manifest
/bluepaws/maps/tiles/{z}/{x}/{y}.jpg         active OSM road layer
/bluepaws/maps/layers/aerial/...             EA Gloucester aerial layer
/bluepaws/maps/layers/osm-road-v2/...         high-contrast OSM road layer
/bluepaws/maps/layers/ordnance-survey/...     OS Open Zoomstack Road layer
/bluepaws/maps/layers/satellite/...          legacy EOX Sentinel-2 overview layer
/bluepaws/maps/layers/satellite-v2/...       coherent EA high-resolution aerial layer
/bluepaws/maps/legacy/os-zoomstack-fixture/  preserved first hardware proof
```

The current firmware reads only the active root `tiles` tree. The two layer
directories are ready for the forthcoming map-layer switcher; copying them to
the card now avoids another long preparation step later.

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
from current OpenStreetMap data. The hardware pack uses the BluePaws
`bluepaws-carto` style: warm land, strong water/woodland/building fills, an
OSM-Carto-like coloured road hierarchy and dark, haloed labels. The original
Protomaps `light` render was technically correct but too pale on the hub LCD.
The PC renders that vector source to the same hardware-friendly JPEG XYZ layout;
the P4 does not need to parse PMTiles or run a map server. Do not bulk-download
`tile.openstreetmap.org`, whose usage policy prohibits offline prefetching.
OpenStreetMap's [downloading data](https://wiki.openstreetmap.org/wiki/Downloading_data)
guide recommends starting with a regional extract and using extract providers
for larger downloads. That is the model used here: obtain a bounded vector
extract, then render it locally. It is distinct from scraping the public OSM
raster or vector tile services, whose usage policies prohibit offline bulk
prefetching.
The card pack combines a Great Britain z5-11 extract with a Gloucestershire
z10-16 extract and z17 detail along the Gloucester/Cheltenham corridor. The
overlap is intentional and the county render wins when the two trees are
merged.

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
  '--output=home-hub/maps/work/styles/osm-bluepaws-carto.json' `
  '--flavor=bluepaws-carto' `
  '--tile-url=http://127.0.0.1:8077/gloucestershire-20260829/{z}/{x}/{y}.mvt'

& 'C:\Program Files\QGIS 3.44.13\bin\python-qgis-ltr.bat' `
  tools/build_home_hub_map_pack.py `
  '--tile-url=http://127.0.0.1:8077/gloucestershire-20260829/{z}/{x}/{y}.mvt' `
  --mapbox-style home-hub/maps/work/styles/osm-bluepaws-carto.json `
  --output home-hub/maps/work/osm-fixture/tiles `
  --manifest home-hub/maps/work/osm-fixture/map_manifest.json `
  --profile fixture
```

After visual approval, change the output directory and use
`--profile gloucester` for the card-efficient city test pack, or
`--profile gloucestershire` for the larger master pack. The city profile covers
Gloucester and its immediate approaches at z10-17. The county profile renders
county-wide z10-16 plus z17 around the Gloucester/Cheltenham corridor.
OpenStreetMap attribution must stay in the manifest and the eventual map
information panel.

The LCD-focused `bluepaws-road` flavour uses stronger building outlines,
vegetation, water, road casings and label halos than the earlier pastel pack:

```powershell
node tools/protomaps-style/build-style.mjs `
  '--output=home-hub/maps/work/styles/osm-bluepaws-road.json' `
  '--flavor=bluepaws-road' `
  '--tile-url=http://127.0.0.1:8077/gloucestershire-20260829/{z}/{x}/{y}.mvt'

& 'C:\Program Files\QGIS 3.44.13\bin\python-qgis-ltr.bat' `
  tools/build_home_hub_map_pack.py `
  '--tile-url=http://127.0.0.1:8077/gloucestershire-20260829/{z}/{x}/{y}.mvt' `
  --mapbox-style home-hub/maps/work/styles/osm-bluepaws-road.json `
  --output 'D:\bluepaws\maps\layers\osm-road-v2\tiles' `
  --manifest 'D:\bluepaws\maps\layers\osm-road-v2\map_manifest.json' `
  --profile gloucester --quality 90 --name 'BluePaws OpenStreetMap Road'
```

## Ordnance Survey road layer

OS Open Zoomstack is an Open Government Licence vector basemap covering Great
Britain from national to street level. Download the official Vector Tiles
(MBTiles) release rather than caching OS API raster responses. The QGIS builder
flattens the official Road style's unsupported 3D building extrusion into
ordinary 2D footprints before rasterising, retaining roads, labels and local
context on the hub:

```powershell
curl.exe -L --continue-at - `
  --output 'D:\bluepaws\maps\work\os-open-zoomstack-2026-06\OS_Open_Zoomstack.mbtiles' `
  'https://api.os.uk/downloads/v1/products/OpenZoomstack/downloads?area=GB&format=Vector%20Tiles&subformat=%28MBTiles%29&redirect'

& 'C:\Program Files\QGIS 3.44.13\bin\python-qgis-ltr.bat' `
  tools/build_home_hub_map_pack.py `
  --mbtiles 'D:\bluepaws\maps\work\os-open-zoomstack-2026-06\OS_Open_Zoomstack.mbtiles' `
  --mapbox-style 'D:\bluepaws\maps\work\os-open-zoomstack-2026-06\styles\Vector Tiles\Mapbox GL Styles\OS Open Zoomstack - Road.json' `
  --output 'D:\bluepaws\maps\layers\ordnance-survey\tiles' `
  --manifest 'D:\bluepaws\maps\layers\ordnance-survey\map_manifest.json' `
  --profile gloucester --quality 90 --name 'BluePaws Ordnance Survey Road'
```

## Aerial layer

The preferred close-detail aerial source is the Environment Agency Vertical
Aerial Photography collection. It is Open Government Licence data, supplied as
5 km British National Grid ECW downloads at roughly 10-50 cm resolution where
surveys exist. QGIS/GDAL can mosaic and reproject selected Gloucester coverage
before rendering it to a separate JPEG XYZ layer.

For the Gloucester/Sandhurst hardware layer, use one Environment Agency survey
only. The `build_ea_aerial_pack.py` builder downloads the selected official 5 km
packages, mosaics their native ECW rasters before creating XYZ tiles, and never
fills gaps from a second provider. This is essential: mixing Sentinel and EA
pixels inside one apparent imagery layer produced visible seams and a false
impression that adjacent XYZ tiles were misaligned.

The validated 2007 Gloucester survey is 20 cm/pixel. Block `SO8015` contains
real RGB pixels at the tracker centre, unlike the newer partial strip. Add the
adjacent `SO8020` block when the Defra package service can generate it without
timing out; both belong to the same coherent survey. Build the validated block
directly onto a staging directory on the SD
card with the QGIS LTR Python environment, which includes the licensed ECW
reader:

```powershell
& 'C:\Program Files\QGIS 3.44.13\bin\python-qgis-ltr.bat' `
  tools\build_ea_aerial_pack.py `
  --work 'D:\bluepaws\maps\work\ea-aerial' `
  --output 'D:\bluepaws\maps\layers\satellite-v2\tiles' `
  --manifest 'D:\bluepaws\maps\layers\satellite-v2\map_manifest.json' `
  --year 2007 --resolution 0.2 --min-zoom 14 --max-zoom 17 --workers 3
```

The staged tree must be count-, JPEG-signature-, and sample-coordinate checked
before firmware is pointed at `satellite-v2`. Keep the old Sentinel directory
until that check passes, so an interrupted build never destroys the last known
bootable map set.

Earlier experimental packs mixed incomplete 2012 survey strips with Sentinel.
They are retained only as diagnostic history and must not be installed as the
Satellite layer. The validated pack renders bands 1-3 from the 2007 `IRRGB`
product as natural colour and uses neutral pixels at the survey boundary.

Copernicus Sentinel-2 true-colour imagery is the open gap-filler where no EA
survey exists. Its best RGB bands are 10 m per pixel, so it is useful for broad
landscape context but not cat-scale or house-scale detail. Google, Bing and
similar consumer basemaps are not offline-pack sources. Road and aerial tiles
will remain separate layer directories so switching layers never requires
duplicating cat markers or interaction overlays.

The broad satellite pack uses EOX's `s2cloudless_3857` service, which is the
2016/2017 Sentinel-2 cloudless mosaic released under CC BY 4.0 with rendered
tile download explicitly allowed. It contains Great Britain at z5-11 and
Gloucestershire at z12-14. Later EOX annual mosaics have different
non-commercial/licensing terms, so do not silently substitute a newer layer.

The old composite builder below is retained for reproducing the rejected test
pack only. Do not use it for the active Satellite layer because it deliberately
combines providers and creates the visible imagery seam the coherent builder
avoids:

```powershell
& 'C:\Program Files\QGIS 3.44.13\bin\python-qgis-ltr.bat' `
  tools\build_home_hub_aerial_composite.py `
  --aerial home-hub\maps\work\packs\aerial\tiles `
  --satellite home-hub\maps\work\packs\satellite\tiles `
  --output home-hub\maps\work\packs\aerial-composite\tiles `
  --aerial-manifest home-hub\maps\work\packs\aerial\map_manifest.json `
  --output-manifest home-hub\maps\work\packs\aerial-composite\map_manifest.json
```

At z15-z17 the fallback is cropped and scaled from the z14 Sentinel parent, so
it is deliberately soft; true EA pixels retain their native detail. The output
manifest carries both OGL and CC BY 4.0 attribution.

Download only from a provider whose offline terms have been checked. The
bounded downloader is deliberately profile-based:

```powershell
python tools\download_home_hub_raster_tiles.py `
  --profile western-england `
  --url-template 'https://tiles.maps.eox.at/wmts/1.0.0/s2cloudless_3857/default/g/{z}/{row}/{col}.jpg' `
  --output home-hub\maps\work\packs\satellite\tiles `
  --manifest home-hub\maps\work\packs\satellite\map_manifest.json `
  --name 'BluePaws Sentinel-2 cloudless 2016/2017' `
  --attribution 'Sentinel-2 cloudless by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016 & 2017), CC BY 4.0' `
  --source-url 'https://eox.at/2017/08/sentinel-2-global-cloudless-mosaic/'
```

The default profile keeps z12-14 limited to Gloucestershire. The recommended
`western-england` profile keeps Great Britain at z5-11 and extends z12-14 over
a broad region around Gloucester (47,663 requested XYZ positions). The
optional `great-britain` profile extends genuine imagery through z14 across the
whole country (589,877 positions), but takes hours and creates too many small
files for a responsive test card. Every profile deliberately stops at z14:
Sentinel-2 RGB source pixels are 10 m, so extra zooms would consume space
without adding image detail.

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
