# T190 Radio Monitor

Passive diagnostic receiver firmware for the Heltec Vision Master T190
(ESP32-S3 + SX1262). It displays received BluePaws packets without participating
in normal collar-to-hub routing.

Build it with the backward-compatible PlatformIO environment name:

```bash
pio run -e sniffer
```

Protocol and operating notes are in
[`docs/diagnostics/T190_RADIO_MONITOR.md`](../../docs/diagnostics/T190_RADIO_MONITOR.md).
