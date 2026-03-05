/*
  Bluepaws V4 — Home Hub Pin Definitions
  Hardware: Seeed XIAO ESP32-S3 + SX1262
*/

#ifndef HUB_PINS_H
#define HUB_PINS_H

// ── SX1262 LoRa Radio (SPI) ──
#define PIN_LORA_NSS   41
#define PIN_LORA_SCK   7
#define PIN_LORA_MOSI  9
#define PIN_LORA_MISO  8
#define PIN_LORA_RST   42
#define PIN_LORA_BUSY  40
#define PIN_LORA_DIO1  39

// ── Status LED ──
#define PIN_LED        LED_BUILTIN

#endif // HUB_PINS_H
