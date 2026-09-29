# RAK3272S BluePaws radio test

Connect the USB-TTL adapter to the breakout board's UART2 (TX to RX, RX to TX,
and common GND). Attach the LoRa antenna before transmitting. The adapter used
on the bench is COM13 at 115200 baud.

The script's settings match the shared BluePaws radio profile in
`shared/lib/BluepawsProtocol/bp_config.h`: 869.500 MHz, SF10, 125 kHz,
CR 4/6, eight-symbol preamble, and private
sync word `0x12`. RadioLib maps that one-byte value to the SX126x register bytes
`1424`, which is the value used by the RAK `AT+SYNCWORD` command. The script
also selects normal IQ, no RAK P2P encryption, variable payload length and
channel activity detection. Transmit power is set to **5 dBm**, the minimum
supported by the RAK P2P command, for this indoor bench test; it is deliberately
lower than the normal BluePaws operating-profile power.

```powershell
./diagnostics/rak3272s-bluepaws-test/rak3272s_bluepaws_test.ps1 -Action Configure
./diagnostics/rak3272s-bluepaws-test/rak3272s_bluepaws_test.ps1 -Action Status
./diagnostics/rak3272s-bluepaws-test/rak3272s_bluepaws_test.ps1 -Action Send
./diagnostics/rak3272s-bluepaws-test/rak3272s_bluepaws_test.ps1 -Action Heartbeat
./diagnostics/rak3272s-bluepaws-test/rak3272s_bluepaws_test.ps1 -Action Listen -ListenSeconds 15
```

The default test payload is a 40-byte BluePaws TLV v1.2 frame with a fresh
sequence and timestamp, source `FFFD` (chosen diagnostic identity), destination
`0000` (cloud), DEBUG profile, no TLVs, and an all-zero authentication tag. It
is deliberately **not authenticated** and is not a collar command. The T190
sniffer should display `structure=valid auth=unchecked`, `source=FFFD`,
`destination=0000`, and its raw bytes in a `[RX] Hex:` line. The RAK should
report `+EVT:TXP2P DONE`.

For the receiving board's firmware and serial output, see the
[T190 radio monitor](../t190-radio-monitor/README.md) and its
[operating notes](../../docs/diagnostics/T190_RADIO_MONITOR.md).

`Heartbeat` sends a fresh diagnostic frame immediately and then every 60
seconds while the PC process is running. Stop it with Ctrl+C in the terminal.
The RAK firmware does not schedule these transmissions by itself.

The current RAK RUI3 AT interface does not expose a P2P CRC setting. Its actual
PHY CRC compatibility must be confirmed by the T190 receiving the test packet;
an AT readback alone cannot prove it. A collar application exchange also needs
a valid authentication tag, so this diagnostic frame does not prove collar
command handling.
