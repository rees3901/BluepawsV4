#pragma once

// BluePawzTransmitter 1b2ef636: ESP32-S3 B2B edition ONLY.
// GPIO numbers, not XIAO Dx labels; do not use the nRF52840 module pin map.
constexpr int RADIO_NSS = 41, RADIO_SCK = 7, RADIO_MOSI = 9, RADIO_MISO = 8;
constexpr int RADIO_RST = 42, RADIO_BUSY = 40, RADIO_DIO1 = 39;
constexpr int GNSS_RX = 44, GNSS_TX = 43, GNSS_WAKE = 1;
constexpr int USER_BUTTON = 21;
// Legacy assembly uses GPIO48. Never substitute LED_BUILTIN (GPIO21/button).
constexpr int USER_LED = 48;
constexpr int GNSS_BAUD = 9600;
