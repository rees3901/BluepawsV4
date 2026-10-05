# Personal T190 replacement home hub

Isolated Heltec Vision Master T190 adapter for the personal hub 0030. No canonical
source, packet contract, production target, web application or backend is changed.
Keep the failed Tracker disconnected: only one physical hub may use this identity.

Build with `pio run -d hub/personal-t190 -e personal_t190`. Supply the private
`.secrets/hub_secrets.h` with the replacement gateway configuration and
`PERSONAL_HOME_LAT` / `PERSONAL_HOME_LON` before building. Never commit this file.
Coordinates are the user's configured home fallback; optional persisted
`bp-hub-self` keys `homeLat` / `homeLon` override the provisioning defaults.
The T190 has no GNSS. Status uses the existing `position_simulated` contract
to distinguish the supplied position from a measured fix. Collar GPS is unchanged.

`build_config.py` generates private translation units from the existing canonical
hub services and personal Tracker display. Checked substitutions remove the
Tracker-only RF front-end controls and GNSS UART access while preserving the
settings persistence worker, relay, commands, BLE, Wi-Fi and storage services.
An upstream source mismatch fails the build instead of silently applying an adapter.

Radio pins come directly from `diagnostics/t190-radio-monitor/include/pins.h`.
SX1262 runs on HSPI; ST7789 runs separately on FSPI. USER GPIO21 cycles the eight
status pages; hold USER to return home. The compact Tracker layout is retained.
TFT power GPIO7 is active low. Never flash the Tracker V2 binary onto the T190.

The screen and backlight blank after 60 seconds without USER interaction.
The first USER tap (or hold) wakes the same page; subsequent taps cycle pages
and a hold returns to HUB. Incoming packets do not extend the display timeout.
Only the display is disabled: radio reception, BLE and cloud relay keep running.
The timeout is `DisplayIdleMs` in the isolated build adapter; TFT power stays on.

This device has 16MB flash. Preserve its complete original flash before upload.
Flash application with `pio run -d hub/personal-t190 -e personal_t190 -t upload
--upload-port COM22`; upload the existing browser filesystem with `-t uploadfs`.
Both targets write device flash. Private recovery images and SHA256 hashes are
kept outside Git. Restore the original complete image at offset zero to recover
the sniffer, or rebuild the existing `sniffer` target.

Hardware verification must confirm radio initialization, Wi-Fi connection,
authenticated cloud status and collar relay. A successful build alone does not
prove command delivery or range. Production deployments remain outside this work.

## COM22 validation — 5 October 2026

- ESP32-S3 revision 0.2, 16MB flash; complete original sniffer image backed up.
- Pinned PlatformIO build passed. Firmware and browser filesystem write hashes
  verified; recovery artifacts and private configuration preserved outside Git.
- SX1262 initialization passed, primary guest Wi-Fi connected, display initialized
  on the separate SPI bus. Visual orientation/button confirmation remains physical.
- Authenticated hub status returned HTTP 200. Backend confirmed Home mode, the
  configured home coordinate match, `position_simulated=true`, settings revision 7
  applied and BLE off. No measured hub GNSS fix is claimed.
- Direct LAN browser request from the PC timed out; browser files are installed,
  but guest-network reachability from this PC is not confirmed.
- Collar relay, command delivery, range and prolonged uptime need actual traffic
  validation. No production software or schema changes were made.

### LED relay and display idle update

The personal LED cloud relay adapter was flashed to COM22, followed by the
60-second display idle update. Both uploads passed flash hash verification.
SX1262 initialization, Wi-Fi connection and BLE home advertising were observed.
Authenticated hub status was accepted with HTTP 200 after both updates.
Serial confirmed the display idle transition 60 seconds after display startup;
physical USER wake and visual blanking require the user's confirmation.
The existing stored replay batch returns HTTP 400 and snapshot refresh returned
HTTP 503; queued records were preserved. Fresh collar relay and the unreleased
GUI LED command flow must not be treated as verified from these startup checks.
The recovery bundle includes `personal-t190-idle-display-firmware.bin`.
