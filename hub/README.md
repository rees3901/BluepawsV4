# Home Hub

This directory contains every Home Hub implementation and its shared supporting
material:

- `platformio/` — the Heltec Wireless Tracker V2 ESP32-S3 prototype. Its embedded
  LittleFS dashboard is in `platformio/data/`.
- `esp-idf-p4/` — the ESP-IDF implementation for the Guition ESP32-P4 testbed.
- `maps/` — tooling inputs and documentation for offline map packs.
- `tests/esp-idf-p4/` — host-side tests for the ESP-IDF core component.

The PlatformIO environment retains the name `hub` for command-line and CI
compatibility.
