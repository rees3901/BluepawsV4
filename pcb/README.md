# PCB Hardware

KiCad projects and their supporting component models are grouped by product.

For GM02SP, GNSS/LTE antenna, and modem power decisions, start with the
[Walter/GM02SP source guide](../docs/firmware/collar/WALTER_GM02SP_UPSTREAM_REFERENCE.md).
Its Walter board specifications must be checked against the bare GM02SP and the
production collar schematic before being used as PCB requirements.

- `collar/` — BluePaws V4 collar main board, second board, evaluation layouts,
  plots, reports, symbols, footprints, 3D models, and retained vendor source
  packages.

The vendor assets have deliberately been retained. Some appear duplicated
because they preserve the original package or tool format used by an existing
KiCad project; deduplication should only happen after KiCad library references
are audited interactively.
