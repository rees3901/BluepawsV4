/*
  Bluepaws V4 — Home Hub Pin Definitions
  Hardware: Heltec Wireless Tracker V2 / HTIT-Tracker V2.x
*/

#ifndef HUB_PINS_H
#define HUB_PINS_H

// ── SX1262 LoRa Radio (SPI) ──
#define PIN_LORA_NSS   8
#define PIN_LORA_SCK   9
#define PIN_LORA_MOSI  10
#define PIN_LORA_MISO  11
#define PIN_LORA_RST   12
#define PIN_LORA_BUSY  13
#define PIN_LORA_DIO1  14

// ── KCT8103L RF Front-End Module (Heltec Tracker V2.x) ──
// The Tracker V2 routes the SX1262 through an external PA/LNA switch.
// Power/enable must be driven or the SX1262 can initialise while the
// effective antenna path is not usable. Keep CTX LOW for RX, HIGH for TX.
#define PIN_FEM_VCTRL  7
#define PIN_FEM_CSD    4
#define PIN_FEM_CTX    5

// ── Status LED ──
// Heltec Wireless Tracker V2 onboard white LED.
#ifndef LED_BUILTIN
#define LED_BUILTIN    18
#endif
#define PIN_LED        LED_BUILTIN

// Heltec Tracker V2 UC6580: GNSS TX -> MCU RX33, GNSS RX <- MCU TX34.
// V2 Vext is active HIGH (not the older V1 active-low example).
#define PIN_GNSS_RX 33
#define PIN_GNSS_TX 34
#define PIN_GNSS_RESET 35
#define PIN_GNSS_POWER 3

#endif // HUB_PINS_H
