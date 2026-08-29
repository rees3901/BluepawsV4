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
