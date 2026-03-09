/*
  Bluepaws V4 — Collar Pin Definitions
  Hardware: RAKwireless RAK4630 (nRF52840 + SX1262 integrated module)
            + Sequans Monarch 2 GM02SP (LTE-M/NB-IoT + GNSS)

  The RAK4630 has the SX1262 LoRa transceiver wired internally to
  the nRF52840 MCU. These pin assignments reflect the module's
  fixed internal connections — they cannot be changed.

  Ref: https://docs.rakwireless.com/product-categories/wisduo/rak4630-module/datasheet/
*/

#ifndef COLLAR_PINS_H
#define COLLAR_PINS_H

// ── SX1262 LoRa Radio (internal SPI — fixed on RAK4630) ──
// These are the nRF52840 GPIOs connected to the SX1262 inside the
// RAK4630 module. Do NOT change these — they are hard-wired on the PCB.
#define PIN_LORA_NSS   42   // P1.10 — SPI chip select (active low)
#define PIN_LORA_SCK   43   // P1.11 — SPI clock
#define PIN_LORA_MOSI  44   // P1.12 — SPI data out (MCU → SX1262)
#define PIN_LORA_MISO  45   // P1.13 — SPI data in  (SX1262 → MCU)
#define PIN_LORA_BUSY  46   // P1.14 — SX1262 busy indicator (active high)
#define PIN_LORA_DIO1  47   // P1.15 — SX1262 interrupt (RX done, TX done)
#define PIN_LORA_RST   38   // P1.06 — SX1262 reset (active low)

// RAK4630 SX1262 initialisation notes:
//   - DIO2 controls the RF antenna switch (do NOT manually init P1.07)
//   - DIO3 controls the TCXO power supply
//   - Use DCDC regulator mode (not LDO) for better efficiency
//   - Do NOT call pinMode on PIN_LORA_RST before RadioLib init

// ── Sequans Monarch 2 GM02SP (LTE-M/NB-IoT + GNSS) ──
// Single module handles both cellular connectivity and GPS positioning.
// Connected via UART. GPS data comes through AT commands, not a separate
// UART — the Sequans module multiplexes GNSS NMEA over the AT interface.
#define PIN_CELL_TX    0    // MCU TX → GM02SP RX
#define PIN_CELL_RX    1    // MCU RX ← GM02SP TX
#define CELLULAR_BAUD_RATE  115200
#define PIN_CELL_PWR   2    // Power key — pulse to toggle modem on/off
#define PIN_CELL_RST   6    // Reset (active low)

// GPS is provided by the Sequans GM02SP's integrated GNSS receiver.
// No separate GPS module or UART — positioning is accessed via AT+SQNGNSS.
// GNSS power is controlled via AT commands, not a dedicated GPIO.

// ── User Interface ──
#define PIN_LED        LED_BUILTIN    // Green LED on RAK4630
#define PIN_BUTTON     D10            // Provision / wake button
#define PIN_BUZZER     A4             // Passive piezo buzzer (PWM)

// ── NFC (provisioning) ──
// NFC pins are fixed on nRF52840 (P0.09 / P0.10)
// Managed by the NFC peripheral, not GPIO

#endif // COLLAR_PINS_H
