# Home Hub UI icon sources

These PNGs are the user-selected BluePaws Home Hub artwork supplied during the
2026-08-29 hardware UI session. They are retained here so the embedded LVGL
assets are reproducible rather than being tied to files in a Downloads folder.

Regenerate `main/ui_icons.cpp` and `main/ui_icons.h` from the repository root
with a Python environment containing Pillow:

```powershell
python tools/build_home_hub_icons.py
```

The converter crops and downsamples the artwork with alpha-aware Lanczos
filtering. Monochrome controls become compact LVGL A8 masks so dark/light themes
can recolour them without storing duplicates. The coloured map artwork becomes
an ARGB8888 image.

The magnifying-glass artwork supplied for zooming out is retained as
`zoom-out.png`; its filename describes its BluePaws control role rather than
the generic source artwork name.
