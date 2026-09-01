#!/usr/bin/env python3
"""Build a coherent offline XYZ layer from Environment Agency aerial surveys.

Run this through the QGIS Python environment so GDAL's ECW driver is available.
The builder deliberately accepts one survey year/product at a time: mixing
surveys is how visible seams and misaligned-looking tile boundaries arise.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

from osgeo import gdal
from PIL import Image


EA_TILE_URL = (
    "https://environment.data.gov.uk/tiles/collections/survey/"
    "{product}/{year}/{resolution}/{block}?subscription-key=dspui"
)
DEFAULT_BLOCKS = (
    "SO8015",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Download and render one coherent EA aerial survey"
    )
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--year", default="2007")
    parser.add_argument("--resolution", default="0.2")
    parser.add_argument(
        "--product", default="vertical_aerial_photography_tiles_irrgb"
    )
    parser.add_argument("--blocks", nargs="+", default=list(DEFAULT_BLOCKS))
    parser.add_argument("--min-zoom", type=int, default=14)
    parser.add_argument("--max-zoom", type=int, default=17)
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--jpeg-quality", type=int, default=88)
    return parser.parse_args()


def package_path(args: argparse.Namespace, block: str) -> Path:
    millimetres = round(float(args.resolution) * 1000)
    return args.work / "packages" / f"ea-{args.year}-{millimetres}mm-{block}.zip"


def download_package(args: argparse.Namespace, block: str) -> Path:
    destination = package_path(args, block)
    if destination.is_file() and zipfile.is_zipfile(destination):
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(".zip.part")
    url = EA_TILE_URL.format(
        product=args.product,
        year=args.year,
        resolution=args.resolution,
        block=block,
    )
    request = urllib.request.Request(
        url, headers={"User-Agent": "BluePaws-EA-aerial-builder/1.0"}
    )
    last_error: Exception | None = None
    for attempt in range(4):
        try:
            print(f"Downloading EA block {block} (attempt {attempt + 1})", flush=True)
            with urllib.request.urlopen(request, timeout=600) as response, temporary.open(
                "wb"
            ) as output:
                shutil.copyfileobj(response, output, length=1024 * 1024)
            break
        except (OSError, urllib.error.URLError) as error:
            last_error = error
            temporary.unlink(missing_ok=True)
            if attempt == 3:
                raise RuntimeError(f"EA download failed for block {block}: {error}") from error
            time.sleep(3 * (attempt + 1))
    if last_error is not None:
        print(f"EA block {block} recovered after: {last_error}", flush=True)
    if not zipfile.is_zipfile(temporary):
        temporary.unlink(missing_ok=True)
        raise RuntimeError(f"EA response for {block} was not a ZIP archive")
    os.replace(temporary, destination)
    return destination


def extract_package(args: argparse.Namespace, archive: Path) -> list[Path]:
    destination = args.work / "sources" / archive.stem
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as package:
        for member in package.infolist():
            if Path(member.filename).suffix.lower() == ".ecw":
                target = destination / Path(member.filename).name
                if not target.is_file() or target.stat().st_size != member.file_size:
                    with package.open(member) as source, target.open("wb") as output:
                        shutil.copyfileobj(source, output, length=1024 * 1024)
    return sorted(destination.glob("*.ecw"))


def build_vrt(sources: list[Path], destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    options = gdal.BuildVRTOptions(
        bandList=[1, 2, 3],
        srcNodata="0 0 0",
        VRTNodata="0 0 0",
        outputSRS="EPSG:27700",
        resolution="highest",
    )
    dataset = gdal.BuildVRT(str(destination), [str(path) for path in sources], options=options)
    if dataset is None:
        raise RuntimeError("GDAL could not create the EA survey mosaic")
    dataset.FlushCache()
    dataset = None


def render_png_tiles(args: argparse.Namespace, vrt: Path, destination: Path) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    command = [
        sys.executable,
        "-m",
        "osgeo_utils.gdal2tiles",
        "--xyz",
        "--exclude",
        "--webviewer=none",
        "--resampling=bilinear",
        f"--processes={max(1, args.workers)}",
        f"--zoom={args.min_zoom}-{args.max_zoom}",
        "--tiledriver=PNG",
        str(vrt),
        str(destination),
    ]
    subprocess.run(command, check=True)


def convert_tile(source: Path, png_root: Path, jpg_root: Path, quality: int) -> int:
    relative = source.relative_to(png_root).with_suffix(".jpg")
    destination = jpg_root / relative
    with Image.open(source) as tile:
        rgba = tile.convert("RGBA")
        alpha = rgba.getchannel("A")
        if alpha.getbbox() is None:
            return 0
        # Keep survey-edge pixels visibly neutral instead of filling them with
        # a second imagery source. This prevents the two-provider seams that
        # made the earlier composite unusable.
        background = Image.new("RGB", rgba.size, (30, 36, 42))
        background.paste(rgba.convert("RGB"), mask=alpha)
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_suffix(".jpg.part")
        background.save(temporary, format="JPEG", quality=quality, optimize=True)
        os.replace(temporary, destination)
    return destination.stat().st_size


def convert_tiles(args: argparse.Namespace, png_root: Path) -> tuple[int, int]:
    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)
    sources = sorted(png_root.rglob("*.png"))
    total_bytes = 0
    count = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [
            pool.submit(
                convert_tile, source, png_root, args.output, args.jpeg_quality
            )
            for source in sources
        ]
        for index, future in enumerate(concurrent.futures.as_completed(futures), 1):
            size = future.result()
            if size:
                count += 1
                total_bytes += size
            if index % 2000 == 0 or index == len(futures):
                print(f"Converted {index}/{len(futures)} PNG tiles", flush=True)
    return count, total_bytes


def write_manifest(args: argparse.Namespace, count: int, payload_bytes: int) -> None:
    manifest = {
        "schema_version": 1,
        "name": f"Environment Agency {args.year} Gloucester aerial",
        "version": f"EA-{args.year}",
        "projection": "EPSG:3857",
        "tile_scheme": "xyz",
        "tile_size": 256,
        "min_zoom": args.min_zoom,
        "max_zoom": args.max_zoom,
        "format": "jpg",
        "tile_path": "tiles/{z}/{x}/{y}.jpg",
        "center": {"latitude": 51.8642, "longitude": -2.2382, "zoom": 17},
        "coverage_blocks": args.blocks,
        "source_product": args.product,
        "source_resolution_metres": float(args.resolution),
        "tile_count": count,
        "payload_bytes": payload_bytes,
        "attribution": "Environment Agency copyright and/or database right; Open Government Licence v3.0",
        "source_url": "https://environment.data.gov.uk/dataset/dae203a8-ba24-4c54-bab0-866b9faadb58",
        "note": "One survey year only; uncovered survey edges are neutral, never filled from another imagery provider.",
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    gdal.UseExceptions()
    print(f"Downloading {len(args.blocks)} coherent EA survey blocks", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        archives = list(pool.map(lambda block: download_package(args, block), args.blocks))
    sources: list[Path] = []
    for archive in archives:
        sources.extend(extract_package(args, archive))
    if not sources:
        raise SystemExit("No ECW sources were found in the downloaded packages")
    print(f"Mosaicking {len(sources)} ECW source images", flush=True)
    vrt = args.work / f"ea-{args.year}-mosaic.vrt"
    build_vrt(sources, vrt)
    png_root = args.work / "rendered-png"
    render_png_tiles(args, vrt, png_root)
    count, payload_bytes = convert_tiles(args, png_root)
    write_manifest(args, count, payload_bytes)
    digest = hashlib.sha256(args.manifest.read_bytes()).hexdigest().upper()
    print(f"EA aerial pack ready: {count} JPEG tiles, {payload_bytes} bytes")
    print(f"Manifest SHA-256: {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
