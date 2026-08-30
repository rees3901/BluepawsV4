#!/usr/bin/env python3
"""Render the BluePaws offline-map tile pack with an installed QGIS LTR.

Run this script through QGIS's ``python-qgis-ltr.bat`` wrapper so the PyQGIS
modules and native processing algorithms are available. It accepts either a
local vector MBTiles file or an XYZ vector-tile URL, plus a QGIS QML or
Mapbox/MapLibre JSON style.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path

# The Windows QGIS installer can render without a visible window. Forcing Qt's
# offscreen platform there prevents it from discovering installed system fonts
# and turns every map label into missing-glyph boxes. Retain offscreen mode for
# genuinely headless non-Windows builders only.
if sys.platform != "win32":
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
    QgsMapBoxGlStyleConverter,
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

# A practical on-device pack for the first layer-switching build: Gloucester,
# its immediate suburbs and approaches at every firmware-supported zoom. This
# is deliberately much smaller than the county profile because FAT32 wastes a
# full allocation unit for every small JPEG tile on the test card.
GLOUCESTER_PASSES = (
    RenderPass("Gloucester city", (-2.36, 51.78, -2.10, 51.96), 10, 17),
)

# Coarse Great Britain coverage plus a roughly 40 km square around Gloucester.
# OS Open Zoomstack does not include Northern Ireland.
PILOT_PASSES = (
    RenderPass("Great Britain overview", (-8.82, 49.79, 1.92, 60.95), 5, 11),
    RenderPass("Gloucester high detail", (-2.527, 51.684, -1.949, 52.044), 12, 17),
)

# County-wide navigation through z16, with z17 detail around the Gloucester /
# Cheltenham urban corridor. This provides substantially more room to pan while
# keeping the first OSM-style pack comfortably below the FAT32 card budget.
GLOUCESTERSHIRE_PASSES = (
    RenderPass("Gloucestershire", (-2.72, 51.55, -1.62, 52.15), 10, 16),
    RenderPass("Gloucester urban detail", (-2.42, 51.75, -1.98, 52.00), 17, 17),
)

# The nationwide part is rendered from a deliberately shallow PMTiles extract.
# Keeping it as a separate profile lets it be merged with the more detailed
# Gloucestershire render without downloading a Great Britain z17 archive.
GREAT_BRITAIN_OVERVIEW_PASSES = (
    RenderPass("Great Britain overview", (-8.82, 49.79, 1.92, 60.95), 5, 11),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render vector maps into BluePaws 256px JPEG XYZ tiles."
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--mbtiles", type=Path, help="Local vector MBTiles source")
    source.add_argument("--tile-url", help="XYZ vector URL containing {z}, {x}, and {y}")
    style = parser.add_mutually_exclusive_group(required=True)
    style.add_argument("--style", type=Path, help="QGIS vector-tile QML style")
    style.add_argument("--mapbox-style", type=Path, help="Mapbox/MapLibre JSON style")
    parser.add_argument("--output", type=Path, required=True, help="Destination tile directory")
    parser.add_argument(
        "--profile",
        choices=("fixture", "gloucester", "pilot", "great-britain-overview", "gloucestershire"),
        default="fixture",
        help=(
            "fixture is fast; gloucester is the city test pack; pilot uses OS GB coverage; great-britain-overview "
            "renders the shallow OSM UK extract; gloucestershire adds local detail"
        ),
    )
    parser.add_argument("--quality", type=int, default=85, choices=range(40, 96))
    parser.add_argument("--manifest", type=Path, help="Optional manifest output path")
    return parser.parse_args()


def validate_inputs(args: argparse.Namespace) -> None:
    if args.mbtiles is not None and not args.mbtiles.is_file():
        raise ValueError(f"MBTiles input does not exist: {args.mbtiles}")
    if args.style is not None and not args.style.is_file():
        raise ValueError(f"QGIS style does not exist: {args.style}")
    if args.mapbox_style is not None and not args.mapbox_style.is_file():
        raise ValueError(f"Mapbox style does not exist: {args.mapbox_style}")
    if args.tile_url is not None and not all(token in args.tile_url for token in ("{z}", "{x}", "{y}")):
        raise ValueError("Tile URL must contain {z}, {x}, and {y}")
    if args.output.exists() and any(args.output.iterdir()):
        raise ValueError(f"Output directory is not empty: {args.output}")
    if args.manifest and args.manifest.exists():
        raise ValueError(f"Manifest already exists: {args.manifest}")


def vector_tile_source(path: Path | None, tile_url: str | None) -> str:
    uri = QgsDataSourceUri()
    if path is not None:
        uri.setParam("type", "mbtiles")
        uri.setParam("url", str(path.resolve()))
    else:
        uri.setParam("type", "xyz")
        uri.setParam("url", tile_url)
        uri.setParam("zmin", "0")
        uri.setParam("zmax", "15")
    encoded = uri.encodedUri()
    return bytes(encoded).decode("utf-8")


def create_project(args: argparse.Namespace) -> QgsProject:
    project = QgsProject.instance()
    project.clear()
    project.setCrs(QgsCoordinateReferenceSystem("EPSG:3857"))

    layer = QgsVectorTileLayer(vector_tile_source(args.mbtiles, args.tile_url), "BluePaws basemap")
    if not layer.isValid():
        raise RuntimeError("QGIS could not open the vector-tile source")

    if args.style is not None:
        style_message, style_ok = layer.loadNamedStyle(str(args.style.resolve()))
        if not style_ok:
            raise RuntimeError(f"QGIS could not load style {args.style}: {style_message}")
    else:
        style_json = args.mapbox_style.read_text(encoding="utf-8")
        converter = QgsMapBoxGlStyleConverter()
        if converter.convert(style_json) != QgsMapBoxGlStyleConverter.Success:
            raise RuntimeError(f"QGIS could not convert Mapbox style: {converter.errorMessage()}")
        renderer = converter.renderer()
        if renderer is None:
            raise RuntimeError("Converted Mapbox style did not contain a vector renderer")
        layer.setRenderer(renderer)
        labeling = converter.labeling()
        if labeling is not None:
            layer.setLabeling(labeling)
            layer.setLabelsEnabled(True)
        for warning in converter.warnings():
            print(f"style warning: {warning}", flush=True)
    project.addMapLayer(layer)
    return project


def render_pass(
    render: RenderPass,
    output: Path,
    quality: int,
    context: QgsProcessingContext,
    feedback: QgsProcessingFeedback,
    osm_attribution: bool,
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
            "BACKGROUND_COLOR": "#F2EFE9",
            "ANTIALIAS": True,
            "TILE_FORMAT": 1,
            "QUALITY": quality,
            "METATILESIZE": 4,
            "TILE_WIDTH": 256,
            "TILE_HEIGHT": 256,
            "TMS_CONVENTION": False,
            "HTML_TITLE": "BluePaws Gloucester offline map",
            "HTML_ATTRIBUTION": (
                "© OpenStreetMap contributors"
                if osm_attribution
                else "Contains OS data © Crown copyright and database right 2026"
            ),
            "HTML_OSM": osm_attribution,
            "OUTPUT_DIRECTORY": str(output),
        },
        context=context,
        feedback=feedback,
        is_child_algorithm=False,
    )


def write_manifest(
    path: Path,
    output: Path,
    profile: str,
    quality: int,
    osm_attribution: bool,
) -> None:
    maximum_zoom = 11 if profile == "great-britain-overview" else 17
    if profile == "fixture":
        bounds = {
            "west": -2.285,
            "south": 51.837,
            "east": -2.191,
            "north": 51.891,
            "min_zoom": 12,
        }
    elif profile == "gloucester":
        bounds = {
            "west": -2.36,
            "south": 51.78,
            "east": -2.10,
            "north": 51.96,
            "min_zoom": 10,
        }
    elif profile == "pilot":
        bounds = {
            "west": -2.527,
            "south": 51.684,
            "east": -1.949,
            "north": 52.044,
            "min_zoom": 12,
        }
    elif profile == "gloucestershire":
        bounds = {
            "west": -2.72,
            "south": 51.55,
            "east": -1.62,
            "north": 52.15,
            "min_zoom": 10,
        }
    else:
        bounds = {
            "west": -8.82,
            "south": 49.79,
            "east": 1.92,
            "north": 60.95,
            "min_zoom": 5,
        }
    tile_files = list(output.rglob("*.jpg"))
    manifest = {
        "schema_version": 1,
        "name": f"BluePaws {profile}",
        "version": "2026-08",
        "projection": "EPSG:3857",
        "tile_scheme": "xyz",
        "tile_size": 256,
        "min_zoom": (
            5
            if profile in ("pilot", "great-britain-overview")
            else (10 if profile in ("gloucester", "gloucestershire") else 12)
        ),
        "max_zoom": maximum_zoom,
        "format": "jpg",
        "jpeg_quality": quality,
        "tile_path": "tiles/{z}/{x}/{y}.jpg",
        "tile_count": len(tile_files),
        "payload_bytes": sum(tile.stat().st_size for tile in tile_files),
        "center": {"latitude": 51.8642, "longitude": -2.2382, "zoom": 14},
        "high_detail_bounds": bounds,
        "attribution": (
            "© OpenStreetMap contributors"
            if osm_attribution
            else "Contains OS data © Crown copyright and database right 2026"
        ),
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
        project = create_project(args)
        context = QgsProcessingContext()
        context.setProject(project)
        feedback = QgsProcessingFeedback()
        args.output.mkdir(parents=True, exist_ok=True)
        passes = {
            "fixture": FIXTURE_PASSES,
            "gloucester": GLOUCESTER_PASSES,
            "pilot": PILOT_PASSES,
            "great-britain-overview": GREAT_BRITAIN_OVERVIEW_PASSES,
            "gloucestershire": GLOUCESTERSHIRE_PASSES,
        }[args.profile]
        for render in passes:
            render_pass(
                render,
                args.output,
                args.quality,
                context,
                feedback,
                osm_attribution=args.tile_url is not None,
            )
        if args.manifest:
            write_manifest(
                args.manifest,
                args.output,
                args.profile,
                args.quality,
                osm_attribution=args.tile_url is not None,
            )
        print(f"Tile rendering complete: {args.output}", flush=True)
        return 0
    finally:
        QgsProject.instance().clear()
        qgis.exitQgis()


if __name__ == "__main__":
    raise SystemExit(main())
