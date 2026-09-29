# Diagnostics

Diagnostic firmware and hardware-facing test utilities live here. These are not
production collar or Home Hub applications.

- `t190-radio-monitor/` — passive Heltec Vision Master T190 receiver used to
  inspect BluePaws LoRa/TLV traffic. It was previously named `sniffer/`.
- `rak3272s-bluepaws-test/` — RAK3272S UART2/P2P bench sender and receiver,
  including the 5 dBm TLV diagnostic heartbeat used with the T190 sniffer.
