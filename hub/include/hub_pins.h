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

// ── Status LED ──
// Heltec Wireless Tracker V2 onboard white LED.
#ifndef LED_BUILTIN
#define LED_BUILTIN    18
#endif
#define PIN_LED        LED_BUILTIN

#endif // HUB_PINS_H
