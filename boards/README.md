# Development board support

This directory contains project-local PlatformIO definitions and variant files for development boards used to build or test BluePaws firmware.

- `*.json` files are custom PlatformIO board definitions. They remain directly in this directory because `platformio.ini` sets `boards_dir = boards`.
- `variants/` contains vendor-specific pin and hardware variants required by those definitions.

These files describe third-party development hardware; they are not BluePaws PCB designs or standalone firmware applications. BluePaws PCB source belongs under `pcb/`, product firmware under `collar/` or `hub/`, and diagnostic firmware under `diagnostics/`.
