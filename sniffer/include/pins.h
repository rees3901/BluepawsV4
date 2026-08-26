#pragma once

// Heltec Vision Master T190 hardware map. The SX1262 and TFT use separate SPI
// buses, so neither peripheral may borrow the other peripheral's pins.
#define SNIFFER_LORA_SCK   9
#define SNIFFER_LORA_MISO  11
#define SNIFFER_LORA_MOSI  10
#define SNIFFER_LORA_NSS   8
#define SNIFFER_LORA_DIO1  14
#define SNIFFER_LORA_RST   12
#define SNIFFER_LORA_BUSY  13

#define SNIFFER_TFT_SCK    38
#define SNIFFER_TFT_MOSI   48
#define SNIFFER_TFT_CS     39
#define SNIFFER_TFT_DC     47
#define SNIFFER_TFT_RST    40
#define SNIFFER_TFT_BL     17
#define SNIFFER_TFT_POWER  7

#define SNIFFER_HEARTBEAT  35
#define SNIFFER_USER_BTN   21
#define SNIFFER_BOOT_BTN   0
