#!/usr/bin/env python3
"""Render the BluePaws offline-map tile pack with an installed QGIS LTR.

Run this script through QGIS's ``python-qgis-ltr.bat`` wrapper so the PyQGIS
modules and native processing algorithms are available. The input is the
official OS Open Zoomstack vector MBTiles file and its QGIS QML stylesheet.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

# The Windows QGIS Python wrapper adds the core bindings but not the bundled
# Processing plugin directory to sys.path.
qgis_prefix = os.environ.get("QGIS_PREFIX_PATH")
if qgis_prefix:
    processing_plugins = Path(qgis_prefix) / "python" / "plugins"
    if processing_plugins.is_dir():
        sys.path.insert(0, str(processing_plugins))

from qgis.core import (  # noqa: E402
    QgsApplication,
    QgsCoordinateReferenceSystem,
    QgsDataSourceUri,
    QgsProcessingContext,
    QgsProcessingFeedback,
    QgsProject,
    QgsVectorTileLayer,
)
from processing.core.Processing import Processing  # noqa: E402
import processing  # noqa: E402


@dataclass(frozen=True)
class RenderPass:
    name: str
    bounds_wgs84: tuple[float, float, float, float]
    minimum_zoom: int
    maximum_zoom: int


# Small enough to render rapidly, but wide enough to exercise panning and
# multiple tile loads around central Gloucester.
FIXTURE_PASSES = (
    RenderPass("Gloucester fixture", (-2.285, 51.837, -2.191, 51.891), 12, 17),
)

# Coarse Great Britain coverage plus a roughly 40 km square around Gloucester.
# OS Open Zoomstack does not include Northern Ireland.
PILOT_PASSES = (
    RenderPass("Great Britain overview", (-8.82, 49.79, 1.92, 60.95), 5, 11),
    RenderPass("Gloucester high detail", (-2.527, 51.684, -1.949, 52.044), 12, 17),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render OS Open Zoomstack into BluePaws 256px JPEG XYZ tiles."
    )
    parser.add_argument("--mbtiles", type=Path, required=True, help="OS_Open_Zoomstack.mbtiles")
    parser.add_argument("--style", type=Path, required=True, help="OS vector-tile QML style")
    parser.add_argument("--output", type=Path, required=True, help="Destination tile directory")
    parser.add_argument(
        "--profile",
        choices=("fixture", "pilot"),
        default="fixture",
        help="fixture is fast; pilot adds GB z5-11 and Gloucester z12-17",
    )
    parser.add_argument("--quality", type=int, default=85, choices=range(40, 96))
    parser.add_argument("--manifest", type=Path, help="Optional manifest output path")
    return parser.parse_args()


def validate_inputs(args: argparse.Namespace) -> None:
    if not args.mbtiles.is_file():
        raise ValueError(f"MBTiles input does not exist: {args.mbtiles}")
    if not args.style.is_file():
        raise ValueError(f"QGIS style does not exist: {args.style}")
    if args.output.exists() and any(args.output.iterdir()):
        raise ValueError(f"Output directory is not empty: {args.output}")
    if args.manifest and args.manifest.exists():
        raise ValueError(f"Manifest already exists: {args.manifest}")


def vector_tile_source(path: Path) -> str:
    uri = QgsDataSourceUri()
    uri.setParam("type", "mbtiles")
    uri.setParam("url", str(path.resolve()))
    encoded = uri.encodedUri()
    return bytes(encoded).decode("utf-8")


def create_project(mbtiles: Path, style: Path) -> QgsProject:
    project = QgsProject.instance()
    project.clear()
    project.setCrs(QgsCoordinateReferenceSystem("EPSG:3857"))

    layer = QgsVectorTileLayer(vector_tile_source(mbtiles), "OS Open Zoomstack")
    if not layer.isValid():
        raise RuntimeError(f"QGIS could not open vector MBTiles: {mbtiles}")

    style_message, style_ok = layer.loadNamedStyle(str(style.resolve()))
    if not style_ok:
        raise RuntimeError(f"QGIS could not load style {style}: {style_message}")
    project.addMapLayer(layer)
    return project


def render_pass(
    render: RenderPass,
    output: Path,
    quality: int,
    context: QgsProcessingContext,
    feedback: QgsProcessingFeedback,
) -> None:
    west, south, east, north = render.bounds_wgs84
    extent = f"{west},{east},{south},{north} [EPSG:4326]"
    print(
        f"Rendering {render.name}: z{render.minimum_zoom}-{render.maximum_zoom} "
        f"within {west},{south},{east},{north}",
        flush=True,
    )
    processing.run(
        "native:tilesxyzdirectory",
        {
            "EXTENT": extent,
            "ZOOM_MIN": render.minimum_zoom,
            "ZOOM_MAX": render.maximum_zoom,
            "DPI": 96,
            "BACKGROUND_COLOR": "#A9DDEF",
            "ANTIALIAS": True,
            "TILE_FORMAT": 1,
            "QUALITY": quality,
            "METATILESIZE": 4,
            "TILE_WIDTH": 256,
            "TILE_HEIGHT": 256,
            "TMS_CONVENTION": False,
            "HTML_TITLE": "BluePaws Gloucester offline map",
            "HTML_ATTRIBUTION": "Contains OS data © Crown copyright and database right 2026",
            "HTML_OSM": False,
            "OUTPUT_DIRECTORY": str(output),
        },
        context=context,
        feedback=feedback,
        is_child_algorithm=False,
    )


def write_manifest(path: Path, profile: str, quality: int) -> None:
    maximum_zoom = 17
    if profile == "fixture":
        bounds = {
            "west": -2.285,
            "south": 51.837,
            "east": -2.191,
            "north": 51.891,
            "min_zoom": 12,
        }
    else:
        bounds = {
            "west": -2.527,
            "south": 51.684,
            "east": -1.949,
            "north": 52.044,
            "min_zoom": 12,
        }
    manifest = {
        "schema_version": 1,
        "name": f"BluePaws Gloucester {profile}",
        "version": "2026-08",
        "projection": "EPSG:3857",
        "tile_scheme": "xyz",
        "tile_size": 256,
        "min_zoom": 5 if profile == "pilot" else 12,
        "max_zoom": maximum_zoom,
        "format": "jpg",
        "jpeg_quality": quality,
        "tile_path": "tiles/{z}/{x}/{y}.jpg",
        "center": {"latitude": 51.8642, "longitude": -2.2382, "zoom": 14},
        "high_detail_bounds": bounds,
        "attribution": "Contains OS data © Crown copyright and database right 2026",
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    try:
        validate_inputs(args)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    qgis = QgsApplication([], False)
    qgis.initQgis()
    Processing.initialize()
    try:
        project = create_project(args.mbtiles, args.style)
        context = QgsProcessingContext()
        context.setProject(project)
        feedback = QgsProcessingFeedback()
        args.output.mkdir(parents=True, exist_ok=True)
        passes = FIXTURE_PASSES if args.profile == "fixture" else PILOT_PASSES
        for render in passes:
            render_pass(render, args.output, args.quality, context, feedback)
        if args.manifest:
            write_manifest(args.manifest, args.profile, args.quality)
        print(f"Tile rendering complete: {args.output}", flush=True)
        return 0
    finally:
        QgsProject.instance().clear()
        qgis.exitQgis()


if __name__ == "__main__":
    raise SystemExit(main())
