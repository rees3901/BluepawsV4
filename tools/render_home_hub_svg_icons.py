#!/usr/bin/env python3
"""Render the two supplied SVG metadata icons through QGIS' Qt runtime."""

from __future__ import annotations

import argparse
from pathlib import Path

from qgis.PyQt.QtCore import QByteArray, Qt
from qgis.PyQt.QtGui import QImage, QPainter
from qgis.PyQt.QtSvg import QSvgRenderer


def render(source: Path, destination: Path, width: int, height: int) -> None:
    renderer = QSvgRenderer(QByteArray(source.read_bytes()))
    if not renderer.isValid():
        raise ValueError(f"Invalid SVG: {source}")
    image = QImage(width, height, QImage.Format_ARGB32)
    image.fill(Qt.transparent)
    painter = QPainter(image)
    renderer.render(painter)
    painter.end()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not image.save(str(destination), "PNG"):
        raise OSError(f"Could not write {destination}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--assets",
        type=Path,
        default=Path("home-hub/firmware/assets/icons"),
    )
    args = parser.parse_args()
    assets = args.assets.resolve()
    render(assets / "status-home.svg", assets / "status-home.png", 64, 64)
    render(assets / "status-stopwatch.svg", assets / "status-stopwatch.png", 64, 64)


if __name__ == "__main__":
    main()
