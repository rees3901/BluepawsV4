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
    uint16_t     sleep_interval_s;   // 0 = no sleep (lost mode)
    uint8_t      led_flashes;
    bool         beacon_enabled;     // LED beacon during active period
    bool         gps_continuous;     // keep GPS on between cycles
    uint8_t      cellular_ratio;     // 1 cellular per N cycles (0 = disabled)
};

// Profile lookup table
static const bp_profile_config_t BP_PROFILES[] = {
    //                        power  sleep   led  beacon  gps_cont  cell_ratio
    { PROFILE_NORMAL,          19,    600,    5,  false,  false,    10 },  // 10 min, cell 1:10
    { PROFILE_POWERSAVE,       10,   1800,    3,  false,  false,    30 },  // 30 min, cell 1:30
    { PROFILE_ACTIVE,          19,     60,    5,  false,  false,     5 },  // Active Find: 1 min, cell 1:5
    { PROFILE_LOST,            22,      0,   10,  true,   true,      3 },  // Emergency Lost: no sleep, cell 1:3
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
#define LOST_MODE_CYCLE_INTERVAL_S  30   // TX interval when in lost mode (no sleep)

// ═══════════════════════════════════════════════
// GPS Timing (two-phase acquisition)
//
// Phase 1 — TTFF (Time To First Fix):
//   Wake GPS, wait up to TTFF timeout for initial fix.
//   Cold start: up to 60s. Warm start: up to 20s.
//
// Phase 2 — Stabilisation:
//   Once initial fix is detected, wait an additional
//   period for the fix to stabilise before reading coords.
// ═══════════════════════════════════════════════
#define GPS_TTFF_COLD_TIMEOUT_S   20     // max wait for first fix (cold)
#define GPS_TTFF_WARM_TIMEOUT_S   15     // max wait for first fix (warm)
#define GPS_STABILISATION_S       10     // stabilisation after first fix
#define GPS_STALE_THRESHOLD_S     60     // fix older than this = stale

// ═══════════════════════════════════════════════
// BLE Home Beacon
// ═══════════════════════════════════════════════
#define BLE_SCAN_DURATION_S       10
#define BLE_HOME_BEACON_NAME      "BLUEPAWS_HOME"
#define BLE_HOME_CYCLE_THRESHOLD  5   // consecutive detections to confirm "home"

// ═══════════════════════════════════════════════
// NB-IoT Cellular
// ═══════════════════════════════════════════════
#define CELLULAR_BAUD_RATE        115200

// Sequans Monarch 2 GM02SP Power Saving Mode (PSM) — collar sleep periods
// TAU (T3412): periodic tracking area update timer
// Active timer (T3324): time modem stays reachable after TAU
#define CELLULAR_PSM_TAU          "10100010"  // 12 hours (binary coded)
#define CELLULAR_PSM_ACTIVE       "00000101"  // 10 seconds

// eDRX (Extended Discontinuous Reception)
// Cycle length for paging windows when modem is idle
#define CELLULAR_EDRX_VALUE       "0010"      // 20.48 seconds
#define CELLULAR_EDRX_PTW         "0000"      // 1.28 seconds paging window

// ═══════════════════════════════════════════════
// Command Listen Window
// ═══════════════════════════════════════════════
#define CMD_LISTEN_WINDOW_MS      2000
#define CMD_QUEUE_INTERVAL_MS     3000  // rate limit between outbound commands

#endif // BP_CONFIG_H
