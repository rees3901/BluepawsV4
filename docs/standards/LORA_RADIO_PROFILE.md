# BluePaws LoRa Radio Profile

Status: locked production design decision  
Last updated: 2026-10-19  
Applies to: collar, Home Hub, diagnostic receivers and LoRa bench tools

## 1. Source of truth

This document records the agreed radio standard and its engineering rationale.

The compiled source of truth is:

```text
shared/lib/BluepawsProtocol/bp_config.h
```

All transmitting and receiving implementations must use the same frequency, spreading factor, bandwidth, coding rate, preamble, header mode, CRC setting and sync word. Operating profiles may change transmit power only.

## 2. Locked UK profile

| Parameter | Locked value |
|---|---|
| Radio | SX1262, including the RAK4630 implementation |
| Frequency | 869.500 MHz |
| Spreading factor | SF10 |
| Bandwidth | 125 kHz |
| Coding rate | 4/6 |
| Forward error correction | Enabled through CR4/6 |
| Preamble | 8 symbols |
| Header mode | Explicit |
| Payload CRC | Enabled |
| Low data-rate optimisation | Disabled |
| Sync word | 0x12, fixed private-network value |
| Maximum raw application payload | 64 bytes |
| Dynamic SF/BW/CR profiles | Not used |

CR4/6 adds 50 percent redundancy relative to the uncoded application data. One third of the resulting coded bits represent redundancy. The radio modem applies this coding internally, so a 64-byte application payload must still be supplied to the modem as 64 bytes rather than being expanded to 96 bytes by the application.

## 3. Transmit-power policy

| Operating profile | TX power |
|---|---:|
| POWER_SAVE | 10 dBm |
| NORMAL | 14 dBm |
| DEBUG | 14 dBm |
| ACTIVE | 20 dBm |
| LOST_ALERT | 20 dBm |

The higher-power profiles remain subject to final product compliance, including complete-system ERP after antenna gain and feeder losses. Lost Alert does not provide an exemption from the applicable radio requirements.

## 4. Why SF10 was selected instead of SF12

For the maximum 64-byte application packet, using BW125, CR4/6, CRC, an explicit header and an 8-symbol preamble:

| Comparison | SF10, selected | SF12, rejected |
|---|---:|---:|
| Time on air | 804.864 ms | 3,219.456 ms |
| Relative airtime | 1x | 4x |
| Approximate TX energy per packet at equal power | 1x | 4x |
| Approximate receiver sensitivity | -132 dBm | -137 dBm |
| Link-budget improvement | Baseline | Approximately 5 dB |
| Theoretical free-space range | Baseline | Approximately 1.78x |
| Likely practical ground-level collar range | Baseline | Approximately 1.3x to 1.5x |

SF12 was rejected because it uses approximately four times the airtime and transmit energy for only about 30 to 50 percent more practical range in the intended ground-level pet-collar installation.

The theoretical 5 dB improvement can produce about 78 percent more distance in ideal free space. Normal use is limited by ground proximity, buildings, foliage, antenna orientation, multipath and absorption or detuning caused by the animal. These effects reduce the realistic improvement substantially.

SF10 therefore provides the preferred balance of:

- long usable range;
- battery consumption;
- channel occupancy;
- collision probability;
- reporting frequency; and
- reliable fixed compatibility between collar and Home Hub.

SF12/BW125 would also require low data-rate optimisation to be enabled. The selected SF10/BW125 profile keeps it disabled.

## 5. Fixed-profile rule

Do not dynamically change spreading factor, bandwidth or coding rate according to the collar operating profile. A receiver configured for different PHY parameters would miss the transmission.

Only transmit power may change between POWER_SAVE, NORMAL, ACTIVE and LOST_ALERT. Any future proposal to change the fixed PHY must update this document, `bp_config.h`, the collar, Home Hub and diagnostic receivers together.
