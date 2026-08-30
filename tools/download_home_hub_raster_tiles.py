#!/usr/bin/env python3
"""Download a bounded, licensed raster layer into BluePaws XYZ layout.

This is intentionally a region/profile downloader rather than a general tile
scraper.  Only use it with a provider that explicitly permits offline bulk
downloads, and keep the provider attribution in the generated manifest.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class DownloadPass:
    name: str
    bounds: tuple[float, float, float, float]
    minimum_zoom: int
    maximum_zoom: int


SATELLITE_PASSES = (
    DownloadPass("Great Britain overview", (-8.82, 49.79, 1.92, 60.95), 5, 11),
    DownloadPass("Gloucestershire detail", (-2.72, 51.55, -1.62, 52.15), 12, 14),
)

FULL_GB_PASSES = (
    DownloadPass("Great Britain complete", (-8.82, 49.79, 1.92, 60.95), 5, 14),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Download licensed JPEG XYZ map tiles")
    parser.add_argument(
        "--url-template",
        required=True,
        help="Provider URL containing {z}, {x}, and {y}; {row}/{col} aliases are accepted",
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--attribution", required=True)
    parser.add_argument("--source-url", required=True)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--retries", type=int, default=3)
    parser.add_argument(
        "--profile",
        choices=("gloucestershire", "great-britain"),
        default="gloucestershire",
        help="Coverage profile; great-britain extends genuine Sentinel detail through z14",
    )
    return parser.parse_args()


def longitude_to_x(longitude: float, zoom: int) -> int:
    return int((longitude + 180.0) / 360.0 * (1 << zoom))


def latitude_to_y(latitude: float, zoom: int) -> int:
    latitude = max(-85.05112878, min(85.05112878, latitude))
    value = math.asinh(math.tan(math.radians(latitude)))
    return int((1.0 - value / math.pi) / 2.0 * (1 << zoom))


def enumerate_tiles(profile: str = "gloucestershire") -> list[tuple[int, int, int]]:
    tiles: set[tuple[int, int, int]] = set()
    passes = FULL_GB_PASSES if profile == "great-britain" else SATELLITE_PASSES
    for render in passes:
        west, south, east, north = render.bounds
        for zoom in range(render.minimum_zoom, render.maximum_zoom + 1):
            x_min = longitude_to_x(west, zoom)
            x_max = longitude_to_x(east, zoom)
            y_min = latitude_to_y(north, zoom)
            y_max = latitude_to_y(south, zoom)
            for x in range(x_min, x_max + 1):
                for y in range(y_min, y_max + 1):
                    tiles.add((zoom, x, y))
    return sorted(tiles)


def tile_url(template: str, zoom: int, x: int, y: int) -> str:
    return template.format(z=zoom, x=x, y=y, col=x, row=y)


def download_one(
    tile: tuple[int, int, int], args: argparse.Namespace
) -> tuple[str, int, str]:
    zoom, x, y = tile
    destination = args.output / str(zoom) / str(x) / f"{y}.jpg"
    empty_marker = destination.with_suffix(".empty")
    if destination.is_file() and destination.stat().st_size > 4:
        with destination.open("rb") as existing:
            if existing.read(2) == b"\xff\xd8":
                return "skipped", destination.stat().st_size, str(destination)
    if empty_marker.is_file():
        return "empty", 0, str(destination)

    request = urllib.request.Request(
        tile_url(args.url_template, zoom, x, y),
        headers={"User-Agent": "BluePaws-offline-map-builder/1.0"},
    )
    last_error: Exception | None = None
    for attempt in range(args.retries):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = response.read()
            # EOX responds with a tiny, all-empty paletted PNG outside the
            # Sentinel mosaic footprint even when the requested extension is
            # JPEG. Preserve that distinction locally without putting dummy
            # image files in the firmware's tile tree.
            if payload.startswith(b"\x89PNG\r\n\x1a\n") and len(payload) <= 512:
                destination.parent.mkdir(parents=True, exist_ok=True)
                empty_marker.write_text("provider returned an empty tile\n", encoding="ascii")
                return "empty", 0, str(destination)
            if not payload.startswith(b"\xff\xd8"):
                raise ValueError("provider response was not a JPEG")
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_suffix(".jpg.part")
            temporary.write_bytes(payload)
            os.replace(temporary, destination)
            return "downloaded", len(payload), str(destination)
        except (OSError, ValueError, urllib.error.URLError) as error:
            last_error = error
            time.sleep(0.5 * (attempt + 1))
    return "failed", 0, f"{destination}: {last_error}"


def write_manifest(args: argparse.Namespace, count: int, total_bytes: int) -> None:
    manifest = {
        "schema_version": 1,
        "name": args.name,
        "version": "2026-08",
        "projection": "EPSG:3857",
        "tile_scheme": "xyz",
        "tile_size": 256,
        "min_zoom": 5,
        "max_zoom": 14,
        "format": "jpg",
        "tile_path": "tiles/{z}/{x}/{y}.jpg",
        "center": {"latitude": 51.8642, "longitude": -2.2382, "zoom": 13},
        "high_detail_bounds": {
            "west": -8.82 if args.profile == "great-britain" else -2.72,
            "south": 49.79 if args.profile == "great-britain" else 51.55,
            "east": 1.92 if args.profile == "great-britain" else -1.62,
            "north": 60.95 if args.profile == "great-britain" else 52.15,
            "min_zoom": 12,
        },
        "coverage_profile": args.profile,
        "tile_count": count,
        "payload_bytes": total_bytes,
        "attribution": args.attribution,
        "source_url": args.source_url,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    if "{z}" not in args.url_template:
        raise SystemExit("URL template must contain {z}")
    if "{x}" not in args.url_template and "{col}" not in args.url_template:
        raise SystemExit("URL template must contain {x} or {col}")
    if "{y}" not in args.url_template and "{row}" not in args.url_template:
        raise SystemExit("URL template must contain {y} or {row}")

    tiles = enumerate_tiles(args.profile)
    print(f"Preparing {len(tiles)} bounded tiles for {args.profile}", flush=True)
    totals = {"downloaded": 0, "skipped": 0, "empty": 0, "failed": 0}
    total_bytes = 0
    failures: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(download_one, tile, args) for tile in tiles]
        for index, future in enumerate(concurrent.futures.as_completed(futures), 1):
            status, size, detail = future.result()
            totals[status] += 1
            total_bytes += size
            if status == "failed":
                failures.append(detail)
            if index % 500 == 0 or index == len(futures):
                print(f"Completed {index}/{len(futures)}", flush=True)

    if failures:
        for failure in failures[:20]:
            print(f"failed: {failure}")
        print(f"Download incomplete: {len(failures)} failures")
        return 1

    jpeg_files = list(args.output.rglob("*.jpg"))
    total_bytes = sum(path.stat().st_size for path in jpeg_files)
    write_manifest(args, len(jpeg_files), total_bytes)
    digest = hashlib.sha256(args.manifest.read_bytes()).hexdigest().upper()
    print(f"Tiles ready: {totals}; payload bytes={total_bytes}")
    print(f"Manifest SHA-256: {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
