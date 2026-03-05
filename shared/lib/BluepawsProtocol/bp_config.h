/*
  ┌─────────────────────────────────────────────────────────────┐
  │  BLUEPAWS V4 — SHARED CONFIGURATION                         │
  │  LoRa radio parameters, operating profiles, timing          │
  │  Referenced by both collar and hub firmware                  │
  └─────────────────────────────────────────────────────────────┘
*/

#ifndef BP_CONFIG_H
#define BP_CONFIG_H

#include "bp_protocol.h"

// ═══════════════════════════════════════════════
// LoRa Radio Parameters (must match on TX and RX)
// ═══════════════════════════════════════════════
#define LORA_FREQUENCY       915.0f   // MHz (US ISM band; EU: 868.0)
#define LORA_SPREADING       8        // SF8
#define LORA_BANDWIDTH       250.0f   // kHz
#define LORA_CODING_RATE     5        // 4/5
#define LORA_PREAMBLE_LEN    16
#define LORA_SYNC_WORD       0x12     // Private network
#define LORA_CRC_ENABLED     true

// Listen Before Talk (collision avoidance)
#define LORA_LBT_ENABLED     true
#define LORA_LBT_RSSI_THRESH -80      // dBm
#define LORA_LBT_RETRIES     5
#define LORA_LBT_BACKOFF_MIN 50       // ms
#define LORA_LBT_BACKOFF_MAX 500      // ms

// AES-128 encryption key (16 bytes)
// Override per-deployment via build flags or provisioning
#ifndef LORA_AES_KEY
#define LORA_AES_KEY { \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  \
}
#endif

// ═══════════════════════════════════════════════
// Operating Profile Definitions
// ═══════════════════════════════════════════════

struct bp_profile_config_t {
    bp_profile_t profile;
    int8_t       tx_power_dBm;
    uint16_t     sleep_interval_s;
    uint8_t      led_flashes;
    bool         beacon_enabled;
};

// Profile lookup table
static const bp_profile_config_t BP_PROFILES[] = {
    // profile            power  interval  led  beacon
    { PROFILE_NORMAL,      19,    600,      5,   false },  // 10 min
    { PROFILE_POWERSAVE,   10,   1200,      5,   false },  // 20 min
    { PROFILE_ACTIVE,      19,     60,      5,   false },  //  1 min
    { PROFILE_LOST,        22,     30,     10,   true  },  // 30 sec
};

#define BP_PROFILE_COUNT  (sizeof(BP_PROFILES) / sizeof(BP_PROFILES[0]))

// Lookup profile config by enum value
static inline const bp_profile_config_t *bp_profile_config(bp_profile_t p) {
    for (size_t i = 0; i < BP_PROFILE_COUNT; i++) {
        if (BP_PROFILES[i].profile == p)
            return &BP_PROFILES[i];
    }
    return &BP_PROFILES[0];  // fallback to normal
}

// ═══════════════════════════════════════════════
// Lost Mode Safety
// ═══════════════════════════════════════════════
#define LOST_MODE_MAX_DURATION_S  7200   // 2 hours
#define LOST_MODE_FALLBACK        PROFILE_ACTIVE

// ═══════════════════════════════════════════════
// GPS Timing
// ═══════════════════════════════════════════════
#define GPS_COLD_START_TIMEOUT_S  60
#define GPS_WARM_START_TIMEOUT_S  20
#define GPS_STABILISATION_S       15
#define GPS_STALE_THRESHOLD_S     60

// ═══════════════════════════════════════════════
// BLE Home Beacon
// ═══════════════════════════════════════════════
#define BLE_SCAN_DURATION_S       10
#define BLE_HOME_BEACON_NAME      "BLUEPAWS_HOME"
#define BLE_HOME_CYCLE_THRESHOLD  5   // consecutive detections to confirm "home"

// ═══════════════════════════════════════════════
// NB-IoT Cellular
// ═══════════════════════════════════════════════
#define CELLULAR_TX_RATIO         10   // 1 cellular per N LoRa transmissions
#define CELLULAR_BAUD_RATE        115200

// ═══════════════════════════════════════════════
// Command Listen Window
// ═══════════════════════════════════════════════
#define CMD_LISTEN_WINDOW_MS      2000
#define CMD_QUEUE_INTERVAL_MS     3000  // rate limit between outbound commands

#endif // BP_CONFIG_H
