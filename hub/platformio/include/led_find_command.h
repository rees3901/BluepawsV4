#pragma once
#include <stdint.h>
// LED extension of TX_CONFIG; requires LED Find-compatible firmware.
constexpr uint8_t TLV_LED_ACTION = 0xFA, TLV_LED_DURATION = 0xFB, TLV_LED_INTERVAL = 0xFC;
constexpr uint8_t LED_FLASH_NOW = 0, LED_REPEAT = 1, LED_STOP = 2;
