# Walter as a BluePaws cellular collar

Walter (ESP32-S3 plus Sequans GM02SP) runs the BluePaws TLV v1.2 collar
protocol with real GNSS and LTE-M. This is still a Walter-specific firmware
target: there is no SX1262, BLE Home-beacon scanning, battery measurement, or production PCB
power circuit on this board. The production nRF52840 collar source is not
flashable onto Walter.

## Device and startup

- The provisioned Walter board is device **1011** in the same Family as the
  previous device 1010. Its APN, bearer token, HMAC key and trusted CA are in
  the ignored local `collar/walter/include/walter_secrets.h`; the matching
  private simulator credential is in the ignored `tools/devices.json`.
- A build with valid credentials starts the online Normal collar schedule on
  every ESP boot. LTE registration supplies UTC before GNSS; no PC `clock`
  command is required. A build without credentials stays idle in offline
  bench mode. `stop` stops the current session until `start` or reboot.
- The initial cycle builds a boot packet and records its failed LoRa attempt;
  the cloud sees the packet only when LTE fallback becomes due. The `send`
  command remains an explicit one-shot diagnostic that uploads immediately
  over LTE, independent of the
  ordinary fallback interval.

## Missing LoRa path and LTE fallback

Walter never initializes or probes an SX1262. Each would-be LoRa uplink logs
`TX_FAILED reason=SX1262_NOT_PRESENT` immediately. It neither reports a
simulated successful transmission nor waits for a nonexistent ACK. The signed
packet carries `ERROR_PRESENT` because the radio path failed. There is no
specific RF-cause field in TLV v1.2; the serial log identifies the cause and
the web report shows the generic packet error flag.

The same profile table as the production collar determines the wake interval,
scheduled LTE ratio and consecutive failed LoRa cycles before fallback.
Failure counts reset in bounded groups after a fallback request. If a scheduled
LTE upload and a fallback coincide, the packet is sent once. The original
packet bytes and HMAC are used for the LTE upload, with a strict cloud receipt
check.

| Profile | Sleep after a cycle | Scheduled LTE ratio | LoRa failure threshold |
| --- | ---: | ---: | ---: |
| Normal | 10 minutes | 1 in 10 cycles | 3 cycles |
| PowerSave | 30 minutes | 1 in 30 cycles | 3 cycles |
| Active | 1 minute | 1 in 5 cycles | 2 cycles |
| Debug | 30 seconds | Every cycle | 1 cycle |

In Normal mode, the boot cycle is failure 1. Failures 2 and 3 occur after
roughly 10 and 20 minutes plus work time; the third triggers LTE. Continued
absence of LoRa then requests another fallback every three cycles, roughly
30 minutes apart. Scheduled 1-in-10 LTE uploads can occur between fallbacks.
The GUI updates only after an accepted LTE report; GNSS measurements on
intervening cycles remain local. GNSS and LTE do not run concurrently.

Walter has no battery measurement. It sends zero millivolts as unavailable;
the GUI displays **No data** for this sentinel. Do not read it as an empty
battery or use it to estimate collar life.

## Commissioning and verification

At 115200 baud on the Walter USB console (COM10 on `reesPC` in November 2026),
`status` shows the build revision, device ID, selected profile, running state,
next wake, LoRa failure count and LTE skip count. The boot log shows the same
build revision. A cold start should show network UTC, GNSS acquisition,
immediate LoRa failure, then a scheduled sleep. The third Normal failure
should show `LoRa failure threshold reached; fallback due`, a matching
`[LTE] ACCEPTED device=1011` receipt and a new Supabase observation.

The private provisioning header and simulator bundle are required for every
future flash. Never commit or paste their contents. The original 1010 image
was backed up locally before flashing 1011; keep that file private because
the old credentials are embedded in the image.
