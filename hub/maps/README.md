# Home Hub offline maps

The supported Home Hub map is a single local OpenStreetMap-derived raster pack:

```text
/bluepaws/maps/layers/osm-road-100km/tiles/{z}/{x}/{y}.jpg
```

Both the ESP32-P4 touchscreen and the Off-Grid browser dashboard use this same
256-pixel JPEG XYZ tree at zoom levels 5-17. The firmware exposes no map-layer
picker because there is only one supported basemap. If the SD pack is absent,
the browser falls back to its small bundled grid and coastline; the hub never
contacts an online tile service.

The former PMTiles/MapLibre vector experiment and the satellite/aerial packs
are intentionally unsupported. Hardware testing found no useful visual benefit
from the vector presentation, while the freely distributable national imagery
was too low-resolution for the intended pet-tracking view. Do not install new
packs in these obsolete locations:

```text
/bluepaws/maps/vector/
/bluepaws/maps/imagery/
/bluepaws/maps/layers/satellite/
/bluepaws/maps/layers/satellite-v2/
/bluepaws/maps/layers/aerial/
/bluepaws/maps/layers/ordnance-survey/
/bluepaws/maps/layers/ordnance-survey-100km/
```

Those directories may be removed from an existing card after verifying the
absolute target path. Preserve the entire `osm-road-100km` directory.

## Storage guidance

Use a FAT32 first partition. Keep enough free space for update staging,
telemetry, indexes and filesystem headroom; the firmware does not require the
card to be filled with nationwide imagery. Tile packs and source databases are
deployment artefacts and must not be committed to Git.

OpenStreetMap attribution and the licence/source metadata used to create the
installed pack must remain with the deployment. Do not bulk-download from the
public `tile.openstreetmap.org` service; build bounded offline packs from a
permitted regional extract instead.
