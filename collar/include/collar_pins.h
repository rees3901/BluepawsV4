/*
  Bluepaws V4 — Collar Pin Definitions
  Hardware: Seeed XIAO nRF52840 + SX1262 + L76K GNSS + Quectel BG77
*/

#ifndef COLLAR_PINS_H
#define COLLAR_PINS_H

// ── SX1262 LoRa Radio (SPI) ──
#define PIN_LORA_NSS   7
#define PIN_LORA_SCK   5
#define PIN_LORA_MOSI  4
#define PIN_LORA_MISO  3
#define PIN_LORA_RST   8
#define PIN_LORA_BUSY  9
#define PIN_LORA_DIO1  10

// ── L76K GNSS (UART) ──
#define PIN_GPS_TX     0   // MCU TX → GPS RX
#define PIN_GPS_RX     1   // MCU RX ← GPS TX
#define GPS_BAUD_RATE  9600
#define PIN_GPS_SLEEP  2   // LOW = sleep, HIGH = wake
#define PIN_GPS_RESET  6   // Active LOW reset

// ── Quectel BG77 NB-IoT (UART) ──
#define PIN_CELL_TX    A0  // MCU TX → BG77 RX
#define PIN_CELL_RX    A1  // MCU RX ← BG77 TX
#define PIN_CELL_PWR   A2  // Power key
#define PIN_CELL_RST   A3  // Reset

// ── User Interface ──
#define PIN_LED        LED_BUILTIN
#define PIN_BUTTON     D10  // Provision / wake button
#define PIN_BUZZER     A4   // Passive piezo buzzer (PWM)

// ── NFC (provisioning) ──
// NFC pins are fixed on nRF52840 (P0.09 / P0.10)
// Managed by the NFC peripheral, not GPIO

#endif // COLLAR_PINS_H
