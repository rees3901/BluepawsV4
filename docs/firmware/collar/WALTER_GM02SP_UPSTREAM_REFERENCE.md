# Walter and GM02SP upstream reference

This is a decision guide to [QuickSpot's Walter documentation][upstream-root],
not a copy of its API or a specification for the final BluePaws collar PCB.
Reviewed against upstream commit `5e16b81f0aaa2f0a455feb6573bb94a46786dd7e`
(29 August 2026). Follow the links for the full, current instructions, and check
the installed modem/library version before relying on a command or firmware
update procedure.

Walter combines an ESP32-S3 and a Sequans Monarch 2 GM02SP. Its board-level
power supply, antenna path, SIM tray, pin mapping and USB behaviour belong to
**Walter**. A BluePaws PCB using a bare GM02SP needs its own schematic, modem
datasheet and RF review; do not transfer Walter's electrical limits or antenna
bias assumptions to that PCB without checking them.

## Where to look upstream

| Decision or task | Primary source | BluePaws use |
| --- | --- | --- |
| Board power, pins, internal modem wiring, test pads and electrical limits | [Walter hardware][hardware] | Walter test fixture and carrier design; check the [Walter hardware CAD repository][cad] for footprints, schematic and mechanical files. |
| Antenna selection and first power-up | [Getting started][getting-started], [GNSS FAQ][gnss-faq] | Select a passive GNSS antenna and provide an LTE antenna before cellular tests. |
| GNSS configuration, assistance, fix events, status codes and time | [Arduino/ESP-IDF GNSS API][gnss-api], [event handlers][events] | Diagnose fix quality and sequence LTE/GNSS operations in the Walter testbed. |
| SIM, RAT, registration, signal, APN and IP context | [SIM/network API][sim-api], [PDP API][pdp-api], [cellular guide][cellular] | Bring up 1NCE or another IoT SIM; record RAT, registration and signal quality. |
| HTTPS and certificate handling | [HTTP API][http-api], [TLS API][tls-api] | Verify the actual Supabase upload and receipt path. |
| Sleep and modem firmware maintenance | [Power API][power-api], [power FAQ][power-faq], [modem update FAQ][update-faq] | Measure real duty-cycle power and plan firmware updates by starting version. |
| Direct AT command details | [Sequans LR8.2 AT manual, Rev. 3][at-manual] | Check syntax and response semantics when the library abstraction is insufficient. |
| Alternative toolchains and protocols | [Toolchains][toolchains], [modem library index][library-index] | Reference for a future ESP-IDF port or CoAP/UDP experiment; no BluePaws protocol change is implied. |

QuickSpot's [WalterDemo packet format][walterdemo] is for its own demo server.
BluePaws continues to use [TLV v1.2](../../protocol/TLV_PROTOCOL_V1_2.md)
and the [Supabase ingestion runbook](../../operations/TLV_INGESTION_RUNBOOK.md).
The `status`, `gnss`, `assist`, `send` and other USB console commands are
**BluePaws testbed firmware commands**, listed in the
[Walter LTE/GNSS testbed guide](WALTER_LTE_TESTBED.md); they are not modem AT
commands or part of QuickSpot's console.
For planned direct AT work, start with the existing
[BluePaws GM02SP command notes](USEFUL_GM02SP_COMMANDS.md), then verify the
installed firmware against the upstream AT manual.

## Hardware and RF decisions

- **GNSS antenna:** Walter's GNSS connector supplies no phantom/bias power.
  QuickSpot specifies a **passive** antenna and recommends the Taoglas
  FXP.611/FXP611.07.0092C. Walter already includes a GNSS LNA and SAW filter.
  A coax-fed ceramic patch may still contain an active LNA; check its part number
  and power requirement before using it. Clear sky and a stationary antenna are
  essential for a useful comparison. [Hardware][hardware],
  [getting started][getting-started], [GNSS FAQ][gnss-faq].
- **LTE antenna:** Fit an LTE antenna before transmitting. QuickSpot warns that
  running LTE without one can damage the modem RF front end and recommends the
  Taoglas FXUB63.07.0150C for Walter. [Getting started][getting-started].
- **Supply and carrier:** Walter accepts 3.0–5.5 V at VIN/USB-C. QuickSpot says
  VIN and USB power are directly connected and must not be applied together;
  its electrical table lists up to 1.5 A consumption at 3.3 V. The switched
  3.3 V output is limited to 250 mA, defaults off and is enabled with GPIO0 low
  after boot. GPIO0 low **during boot** selects the ESP download mode. These are
  Walter-board facts, not a GM02SP supply specification. [Hardware][hardware],
  [hardware FAQ][hardware-faq].
- **Debug access:** The ESP32-S3 connects to GM02SP UART0 through GPIO48 (TX),
  GPIO14 (RX), GPIO21 (RTS), GPIO47 (CTS), GPIO45 (reset) and GPIO46 (wake).
  Walter exposes modem debug/test pads. The modem's default UART0 is 115200 8N1
  with hardware flow control; UART1 is for manual modem updates, and UART2 is
  console output. The USB COM port used by BluePaws firmware is the **ESP32
  console**, not direct access to the modem AT UART. [Hardware][hardware].
- **SIM choice:** Walter has a removable nano-SIM tray and no operator SIM lock.
  QuickSpot says GM02SP iSIM hardware is not activated in Walter; do not plan
  around usable iSIM without fresh vendor confirmation. The removable slot may
  accept a programmable eUICC SIM, which is different from a soldered fixed
  MFF2 SIM. [eSIM/iSIM FAQ][esim-faq], [cellular guide][cellular].

## GNSS acquisition, time and accuracy

1. `gnssConfig` selects sensitivity (low/medium/high), acquisition mode and
   on-device positioning. Its settings persist over resets but may need restoring
   after modem firmware changes. Higher sensitivity uses more power. The default
   acquisition mode is cold/warm. Hot start assumes a known position within
   about 100 km; use it only after a valid fix. QuickSpot reports that
   reconfiguring after a valid fix can shorten subsequent acquisition time.
   [GNSS API][gnss-api].
2. `gnssSetUTCTime` supplies a time hint; it does not prove satellite time.
   `gnssGetUTCTime` reads the GNSS subsystem's clock. After cellular registration,
   `getClock` can read modem/network time. A fix event has its **own timestamp**;
   keep that separate from packet-send time and reject implausible or stale
   timestamps. [GNSS API][gnss-api], [general API][general-api],
   [GM02SP time foundation](GM02SP_TIME_FOUNDATION.md).
3. Assistance status and `gnssUpdateAssistance` cover almanac and real-time or
   predicted ephemeris. Real-time ephemeris is the vendor's recommended immediate
   aid. A download uses cellular data; BluePaws must finish LTE work and turn RF
   off before requesting a GNSS fix. Assistance may improve time to fix but is
   not evidence of a particular position accuracy. [GNSS API][gnss-api].
4. GNSS fix events are asynchronous. In QuickSpot's status enum, **`0` means
   READY**, `1` stopped by user, `2` no RTC and `3` LTE concurrency. The status
   alone does not validate a position: check timestamp, coordinates, reported
   uncertainty and the actual acceptance rules. Satellite entries/CN0 values
   are useful RF diagnostics; the number listed does not prove how many satellites
   were used in the solution. [GNSS API][gnss-api], [event handlers][events].

For the current BluePaws Walter firmware, `gnss settle` logs `status`, UTC,
uncertainty, satellite entries and CN0, then accepts a sample only if
[`usableGnssSnapshot`](../../../collar/walter/src/main.cpp) passes **all** checks:
READY status, plausible and fresh UTC, valid coordinates, at least four
satellite entries within the library's maximum, and estimated confidence in
`(0, 1000]` metres. The diagnostic's 50 m target controls early stopping; it is
**not** the general validity threshold.
Thus `status=0` together with `usable=0` points to another failed check. The
September outdoor log does not show which one; capture `status`/seeded UTC and
compare the fix timestamp, then instrument rejection reasons if needed. A
reported 95 m uncertainty and several CN0 values below 30 warrant an antenna
and sky-view comparison even if the coordinate cluster looks tighter. See the
[testbed settling procedure](WALTER_LTE_TESTBED.md#comparing-snapshots-with-a-bounded-settling-session).

## Cellular, HTTPS and power decisions

- **Registration:** A SIM must actually support LTE-M or NB-IoT in the test
  area. Configure the correct APN/PDP authentication, check SIM readiness,
  registration and RSRP/RSRQ, and record RAT. QuickSpot's RAT change flow uses
  `setRAT` followed by a modem reset. Its UK coverage comments are field
  reports, not a guarantee for our SIM or location. [Cellular guide][cellular],
  [SIM/network API][sim-api], [PDP API][pdp-api],
  [communication FAQ][communication-faq].
- **Transport:** The Walter library supports HTTP(S), CoAP/DTLS, MQTT and
  sockets. QuickSpot warns that TCP-based traffic over NB-IoT may be unreliable
  at scale; changing BluePaws transport requires measuring our provider and
  preserving TLV authentication and cloud receipt semantics. The HTTP API notes
  that `httpConnect` is buggy for the ordinary POST path and recommends
  `httpSend` directly unless TLS client private-key handling requires it.
  Treat a send command response, HTTP status and application receipt as distinct
  outcomes. [Communication FAQ][communication-faq], [HTTP API][http-api],
  [BluePaws testbed](WALTER_LTE_TESTBED.md).
- **TLS:** Configure a TLS profile with both CA and hostname validation, and
  manage CA certificates in modem credential slots. BluePaws currently uses
  modem profile 2 and CA slot 12; verify the Supabase chain before replacing
  them. [TLS API][tls-api], [BluePaws testbed](WALTER_LTE_TESTBED.md).
- **Sleep:** `configPSM` and eDRX make requests; the network may grant
  different timers. Modem minimum state and ESP deep sleep are separate, and
  ESP deep sleep can disconnect the USB serial monitor. QuickSpot lists a
  9.8 µA typical Walter deep-sleep figure; use measured **full collar** current
  and wake cycle timing for BluePaws battery estimates. [Power API][power-api],
  [power FAQ][power-faq], [hardware][hardware],
  [troubleshooting FAQ][troubleshooting-faq].
- **Modem firmware:** Identify the installed release first (`ATI1` via modem
  passthrough; `ATI3` for hardware revision). QuickSpot's published FOTA recipe
  is explicitly for a particular starting release and warns that an update
  resets hardware settings such as UART speed and wake source. It is not a
  generic upgrade command. Recheck the vendor page and plan recovery before
  any update. [Modem update FAQ][update-faq].

## Evidence to keep from hardware tests

| Test | Record so the result can guide design |
| --- | --- |
| GNSS antenna comparison | Exact passive antenna/connector, placement and sky view; seeded UTC, assistance age, each fix status/timestamp/uncertainty, CN0 distribution and measured position error at a surveyed or otherwise known point. Keep the antenna still between runs. |
| Cellular bring-up | SIM/provider, APN without credentials, RAT, modem firmware, registration/rejection state, RSRP/RSRQ, time to attach and whether the modem returned plausible UTC. |
| Cloud upload | TLS profile and CA slot, certificate/hostname validation result, HTTP status, response body and matching BluePaws receipt/packet hash. |
| Power | Supply voltage, current during acquisition, attach, upload, idle and sleep; actual PSM/eDRX values granted by the network and wake interval. Walter values are a testbed baseline, not a final collar battery claim. |

[upstream-root]: https://github.com/QuickSpot/walter-documentation/tree/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e
[hardware]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/hardware/walter.md
[cad]: https://github.com/QuickSpot/walter-hardware
[getting-started]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/guides/getting_started.md
[gnss-faq]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/faq/gnss-gps.md
[hardware-faq]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/faq/hardware-features.md
[esim-faq]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/faq/e-sim-and-i-sim.md
[cellular]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/guides/cellular_connectivity.md
[gnss-api]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/walter-modem/arduino_esp-idf/reference/gnss.md
[events]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/walter-modem/arduino_esp-idf/reference/event_handlers.md
[general-api]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/walter-modem/arduino_esp-idf/reference/general.md
[sim-api]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/walter-modem/arduino_esp-idf/reference/sim_and_network.md
[pdp-api]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/walter-modem/arduino_esp-idf/reference/pdp_ctx_management.md
[http-api]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/walter-modem/arduino_esp-idf/reference/http.md
[tls-api]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/walter-modem/arduino_esp-idf/reference/tls_and_certificates.md
[power-api]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/walter-modem/arduino_esp-idf/reference/power_saving.md
[power-faq]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/faq/power-management.md
[update-faq]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/faq/modem-update.md
[communication-faq]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/faq/communication.md
[troubleshooting-faq]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/faq/troubleshooting.md
[toolchains]: https://github.com/QuickSpot/walter-documentation/tree/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/developer-toolchains
[library-index]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/walter-modem/arduino_esp-idf/reference/reference.md
[at-manual]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/file/gm02s_at_commands.pdf
[walterdemo]: https://github.com/QuickSpot/walter-documentation/blob/5e16b81f0aaa2f0a455feb6573bb94a46786dd7e/guides/walterdemo.md
