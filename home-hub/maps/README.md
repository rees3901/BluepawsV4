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
