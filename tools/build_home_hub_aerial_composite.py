#!/usr/bin/env python3
"""Fill partial Environment Agency aerial coverage with Sentinel imagery.

Run through QGIS's ``python-qgis-ltr.bat`` wrapper. The input and output tile
layout is standard ``{z}/{x}/{y}.jpg`` XYZ. Aerial pixels which are absent or
JPEG-compressed near black are replaced with the matching Sentinel pixel.
Sentinel z14 parents are cropped and scaled for z15-z17 fallback coverage.
"""

from __future__ import annotations

import argparse
import json
import math
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from qgis.PyQt.QtCore import QRect, Qt
from qgis.PyQt.QtGui import QImage


TILE_SIZE = 256
SATELLITE_MAX_ZOOM = 14


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aerial", type=Path, required=True, help="EA aerial tile root")
    parser.add_argument("--satellite", type=Path, required=True, help="Sentinel tile root")
    parser.add_argument("--output", type=Path, required=True, help="Composite tile root")
    parser.add_argument("--aerial-manifest", type=Path, required=True)
    parser.add_argument("--output-manifest", type=Path, required=True)
    parser.add_argument("--quality", type=int, default=85, choices=range(60, 96))
    parser.add_argument("--black-threshold", type=int, default=18, choices=range(0, 65))
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    return parser.parse_args()


def longitude_to_x(longitude: float, zoom: int) -> int:
    return math.floor((longitude + 180.0) / 360.0 * (1 << zoom))


def latitude_to_y(latitude: float, zoom: int) -> int:
    latitude = max(-85.05112878, min(85.05112878, latitude))
    radians = math.radians(latitude)
    return math.floor(
        (1.0 - math.log(math.tan(radians) + 1.0 / math.cos(radians)) / math.pi)
        / 2.0
        * (1 << zoom)
    )


def rgb_array(image: QImage) -> np.ndarray:
    converted = image.convertToFormat(QImage.Format_RGB888)
    pointer = converted.bits()
    pointer.setsize(converted.sizeInBytes())
    rows = np.frombuffer(pointer, dtype=np.uint8).reshape(converted.height(), converted.bytesPerLine())
    return rows[:, : converted.width() * 3].reshape(converted.height(), converted.width(), 3).copy()


def load_satellite(root: Path, zoom: int, x: int, y: int) -> QImage:
    source_zoom = min(zoom, SATELLITE_MAX_ZOOM)
    scale = 1 << (zoom - source_zoom)
    parent_x = x // scale
    parent_y = y // scale
    image = QImage(str(root / str(source_zoom) / str(parent_x) / f"{parent_y}.jpg"))
    if image.isNull():
        raise RuntimeError(f"Missing Sentinel fallback tile z{source_zoom}/{parent_x}/{parent_y}")
    if scale == 1:
        return image.scaled(TILE_SIZE, TILE_SIZE, Qt.IgnoreAspectRatio, Qt.SmoothTransformation)

    crop_size = TILE_SIZE // scale
    crop_x = (x % scale) * crop_size
    crop_y = (y % scale) * crop_size
    return image.copy(QRect(crop_x, crop_y, crop_size, crop_size)).scaled(
        TILE_SIZE, TILE_SIZE, Qt.IgnoreAspectRatio, Qt.SmoothTransformation
    )


def save_rgb(array: np.ndarray, path: Path, quality: int) -> None:
    contiguous = np.ascontiguousarray(array)
    image = QImage(
        contiguous.data,
        TILE_SIZE,
        TILE_SIZE,
        int(contiguous.strides[0]),
        QImage.Format_RGB888,
    ).copy()
    path.parent.mkdir(parents=True, exist_ok=True)
    if not image.save(str(path), "JPG", quality):
        raise RuntimeError(f"Could not save {path}")


def main() -> int:
    args = parse_args()
    manifest = json.loads(args.aerial_manifest.read_text(encoding="utf-8"))
    bounds = manifest["coverage_bounds"]
    jobs: list[tuple[int, int, int]] = []
    for zoom in range(int(manifest["min_zoom"]), int(manifest["max_zoom"]) + 1):
        first_x = longitude_to_x(float(bounds["west"]), zoom)
        last_x = longitude_to_x(float(bounds["east"]), zoom)
        first_y = latitude_to_y(float(bounds["north"]), zoom)
        last_y = latitude_to_y(float(bounds["south"]), zoom)
        jobs.extend(
            (zoom, x, y)
            for x in range(first_x, last_x + 1)
            for y in range(first_y, last_y + 1)
        )

    if args.output.exists() and any(args.output.iterdir()):
        raise RuntimeError(f"Output directory is not empty: {args.output}")
    args.output.mkdir(parents=True, exist_ok=True)

    def convert(tile: tuple[int, int, int]) -> None:
        zoom, x, y = tile
        satellite = rgb_array(load_satellite(args.satellite, zoom, x, y))
        aerial_path = args.aerial / str(zoom) / str(x) / f"{y}.jpg"
        if aerial_path.is_file():
            aerial_image = QImage(str(aerial_path))
            if aerial_image.isNull():
                raise RuntimeError(f"Could not read {aerial_path}")
            aerial = rgb_array(aerial_image.scaled(
                TILE_SIZE, TILE_SIZE, Qt.IgnoreAspectRatio, Qt.SmoothTransformation
            ))
            no_data = np.max(aerial, axis=2) <= args.black_threshold
            aerial[no_data] = satellite[no_data]
            result = aerial
        else:
            result = satellite
        save_rgb(result, args.output / str(zoom) / str(x) / f"{y}.jpg", args.quality)

    completed = 0
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for _ in pool.map(convert, jobs):
            completed += 1
            if completed % 500 == 0 or completed == len(jobs):
                print(f"Composite tiles: {completed}/{len(jobs)}", flush=True)

    output_files = list(args.output.rglob("*.jpg"))
    manifest.update(
        {
            "name": "BluePaws EA aerial with Sentinel gap fill",
            "version": "2026-08-30-composite",
            "jpeg_quality": args.quality,
            "tile_count": len(output_files),
            "payload_bytes": sum(path.stat().st_size for path in output_files),
            "attribution": (
                "Environment Agency © copyright and/or database right 2022; "
                "Sentinel-2 cloudless by EOX IT Services GmbH, CC BY 4.0"
            ),
            "source": "EA 2012/2014 survey mosaic over Sentinel-2 cloudless 2016/2017",
            "licence": "Open Government Licence; CC BY 4.0",
            "note": "Sentinel fills missing tiles and black/no-data pixels in the partial EA surveys.",
        }
    )
    args.output_manifest.parent.mkdir(parents=True, exist_ok=True)
    args.output_manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(output_files)} tiles to {args.output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
