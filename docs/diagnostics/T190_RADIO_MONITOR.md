# Heltec T190 passive TLV sniffer

The `sniffer` PlatformIO environment builds the receive-only diagnostic firmware
for the Heltec Vision Master T190 connected as COM11 during development.

It uses the canonical `BluepawsProtocol` shared library and locked LoRa profile,
so protocol changes are compiled from the same source as the collar and Home Hub.

```powershell
py -3.11 -m platformio run -e sniffer
py -3.11 -m platformio run -e sniffer -t upload --upload-port COM11
py -3.11 -m platformio device monitor --port COM11 --baud 115200
```

The sniffer validates TLV v1.2 packet structure and prints source/destination
addresses, sequence, state, flags, reason, position fields, byte counts, RSSI,
SNR and raw hexadecimal bytes. It intentionally does not contain device HMAC
keys, so `structure=valid` does not mean cryptographically authenticated. The
collar, Home Hub and Supabase remain responsible for authentication.

The user and boot buttons browse the ten-packet in-memory history. Double-click
either button to show the per-source summary. The tool is passive: it never
transmits, acknowledges, forwards or modifies a radio packet.
