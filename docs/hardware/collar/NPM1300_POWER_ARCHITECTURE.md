# nPM1300 collar power architecture

**Status:** Approved direction for the next BluePaws V4 collar PCB revision

**Recorded:** 2026-10-01

**Applies to:** Combined RAK4630 + GM02SP production-collar PCB

## Decision

Use the Nordic Semiconductor **nPM1300** as the collar's central PMIC. It
replaces these three devices in the current schematic:

- BQ24074 battery charger and power-path controller;
- TPS62840 3.3 V buck regulator;
- MAX17048 fuel gauge.

The existing KiCad schematic still contains the superseded devices. This
document is the design target; it does not claim that the schematic migration
has already been completed.

Use the 5 mm x 5 mm QFN32 production package for the first prototypes unless a
later space review justifies WLCSP. Select a current production revision
recommended by Nordic for new designs and review the matching errata before the
BOM is released.

## Why this part

The nPM1300 combines the functions needed by the collar:

- dynamic power-path and a programmable 32 mA to 800 mA linear charger for a
  one-cell Li-ion/LiPo battery;
- USB Type-C source detection/current limiting through CC1 and CC2;
- two independently configurable 1.0 V to 3.3 V, 200 mA buck regulators;
- two outputs configurable as 50 mA LDOs or 100 mA load switches;
- battery voltage, current and temperature measurement for Nordic's host-side
  fuel-gauge algorithm;
- ship and hibernate modes, watchdog, boot monitoring, hard reset, power-fail
  warning, GPIO and three low-side LED drivers;
- I2C-compatible TWI control and interrupt/event routing.

This reduces IC count and gives firmware direct control and diagnostics. It is
also explicitly positioned by Nordic for nRF52 hosts and asset trackers.

## Target power tree

```text
USB-C VBUS ----> nPM1300 VBUS/SYSREG ----+----> VSYS ----> GM02SP VBAT pins
CC1/CC2 -------> nPM1300 CC1/CC2         |          `----> modem bulk capacitors
1-cell LiPo <---> nPM1300 VBAT/charger --+
LiPo NTC ------> nPM1300 NTC

nPM1300 BUCK1, 3.3 V ------------------------> RAK4630 / nRF52840 / SX1262
nPM1300 BUCK2 -------------------------------> reserved; fit only for a proven rail
nPM1300 LDO/load-switch outputs --------------> optional switched low-current loads
nPM1300 TWI + interrupt ----------------------> RAK4630
```

### Rail rules

- Power the **GM02SP directly from VSYS**, not from either 200 mA buck. Nordic
  rates VSYS for up to 1 A from battery and up to 1.5 A from VBUS/battery under
  the specified conditions. This is compatible on paper with the bare GM02SP
  supply range and published current figures, but it still requires bench
  validation at low battery, during LTE bursts, and while USB charging.
- Route all GM02SP VBAT pins as a short, wide, low-impedance connection. Place
  the modem's high-frequency decoupling and bulk capacitance at the module pins.
  The exact capacitor network must follow the current GM02SP hardware guide;
  two 22 uF parts may be a starting point, not an unverified production rule.
- Use BUCK1 at 3.3 V for the RAK4630 domain only after a worst-case load budget
  confirms adequate margin below 200 mA, including attached sensors and LEDs.
- Reserve BUCK2 rather than assigning it automatically. A separate 1.8 V rail
  is only useful if the final eSIM/SIM or level-interface design actually needs
  one. Do not connect it to the GM02SP's 1V8_OUT rail.
- Treat the LDO/load-switch outputs as low-current rails. They cannot switch the
  GM02SP supply directly.
- Use a protected cell/pack or retain the required external cell-protection
  circuit. A charger/PMIC is not a substitute for the battery manufacturer's
  required protection.

## USB-C and charging

- Connect CC1 and CC2 as shown in Nordic's reference design. The nPM1300 has
  the Type-C sink pull-downs and performs source-current detection; it is not a
  USB Power Delivery controller.
- The input-current limit defaults to 100 mA until configured. Firmware must
  not assume 500 mA or 1.5 A is available before source detection and PMIC
  configuration are complete.
- Set charge termination voltage to the selected cell specification, normally
  4.20 V for a conventional one-cell LiPo, and validate it against the actual
  battery datasheet.
- Start prototype evaluation at a conservative **300 mA charge current**. Raise
  it only after checking the cell's allowed charge rate, enclosure temperature,
  PMIC thermal regulation, USB source capability and simultaneous system load.
  The 500 mA value remains an evaluation target, not the default requirement.
- Connect the pack NTC and configure temperature limits. Validate charge
  behaviour at hot and cold boundaries in the intended enclosure.
- USB D+ and D- remain a separate data-path decision; the nPM1300 only handles
  the power/Type-C aspects described above.

## Fuel gauge and firmware

Removing the MAX17048 transfers more responsibility to collar firmware:

- the nPM1300 measures battery voltage, current and temperature;
- Nordic's fuel-gauge algorithm runs on the host nRF52840;
- the production battery needs a suitable Nordic battery model; use an existing
  validated model only when it matches the chosen cell, otherwise profile the
  cell with nPM PowerUP and the appropriate Nordic evaluation hardware;
- confirm that the Nordic fuel-gauge library can be integrated with the
  RAK4630's selected firmware stack and licence/build system before schematic
  sign-off. Raw PMIC measurements are not equivalent to a validated state of
  charge estimate;
- implement a small PMIC driver boundary for initialisation, telemetry, event
  handling, charger policy, watchdog service, ship/hibernate entry and fault
  reporting.

The PMIC TWI bus must remain accessible during development. Provide test access
to TWI, interrupt, VSYS, BUCK1, battery and ground.

## Logic-level clarification

The GM02SP host interface uses a 1.8 V logic domain, but its relevant digital
inputs are specified as 3.3 V tolerant. Therefore, a blanket 3.3 V-to-1.8 V
translator requirement for signals driven by the RAK4630 is not justified.
The reverse direction is a separate question: verify that each 1.8 V GM02SP
output meets the RAK4630/nRF52840 input-high requirement at the selected 3.3 V
RAK I/O supply. Add translation only where the electrical limits require it.

## Schematic and layout requirements

1. Begin from Nordic's current QFN reference design, hardware guidelines and
   checklist; do not recreate the power stage from the product-page block
   diagram.
2. Confirm the exact orderable part/revision and review its current errata.
3. Place the PMIC, inductors and their input/output capacitors as a tight power
   block. Keep switch nodes short and away from GNSS, LTE and LoRa RF regions.
4. Give VSYS and the modem return path enough copper for the full transient
   load. Do not neck the path through thermal spokes or narrow vias.
5. Keep an uninterrupted ground reference and add ground stitching around the
   power block without violating antenna keep-outs.
6. Route battery current sensing exactly as Nordic specifies; uncontrolled
   alternate current paths can invalidate fuel-gauge measurements.
7. Retain measurement points for VBUS, VBAT, VSYS, 3V3, NTC, TWI and interrupt.
8. Check startup defaults and pin-strapped settings so that the essential 3.3 V
   rail starts safely before firmware configures the PMIC.

## Verification gates

The design is not ready for production until these tests pass:

- nPM1300 EK + intended battery evaluation, including battery profiling or
  validation of an existing model;
- RAK4630 worst-case 3.3 V rail current and startup-margin measurement;
- GM02SP attach and maximum-power LTE transmission from a fully charged battery,
  a near-empty battery and USB power;
- simultaneous charge plus LTE load, including cable/source changes;
- VSYS droop and ripple capture at the GM02SP pins;
- charger thermal test in the closed collar enclosure;
- cold/hot NTC handling and charge inhibition;
- ship/hibernate current, wake path, long-press reset, watchdog and failed-boot
  recovery;
- GNSS sensitivity/noise comparison with bucks in automatic and forced-PWM
  modes;
- firmware recovery after TWI errors and unexpected PMIC resets.

Do not rely on scheduling LTE and LoRa transmissions apart as the only power
integrity measure. It is a useful energy/EMI policy, but every rail must remain
electrically safe under credible overlapping and fault conditions.

## Authoritative references

- [Nordic nPM1300 product page](https://www.nordicsemi.com/Products/nPM1300)
- [nPM1300 Product Specification](https://docs.nordicsemi.com/r/bundle/ps_npm1300/page/keyfeatures_html5.html)
- [nPM1300 documentation and reference-design compatibility matrix](https://docs.nordicsemi.com/r/bundle/comp_matrix_npm1300/page/comp/npm1300/npm1300_doc_ref_design_files_overview.html)
- [Developing with the nPM1300 in nRF Connect SDK](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/app_dev/device_guides/pmic/npm1300.html)
- [BluePaws Walter/GM02SP upstream reference](../../firmware/collar/WALTER_GM02SP_UPSTREAM_REFERENCE.md)

Before changing the modem supply, re-check the current bare-GM02SP source and
installed modem revision as required by the BluePaws upstream-reference policy.
