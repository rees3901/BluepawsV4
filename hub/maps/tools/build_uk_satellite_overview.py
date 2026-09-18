#!/usr/bin/env python3
"""Build the browser-only UK Sentinel-2 overview archive."""

import argparse
import io
import math
import sqlite3
import subprocess
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from PIL import Image


BOUNDS = (-8.75, 49.75, 1.85, 60.95)
ATTRIBUTION = (
    "Sentinel-2 cloudless by EOX IT Services GmbH "
    "(contains modified Copernicus Sentinel data 2016)"
)
TILE_URL = (
    "https://tiles.maps.eox.at/wmts/1.0.0/"
    "s2cloudless_3857/default/g/{z}/{y}/{x}.jpg"
)


def tile_xy(lon: float, lat: float, zoom: int) -> tuple[int, int]:
    scale = 1 << zoom
    x = int((lon + 180.0) / 360.0 * scale)
    y = int(
        (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi)
        / 2.0
        * scale
    )
    return x, y


def tile_range(zoom: int):
    west, south, east, north = BOUNDS
    x_min, y_max = tile_xy(west, south, zoom)
    x_max, y_min = tile_xy(east, north, zoom)
    for x in range(x_min, x_max + 1):
        for y in range(y_min, y_max + 1):
            yield zoom, x, y


def fetch_webp(tile, quality: int):
    z, x, y = tile
    request = urllib.request.Request(
        TILE_URL.format(z=z, x=x, y=y),
        headers={"User-Agent": "BluePaws-offline-map-builder/1.0"},
    )
    last_error = None
    for attempt in range(4):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                source = response.read()
            with Image.open(io.BytesIO(source)) as image:
                output = io.BytesIO()
                image.convert("RGB").save(
                    output, "WEBP", quality=quality, method=4, optimize=True
                )
            return tile, output.getvalue()
        except Exception as error:  # network retries are intentional
            last_error = error
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"failed tile {z}/{x}/{y}: {last_error}")


def prepare_database(path: Path, minimum_zoom: int, maximum_zoom: int):
    connection = sqlite3.connect(path)
    connection.execute(
        "CREATE TABLE IF NOT EXISTS metadata (name TEXT, value TEXT)"
    )
    connection.execute(
        "CREATE TABLE IF NOT EXISTS tiles ("
        "zoom_level INTEGER, tile_column INTEGER, tile_row INTEGER, "
        "tile_data BLOB, UNIQUE (zoom_level, tile_column, tile_row))"
    )
    connection.execute(
        "CREATE UNIQUE INDEX IF NOT EXISTS tile_index ON tiles "
        "(zoom_level, tile_column, tile_row)"
    )
    metadata = {
        "name": "BluePaws UK satellite overview",
        "type": "baselayer",
        "version": "1",
        "description": "UK Sentinel-2 cloudless 2016 overview for offline use",
        "format": "webp",
        "bounds": ",".join(str(value) for value in BOUNDS),
        "minzoom": str(minimum_zoom),
        "maxzoom": str(maximum_zoom),
        "attribution": ATTRIBUTION,
    }
    connection.execute("DELETE FROM metadata")
    connection.executemany(
        "INSERT INTO metadata (name, value) VALUES (?, ?)", metadata.items()
    )
    connection.commit()
    return connection


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pmtiles", type=Path, required=True)
    parser.add_argument("--min-zoom", type=int, default=5)
    parser.add_argument("--max-zoom", type=int, default=12)
    parser.add_argument("--quality", type=int, default=72)
    parser.add_argument("--workers", type=int, default=12)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    mbtiles = args.output.with_suffix(".building.mbtiles")
    connection = prepare_database(mbtiles, args.min_zoom, args.max_zoom)
    # MBTiles stores rows in TMS order while the source and tile_range use XYZ.
    # Convert them back here so an interrupted build genuinely resumes.
    existing = {
        (zoom, x, (1 << zoom) - 1 - tms_row)
        for zoom, x, tms_row in connection.execute(
            "SELECT zoom_level, tile_column, tile_row FROM tiles"
        )
    }
    pending = [
        tile
        for zoom in range(args.min_zoom, args.max_zoom + 1)
        for tile in tile_range(zoom)
        if tile not in existing
    ]
    total = len(existing) + len(pending)
    print(f"Tiles: {total}; already present: {len(existing)}; pending: {len(pending)}")

    completed = len(existing)
    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = {
            executor.submit(fetch_webp, tile, args.quality): tile for tile in pending
        }
        batch = []
        for future in as_completed(futures):
            (zoom, x, y), tile_data = future.result()
            tms_row = (1 << zoom) - 1 - y
            batch.append((zoom, x, tms_row, tile_data))
            completed += 1
            if len(batch) >= 200:
                connection.executemany(
                    "INSERT OR REPLACE INTO tiles VALUES (?, ?, ?, ?)", batch
                )
                connection.commit()
                batch.clear()
            if completed % 500 == 0 or completed == total:
                print(f"Downloaded {completed}/{total} tiles", flush=True)
        if batch:
            connection.executemany(
                "INSERT OR REPLACE INTO tiles VALUES (?, ?, ?, ?)", batch
            )
            connection.commit()
    connection.close()

    if args.output.exists():
        args.output.unlink()
    subprocess.run(
        [str(args.pmtiles), "convert", str(mbtiles), str(args.output)], check=True
    )
    print(f"Created {args.output} ({args.output.stat().st_size / 1024**3:.2f} GiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
