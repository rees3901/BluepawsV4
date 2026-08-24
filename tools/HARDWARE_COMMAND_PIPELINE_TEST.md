# Bluepaws V4 Hardware Command Pipeline Test

This is the bench test for collar ↔ Home Hub downlink interaction.

It is intended for the current proof-of-concept hardware:

- Collar testbed: RAK4631 / WisMesh Board One on `COM23`
- Home Hub: Heltec Wireless Tracker V2 on `COM7`
- LoRa sniffer: Heltec T190 Vision Master on `COM11`
- Serial baud: `115200`

The Home Hub receives collar TLV packets over raw LoRa, relays them to Supabase, and can also transmit short LoRa command packets back to the collar during the collar's 10-second RX window.

Pet names are intentionally not part of the collar command path. The collar only knows its 16-bit device ID. Human-friendly names such as `Podge` remain backend/UI aliases.

## What this test covers

The harness exercises:

1. Presence / wake check-in observation.
2. Hub HTTP command queueing.
3. LoRa command transmission from Home Hub to collar.
4. Collar command reception during its RX window.
5. Power-profile change command.
6. Collar mode ACK back to the hub.
7. Device status command.
8. Collar status response, including battery/profile-style telemetry.
9. Optional sniffer confirmation that packets are visible over the air.
10. Restore back to Normal profile at the end.

## One-time setup on Windows

From the repository root:

```powershell
cd "C:\Users\reesMiniPC\Documents\Codex\2026-08-07\prior-conversation-with-codex-conversation-role\work\BluepawsV4"
py -3.11 -m pip install pyserial
```

If your Home Hub firmware has not yet been rebuilt with the `/api/device-status` endpoint, upload the hub first:

```powershell
py -3.11 -m platformio run -e hub -t upload --upload-port COM7
```

The collar should already have the debug serial console available. If not, upload the collar firmware too:

```powershell
py -3.11 -m platformio run -e collar -t upload --upload-port COM23
```

## Run the full bench test

If mDNS works:

```powershell
py -3.11 .\tools\hardware_command_pipeline_test.py --hub-url http://bluepaws-hub.local --target-id 1001
```

If mDNS is flaky, use the Home Hub IP shown in the hub serial log:

```powershell
py -3.11 .\tools\hardware_command_pipeline_test.py --hub-url http://192.168.0.67 --target-id 1001
```

To test a different profile command:

```powershell
py -3.11 .\tools\hardware_command_pipeline_test.py --hub-url http://192.168.0.67 --target-id 1001 --profile powersave
```

Supported `--profile` values:

- `normal`
- `powersave`
- `active`
- `lost`

## Expected successful signs

You should see a sequence like:

- Collar serial:
  - `[DBG] Debug cadence ON`
  - `[TX] ...`
  - `[RX] Listening 10000ms...`
  - `[RX] CMD_MODE → Active`
  - `[TX] MODE_ACK ...`
  - `[RX] CMD_STATUS ...`
  - `[TX] STATUS_RESP ...`

- Hub serial:
  - `[LORA] RX valid ... device=1001`
  - `[CMD] Queued ...`
  - `[LORA] CMD TX ...`
  - `[ACK] Cmd seq ... ACK'd by ...`

- Sniffer serial:
  - `[RX] ... device=1001 ...`

At the end the harness prints a compact pass/fail summary.

## Useful flags

Skip the sniffer if COM11 is busy:

```powershell
py -3.11 .\tools\hardware_command_pipeline_test.py --hub-url http://192.168.0.67 --skip-sniffer
```

Do not force the collar to transmit over serial; wait for the natural collar wake window:

```powershell
py -3.11 .\tools\hardware_command_pipeline_test.py --hub-url http://192.168.0.67 --skip-collar-control --timeout 180
```

Show the plan without touching serial ports or HTTP:

```powershell
py -3.11 .\tools\hardware_command_pipeline_test.py --dry-run
```

## Current prototype caveat

Downlink command authentication is still explicitly a future protocol decision. These commands are useful for bench testing and firmware flow validation, but they are not yet production-secure.
