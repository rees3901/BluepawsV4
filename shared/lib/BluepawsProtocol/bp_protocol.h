/*
  BLUEPAWS V4 - TLV TELEMETRY PROTOCOL v1.2
  Shared collar/home-hub binary packet contract
  Canonical source: docs/TLV_PROTOCOL_V1_2.md

  Layout:
    [32-byte fixed header] + [0-24 bytes TLV] + [8-byte HMAC tag]
    Minimum 40 bytes, maximum 64 bytes.

  v1.2 adds source_id16 + destination_id16 while keeping the header at 32
  bytes. The on-wire version byte is 2 because field offsets changed.
*/

#ifndef BP_PROTOCOL_H
#define BP_PROTOCOL_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define BP_PROTOCOL_VERSION   2
#define BP_HEADER_SIZE        32
#define BP_MAX_TLV_SIZE       24
#define BP_AUTH_TAG_SIZE      8
#define BP_MIN_PACKET_SIZE    (BP_HEADER_SIZE + BP_AUTH_TAG_SIZE)
#define BP_MAX_PACKET_SIZE    (BP_HEADER_SIZE + BP_MAX_TLV_SIZE + BP_AUTH_TAG_SIZE)

#define BP_DEST_CLOUD         0x0000
#define BP_ID_BROADCAST       0xFFFF
#define BP_DEFAULT_HUB_ID     0x0010

/* Historical compatibility names. Do not use DEVICE_ID_HUB for provisioning
   new hubs. A real hub must have its own non-zero multiple-of-16 source ID. */
#define DEVICE_ID_HUB         BP_DEFAULT_HUB_ID
#define DEVICE_ID_BROADCAST   BP_ID_BROADCAST

static inline bool bp_is_hub_id(uint16_t id) {
    return id != BP_DEST_CLOUD && id != BP_ID_BROADCAST && (id & 0x000F) == 0;
}

static inline bool bp_is_collar_id(uint16_t id) {
    return id != BP_DEST_CLOUD && id != BP_ID_BROADCAST && (id & 0x000F) != 0;
}

enum bp_status_t : uint8_t {
    STATUS_HOME          = 0x00,
    STATUS_OUT_AND_ABOUT = 0x01,
    STATUS_LOST          = 0x02,
    STATUS_ERROR         = 0x03,
    STATUS_UNKNOWN       = STATUS_HOME,
    STATUS_BLE_HOME      = STATUS_HOME,
    STATUS_INVALID_GPS   = STATUS_ERROR,
    STATUS_OK            = STATUS_HOME,
    STATUS_LOST_TIMEOUT  = STATUS_LOST,
};

enum bp_profile_t : uint8_t {
    PROFILE_POWERSAVE = 0x00,
    PROFILE_NORMAL    = 0x01,
    PROFILE_ACTIVE    = 0x02,
    PROFILE_LOST      = 0x03,
    PROFILE_DEBUG     = 0x04,
    PROFILE_UNKNOWN   = 0xFF,
};

#define BP_MAX_POWER_PROFILE PROFILE_DEBUG

#define FLAG_GNSS_VALID        0x01
#define FLAG_FIX_3D            0x02
#define FLAG_LOW_BATTERY       0x04
#define FLAG_HOME_BEACON_SEEN  0x08
#define FLAG_GEOFENCE_BREACHED 0x10
#define FLAG_CHARGING          0x20
#define FLAG_STALE_FIX         0x40
#define FLAG_ERROR_PRESENT     0x80
#define FLAG_HAS_GPS           FLAG_GNSS_VALID
#define FLAG_BLE_HOME          FLAG_HOME_BEACON_SEEN
#define FLAG_GPS_WARM          FLAG_STALE_FIX
#define FLAG_CELLULAR          0x00

enum bp_tx_reason_t : uint8_t {
    TX_TELEMETRY    = 0,
    TX_ACK          = 1,
    TX_PING         = 2,
    TX_INTERRUPT    = 3,
    TX_BOOT         = 4,
    TX_ALERT        = 5,
    TX_CONFIG       = 6,
    TX_WAKE_CHECKIN = 7,
};

enum bp_pkt_type_t : uint8_t {
    PKT_TELEMETRY   = TX_TELEMETRY,
    PKT_MODE_ACK    = TX_ACK,
    PKT_STATUS_RESP = TX_ACK,
    PKT_ALERT       = TX_ALERT,
    PKT_CMD_MODE    = TX_CONFIG,
    PKT_CMD_STATUS  = TX_PING,
    PKT_CMD_FIND    = TX_INTERRUPT,
    PKT_FIND_ACK    = TX_ACK,
};

enum bp_error_t : uint8_t {
    BP_ERROR_NONE     = 0x00,
    BP_ERROR_GPS      = 0x01,
    BP_ERROR_RF       = 0x02,
    BP_ERROR_CELLULAR = 0x03,
    BP_ERROR_MODULE   = 0x04,
};

enum bp_tlv_type_t : uint8_t {
    TLV_FW_VER            = 0x04,  // u16  — firmware version, (major << 8) | minor
    TLV_RESET_REASON      = 0x06,  // u8   — reset/cold-start diagnostic
    TLV_UPTIME_S          = 0x10,  // u32  — seconds since boot
    TLV_ACTIVITY_SCORE    = 0x13,  // u8   — simple activity/movement score
    TLV_ACKED_MSG_SEQ_ID  = 0x20,  // u16  — command/message sequence being ACK'd

    TLV_PROFILE           = 0xF1,  // u8   — v1.2 downlink requested power profile

    // Compatibility aliases for old sketch call sites. Keep these out of
    // production packets unless the protocol document formally assigns them.
    TLV_TX_POWER          = 0xF2,
    TLV_SLEEP_INTERVAL    = 0xF3,
    TLV_GPS_WARM          = 0xF4,
    TLV_HOME_CYCLES       = 0xF5,
    TLV_LOG_INFO          = 0xF6,
    TLV_LOST_MODE_S       = TLV_UPTIME_S,
    TLV_NEW_MODE          = 0xF7,
    TLV_DURATION_S        = TLV_UPTIME_S,
    TLV_CMD_MSG_ID        = TLV_ACKED_MSG_SEQ_ID,
    TLV_LED_FLASH         = 0xF8,
    TLV_BUZZER_PATTERN    = 0xF9,
};

static char _bp_dev_name_buf[16];
static inline const char *bp_device_name(uint16_t id) {
    if (id == BP_DEST_CLOUD) return "Cloud";
    if (id == BP_ID_BROADCAST) return "Broadcast";
    snprintf(_bp_dev_name_buf, sizeof(_bp_dev_name_buf), bp_is_hub_id(id) ? "Hub_%04X" : "Collar_%04X", id);
    return _bp_dev_name_buf;
}

static inline uint16_t bp_device_id_from_name(const char *name) {
    if (strcmp(name, "Cloud") == 0 || strcmp(name, "cloud") == 0) return BP_DEST_CLOUD;
    if (strcmp(name, "broadcast") == 0 || strcmp(name, "Broadcast") == 0) return BP_ID_BROADCAST;
    unsigned int id = 0;
    if (strncmp(name, "Hub_", 4) == 0 && sscanf(name + 4, "%x", &id) == 1 && id <= 0xFFFE) return (uint16_t)id;
    if (strncmp(name, "Collar_", 7) == 0 && sscanf(name + 7, "%x", &id) == 1 && id <= 0xFFFE) return (uint16_t)id;
    return 0;
}

static inline bp_profile_t bp_profile_from_name(const char *name) {
    if (strcmp(name, "powersave") == 0 || strcmp(name, "power_save") == 0) return PROFILE_POWERSAVE;
    if (strcmp(name, "normal") == 0) return PROFILE_NORMAL;
    if (strcmp(name, "active_find") == 0 || strcmp(name, "active") == 0) return PROFILE_ACTIVE;
    if (strcmp(name, "emergency_lost") == 0 || strcmp(name, "lost") == 0 || strcmp(name, "lost_alert") == 0) return PROFILE_LOST;
    if (strcmp(name, "debug") == 0 || strcmp(name, "dev") == 0 || strcmp(name, "dev_debug") == 0) return PROFILE_DEBUG;
    return PROFILE_UNKNOWN;
}

static inline const char *bp_profile_name(bp_profile_t p) {
    switch (p) {
    case PROFILE_POWERSAVE: return "PowerSave";
    case PROFILE_NORMAL: return "Normal";
    case PROFILE_ACTIVE: return "Active";
    case PROFILE_LOST: return "Lost Alert";
    case PROFILE_DEBUG: return "Debug";
    default: return "Unknown";
    }
}

static inline const char *bp_status_display(bp_status_t s) {
    switch (s) {
    case STATUS_HOME: return "Home";
    case STATUS_OUT_AND_ABOUT: return "Out";
    case STATUS_LOST: return "Lost";
    case STATUS_ERROR: return "Error";
    default: return "Reserved";
    }
}

static inline const char *bp_tx_reason_display(uint8_t r) {
    switch (r) {
    case TX_TELEMETRY: return "Telemetry";
    case TX_ACK: return "ACK";
    case TX_PING: return "Ping";
    case TX_INTERRUPT: return "Interrupt";
    case TX_BOOT: return "Boot";
    case TX_ALERT: return "Alert";
    case TX_CONFIG: return "Config";
    case TX_WAKE_CHECKIN: return "WakeCheckIn";
    default: return "Reserved";
    }
}

static inline const char *bp_error_display(bp_error_t e) {
    switch (e) {
    case BP_ERROR_GPS: return "GPS";
    case BP_ERROR_RF: return "RF";
    case BP_ERROR_CELLULAR: return "Cellular";
    case BP_ERROR_MODULE: return "Module";
    default: return "None";
    }
}

static inline uint32_t bp_gps_to_unix(uint16_t year, uint8_t month, uint8_t day,
                                      uint8_t hour, uint8_t minute, uint8_t second) {
    static const uint16_t mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    uint32_t y = year;
    uint32_t days = (y - 1970) * 365;
    days += (y - 1969) / 4;
    days -= (y - 1901) / 100;
    days += (y - 1601) / 400;
    days += mdays[month - 1];
    if (month > 2 && (y % 4 == 0) && ((y % 100 != 0) || (y % 400 == 0))) days++;
    days += day - 1;
    return days * 86400UL + hour * 3600UL + minute * 60UL + second;
}

static inline uint8_t bp_pack_state(uint8_t status, uint8_t power_profile) {
    return (uint8_t)(((power_profile & 0x0F) << 4) | (status & 0x0F));
}

/* Canonical v1.2 initializer. */
static inline void pkt_init(uint8_t *buf, uint16_t source_id, uint16_t destination_id,
                            uint16_t msg_seq, uint32_t time_unix,
                            uint8_t status, uint8_t power_profile,
                            uint8_t flags, uint8_t tx_reason) {
    memset(buf, 0, BP_MAX_PACKET_SIZE);
    buf[0] = BP_PROTOCOL_VERSION;
    memcpy(&buf[1], &source_id, 2);
    memcpy(&buf[3], &destination_id, 2);
    memcpy(&buf[5], &msg_seq, 2);
    memcpy(&buf[7], &time_unix, 4);
    buf[11] = bp_pack_state(status, power_profile);
    buf[12] = flags;
    buf[13] = tx_reason;
}

/* Legacy compatibility overload: destination 0000.
   New collar uplinks must explicitly address their affiliated Home Hub.
   Commands and replies must explicitly address the intended participant. */
static inline void pkt_init(uint8_t *buf, uint16_t source_id,
                            uint16_t msg_seq, uint32_t time_unix,
                            uint8_t status, uint8_t power_profile,
                            uint8_t flags, uint8_t tx_reason) {
    pkt_init(buf, source_id, BP_DEST_CLOUD, msg_seq, time_unix, status, power_profile, flags, tx_reason);
}

static inline void pkt_init(uint8_t *buf, uint16_t source_id,
                            uint32_t msg_seq, uint32_t time_unix,
                            uint8_t status, uint8_t tx_reason) {
    pkt_init(buf, source_id, BP_DEST_CLOUD, (uint16_t)(msg_seq & 0xFFFF), time_unix,
             status, PROFILE_NORMAL, 0, tx_reason);
}

static inline void pkt_set_destination(uint8_t *buf, uint16_t destination_id) { memcpy(&buf[3], &destination_id, 2); }
static inline void pkt_set_gps(uint8_t *buf, int32_t lat_e7, int32_t lon_e7) { memcpy(&buf[14], &lat_e7, 4); memcpy(&buf[18], &lon_e7, 4); }
static inline void pkt_set_quality(uint8_t *buf, uint16_t batt_mV, uint16_t acc_m, uint16_t fix_age_s) { memcpy(&buf[22], &batt_mV, 2); memcpy(&buf[24], &acc_m, 2); memcpy(&buf[26], &fix_age_s, 2); }
static inline void pkt_set_sat_count(uint8_t *buf, uint8_t sat_count) { buf[28] = sat_count; }

static inline bool pkt_add_raw_tlv(uint8_t *buf, uint8_t type, const uint8_t *val, uint8_t len) {
    uint8_t tlv_len = buf[31];
    uint8_t off = BP_HEADER_SIZE + tlv_len;
    if ((uint16_t)tlv_len + 2 + len > BP_MAX_TLV_SIZE) return false;
    buf[off] = type; buf[off + 1] = len;
    if (len > 0 && val != nullptr) memcpy(&buf[off + 2], val, len);
    buf[31] = tlv_len + 2 + len;
    return true;
}

static inline bool pkt_add_tlv_u8(uint8_t *buf, uint8_t type, uint8_t val) { return pkt_add_raw_tlv(buf, type, &val, 1); }
static inline bool pkt_add_tlv_i8(uint8_t *buf, uint8_t type, int8_t val) { return pkt_add_tlv_u8(buf, type, (uint8_t)val); }
static inline bool pkt_add_tlv_u16(uint8_t *buf, uint8_t type, uint16_t val) { uint8_t raw[2]; memcpy(raw, &val, 2); return pkt_add_raw_tlv(buf, type, raw, 2); }
static inline bool pkt_add_tlv_u32(uint8_t *buf, uint8_t type, uint32_t val) { uint8_t raw[4]; memcpy(raw, &val, 4); return pkt_add_raw_tlv(buf, type, raw, 4); }
static inline bool pkt_add_tlv_log_info(uint8_t *buf, uint16_t entries, uint16_t size_kb) { uint8_t raw[4]; memcpy(&raw[0], &entries, 2); memcpy(&raw[2], &size_kb, 2); return pkt_add_raw_tlv(buf, TLV_LOG_INFO, raw, 4); }

static inline uint8_t pkt_finalize(uint8_t *buf) {
    uint8_t auth_off = BP_HEADER_SIZE + buf[31];
    memset(&buf[auth_off], 0, BP_AUTH_TAG_SIZE);
    return auth_off + BP_AUTH_TAG_SIZE;
}

static inline uint8_t  pkt_version(const uint8_t *b) { return b[0]; }
static inline uint16_t pkt_source_id(const uint8_t *b) { uint16_t v; memcpy(&v, &b[1], 2); return v; }
static inline uint16_t pkt_destination_id(const uint8_t *b) { uint16_t v; memcpy(&v, &b[3], 2); return v; }
static inline uint16_t pkt_device_id(const uint8_t *b) { return pkt_source_id(b); }
static inline uint16_t pkt_msg_seq(const uint8_t *b) { uint16_t v; memcpy(&v, &b[5], 2); return v; }
static inline uint32_t pkt_time_unix(const uint8_t *b) { uint32_t v; memcpy(&v, &b[7], 4); return v; }
static inline uint8_t pkt_state(const uint8_t *b) { return b[11]; }
static inline uint8_t pkt_status(const uint8_t *b) { return b[11] & 0x0F; }
static inline uint8_t pkt_power_profile(const uint8_t *b) { return (b[11] >> 4) & 0x0F; }
static inline uint8_t pkt_flags(const uint8_t *b) { return b[12]; }
static inline uint8_t pkt_tx_reason(const uint8_t *b) { return b[13]; }
static inline int32_t pkt_lat_e7(const uint8_t *b) { int32_t v; memcpy(&v, &b[14], 4); return v; }
static inline int32_t pkt_lon_e7(const uint8_t *b) { int32_t v; memcpy(&v, &b[18], 4); return v; }
static inline uint16_t pkt_batt_mV(const uint8_t *b) { uint16_t v; memcpy(&v, &b[22], 2); return v; }
static inline uint16_t pkt_acc_m(const uint8_t *b) { uint16_t v; memcpy(&v, &b[24], 2); return v; }
static inline uint16_t pkt_fix_age_s(const uint8_t *b) { uint16_t v; memcpy(&v, &b[26], 2); return v; }
static inline uint8_t pkt_sat_count(const uint8_t *b) { return b[28]; }
static inline uint8_t pkt_tlv_len(const uint8_t *b) { return b[31]; }
static inline uint8_t pkt_pkt_type(const uint8_t *b) { return pkt_tx_reason(b); }

static inline bool pkt_validate_structure(const uint8_t *buf, uint8_t total_len) {
    if (total_len < BP_MIN_PACKET_SIZE || total_len > BP_MAX_PACKET_SIZE) return false;
    if (buf[0] != BP_PROTOCOL_VERSION) return false;
    if (buf[29] != 0 || buf[30] != 0) return false;
    if (buf[31] > BP_MAX_TLV_SIZE) return false;
    if (total_len != BP_HEADER_SIZE + buf[31] + BP_AUTH_TAG_SIZE) return false;
    if ((pkt_status(buf) > STATUS_ERROR) || (pkt_power_profile(buf) > BP_MAX_POWER_PROFILE)) return false;
    if (pkt_tx_reason(buf) > TX_WAKE_CHECKIN) return false;
    uint16_t source = pkt_source_id(buf);
    if (!bp_is_hub_id(source) && !bp_is_collar_id(source)) return false;
    return true;
}

static inline bool pkt_validate_crc(const uint8_t *buf, uint8_t total_len) { return pkt_validate_structure(buf, total_len); }

static inline bool pkt_tlv_find(const uint8_t *buf, uint8_t tlv_type, const uint8_t **value, uint8_t *vlen) {
    uint8_t tlen = buf[31]; uint8_t pos = 0; const uint8_t *tlv = &buf[BP_HEADER_SIZE];
    while (pos + 2 <= tlen) {
        uint8_t t = tlv[pos], l = tlv[pos + 1];
        if (pos + 2 + l > tlen) break;
        if (t == tlv_type) { *value = &tlv[pos + 2]; *vlen = l; return true; }
        pos += 2 + l;
    }
    return false;
}

static inline bool pkt_tlv_get_u8(const uint8_t *buf, uint8_t type, uint8_t *out) { const uint8_t *v; uint8_t l; if (!pkt_tlv_find(buf, type, &v, &l) || l < 1) return false; *out = v[0]; return true; }
static inline bool pkt_tlv_get_i8(const uint8_t *buf, uint8_t type, int8_t *out) { return pkt_tlv_get_u8(buf, type, (uint8_t *)out); }
static inline bool pkt_tlv_get_u16(const uint8_t *buf, uint8_t type, uint16_t *out) { const uint8_t *v; uint8_t l; if (!pkt_tlv_find(buf, type, &v, &l) || l < 2) return false; memcpy(out, v, 2); return true; }
static inline bool pkt_tlv_get_u32(const uint8_t *buf, uint8_t type, uint32_t *out) { const uint8_t *v; uint8_t l; if (!pkt_tlv_find(buf, type, &v, &l) || l < 4) return false; memcpy(out, v, 4); return true; }
static inline bool pkt_tlv_get_log_info(const uint8_t *buf, uint16_t *entries, uint16_t *size_kb) { const uint8_t *v; uint8_t l; if (!pkt_tlv_find(buf, TLV_LOG_INFO, &v, &l) || l < 4) return false; memcpy(entries, v, 2); memcpy(size_kb, v + 2, 2); return true; }

#ifdef ARDUINO
#include <Arduino.h>
static inline void pkt_print_hex(const uint8_t *buf, uint8_t len) {
    Serial.printf("[PKT] %d bytes: ", len);
    for (uint8_t i = 0; i < len; i++) Serial.printf("%02X ", buf[i]);
    Serial.println();
}
#endif

#endif // BP_PROTOCOL_H
