/*
  ┌─────────────────────────────────────────────────────────────┐
  │  BLUEPAWS V4 — BINARY TLV PROTOCOL v2                      │
  │  Shared between collar (TX) and home hub (RX)               │
  │  This file is the single source of truth for both devices   │
  └─────────────────────────────────────────────────────────────┘

  Packet layout:
    [29-byte fixed header] + [0-33 bytes TLV] + [2-byte CRC-16]
    Minimum: 31 bytes (header + CRC, no TLV)
    Maximum: 64 bytes (header + 33 TLV + CRC)

  All multi-byte fields are little-endian via memcpy.
*/

#ifndef BP_PROTOCOL_H
#define BP_PROTOCOL_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

// ═══════════════════════════════════════════════
// Constants
// ═══════════════════════════════════════════════
#define BP_PROTOCOL_VERSION   2
#define BP_HEADER_SIZE        29
#define BP_MAX_TLV_SIZE       33
#define BP_CRC_SIZE           2
#define BP_MIN_PACKET_SIZE    (BP_HEADER_SIZE + BP_CRC_SIZE)                    // 31
#define BP_MAX_PACKET_SIZE    (BP_HEADER_SIZE + BP_MAX_TLV_SIZE + BP_CRC_SIZE)  // 64

// ═══════════════════════════════════════════════
// Status Enum (u8) — device state
// ═══════════════════════════════════════════════
enum bp_status_t : uint8_t {
    STATUS_UNKNOWN       = 0x00,
    STATUS_OUT_AND_ABOUT = 0x01,  // Outdoors with valid GPS
    STATUS_BLE_HOME      = 0x02,  // BLE home beacon detected
    STATUS_INVALID_GPS   = 0x03,  // GPS timeout / no fix
    STATUS_OK            = 0x04,  // General positive (ACKs, commands)
    STATUS_LOST_TIMEOUT  = 0x05,  // Lost mode auto-timed-out
};

// ═══════════════════════════════════════════════
// Flags Bitfield (u16)
// Bits 0-3: packet type, Bits 4-15: boolean flags
// ═══════════════════════════════════════════════
#define PKT_TYPE_MASK  0x000F

enum bp_pkt_type_t : uint16_t {
    PKT_TELEMETRY    = 0x0001,  // TX→RX  position / status
    PKT_MODE_ACK     = 0x0002,  // TX→RX  mode change ACK
    PKT_STATUS_RESP  = 0x0003,  // TX→RX  status query response
    PKT_ALERT        = 0x0004,  // TX→RX  alert notification
    PKT_CMD_MODE     = 0x0005,  // RX→TX  mode change command
    PKT_CMD_STATUS   = 0x0006,  // RX→TX  status request
};

#define FLAG_HAS_GPS    0x0010  // Packet contains valid GPS coordinates
#define FLAG_BLE_HOME   0x0020  // BLE home beacon detected this cycle
#define FLAG_GPS_WARM   0x0040  // GPS module in warm-start state
#define FLAG_CELLULAR   0x0080  // Packet originated via NB-IoT cellular

// ═══════════════════════════════════════════════
// Profile Enum (u8) — operating mode
// ═══════════════════════════════════════════════
enum bp_profile_t : uint8_t {
    PROFILE_UNKNOWN   = 0x00,
    PROFILE_NORMAL    = 0x01,
    PROFILE_POWERSAVE = 0x02,
    PROFILE_ACTIVE    = 0x03,
    PROFILE_LOST      = 0x04,
};

// ═══════════════════════════════════════════════
// TLV Type IDs (u8)
// Format: [type:u8][length:u8][value:length bytes]
// Unknown types are safely skipped by parsers.
// ═══════════════════════════════════════════════
enum bp_tlv_type_t : uint8_t {
    TLV_PROFILE        = 0x01,  // u8   — bp_profile_t
    TLV_TX_POWER       = 0x02,  // i8   — LoRa TX power in dBm
    TLV_SLEEP_INTERVAL = 0x03,  // u16  — sleep duration in seconds
    TLV_GPS_WARM       = 0x04,  // u8   — 0=cold, 1=warm
    TLV_HOME_CYCLES    = 0x05,  // u8   — consecutive BLE home cycles
    TLV_LOG_INFO       = 0x06,  // u16+u16 — log entries + size_kb
    // 0x07 reserved for future use
    TLV_LOST_MODE_S    = 0x08,  // u32  — seconds elapsed in lost mode
    TLV_NEW_MODE       = 0x09,  // u8   — bp_profile_t (mode reverted to)
    TLV_DURATION_S     = 0x0A,  // u32  — total duration in seconds
    TLV_CMD_MSG_ID     = 0x0B,  // u32  — msg_seq of command being ACK'd
};

// ═══════════════════════════════════════════════
// Device ID Convention
// ═══════════════════════════════════════════════
#define DEVICE_ID_HUB        0x0000   // Home hub (RX base station)
#define DEVICE_ID_BROADCAST  0xFFFF   // Broadcast to all collars

// Format device ID as display string
static char _bp_dev_name_buf[16];

static inline const char *bp_device_name(uint16_t id) {
    if (id == DEVICE_ID_HUB)
        return "Hub";
    snprintf(_bp_dev_name_buf, sizeof(_bp_dev_name_buf), "Collar_%04X", id);
    return _bp_dev_name_buf;
}

static inline uint16_t bp_device_id_from_name(const char *name) {
    if (strcmp(name, "Hub") == 0)
        return DEVICE_ID_HUB;
    if (strcmp(name, "broadcast") == 0)
        return DEVICE_ID_BROADCAST;
    if (strncmp(name, "Collar_", 7) == 0) {
        unsigned int id = 0;
        if (sscanf(name + 7, "%x", &id) == 1 && id > 0 && id <= 0xFFFE)
            return (uint16_t)id;
    }
    return 0;
}

// ═══════════════════════════════════════════════
// Profile Helpers
// ═══════════════════════════════════════════════
static inline bp_profile_t bp_profile_from_name(const char *name) {
    if (strcmp(name, "normal") == 0)       return PROFILE_NORMAL;
    if (strcmp(name, "powersave") == 0)    return PROFILE_POWERSAVE;
    if (strcmp(name, "active_find") == 0)  return PROFILE_ACTIVE;
    if (strcmp(name, "active") == 0)       return PROFILE_ACTIVE;   // legacy alias
    if (strcmp(name, "emergency_lost") == 0) return PROFILE_LOST;
    if (strcmp(name, "lost") == 0)         return PROFILE_LOST;     // legacy alias
    return PROFILE_UNKNOWN;
}

static inline const char *bp_profile_name(bp_profile_t p) {
    switch (p) {
    case PROFILE_NORMAL:    return "Normal";
    case PROFILE_POWERSAVE: return "PowerSave";
    case PROFILE_ACTIVE:    return "Active Find";
    case PROFILE_LOST:      return "Emergency Lost";
    default:                return "Unknown";
    }
}

static inline const char *bp_status_display(bp_status_t s) {
    switch (s) {
    case STATUS_OUT_AND_ABOUT: return "Out";
    case STATUS_BLE_HOME:      return "Home";
    case STATUS_INVALID_GPS:   return "Error";
    case STATUS_OK:            return "OK";
    case STATUS_LOST_TIMEOUT:  return "LostTimeout";
    default:                   return "Unknown";
    }
}

// ═══════════════════════════════════════════════
// CRC-16/CCITT-FALSE
// Polynomial: 0x1021, Init: 0xFFFF
// ═══════════════════════════════════════════════
static inline uint16_t bp_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
    }
    return crc;
}

// ═══════════════════════════════════════════════
// GPS Time → Unix Epoch Conversion
// ═══════════════════════════════════════════════
static inline uint32_t bp_gps_to_unix(uint16_t year, uint8_t month, uint8_t day,
                                       uint8_t hour, uint8_t minute, uint8_t second) {
    static const uint16_t mdays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    uint32_t y = year;
    uint32_t days = (y - 1970) * 365;
    days += (y - 1969) / 4;
    days -= (y - 1901) / 100;
    days += (y - 1601) / 400;
    days += mdays[month - 1];
    if (month > 2 && (y % 4 == 0) && ((y % 100 != 0) || (y % 400 == 0)))
        days++;
    days += day - 1;
    return days * 86400UL + hour * 3600UL + minute * 60UL + second;
}

// ═══════════════════════════════════════════════
// Header Field Offsets (for reference)
//
//  [0]      u8   version
//  [1-2]    u16  device_id
//  [3-6]    u32  msg_seq
//  [7-10]   u32  time_unix
//  [11]     u8   status
//  [12-13]  u16  flags
//  [14-17]  i32  lat_e7
//  [18-21]  i32  lon_e7
//  [22-23]  u16  batt_mV
//  [24-25]  u16  acc_m
//  [26-27]  u16  fix_age_s
//  [28]     u8   tlv_len
//
// ═══════════════════════════════════════════════

// ═══════════════════════════════════════════════
// Packet Builders
// ═══════════════════════════════════════════════

// Initialize header. Zeroes the full buffer first.
static inline void pkt_init(uint8_t *buf, uint16_t device_id,
                             uint32_t msg_seq, uint32_t time_unix,
                             uint8_t status, uint16_t flags) {
    memset(buf, 0, BP_MAX_PACKET_SIZE);
    buf[0] = BP_PROTOCOL_VERSION;
    memcpy(&buf[1],  &device_id, 2);
    memcpy(&buf[3],  &msg_seq,   4);
    memcpy(&buf[7],  &time_unix, 4);
    buf[11] = status;
    memcpy(&buf[12], &flags,     2);
}

// Set GPS coordinate fields in header
static inline void pkt_set_gps(uint8_t *buf, int32_t lat_e7, int32_t lon_e7) {
    memcpy(&buf[14], &lat_e7, 4);
    memcpy(&buf[18], &lon_e7, 4);
}

// Set battery and GPS quality fields in header
static inline void pkt_set_quality(uint8_t *buf, uint16_t batt_mV,
                                    uint16_t acc_m, uint16_t fix_age_s) {
    memcpy(&buf[22], &batt_mV,    2);
    memcpy(&buf[24], &acc_m,      2);
    memcpy(&buf[26], &fix_age_s,  2);
}

// ── TLV Appenders ──
// Each reads tlv_len from buf[28], appends entry, updates tlv_len.
// Returns true on success, false on overflow.

static inline bool pkt_add_tlv_u8(uint8_t *buf, uint8_t type, uint8_t val) {
    uint8_t off = BP_HEADER_SIZE + buf[28];
    if (buf[28] + 3 > BP_MAX_TLV_SIZE)
        return false;
    buf[off]     = type;
    buf[off + 1] = 1;
    buf[off + 2] = val;
    buf[28] += 3;
    return true;
}

static inline bool pkt_add_tlv_i8(uint8_t *buf, uint8_t type, int8_t val) {
    return pkt_add_tlv_u8(buf, type, (uint8_t)val);
}

static inline bool pkt_add_tlv_u16(uint8_t *buf, uint8_t type, uint16_t val) {
    uint8_t off = BP_HEADER_SIZE + buf[28];
    if (buf[28] + 4 > BP_MAX_TLV_SIZE)
        return false;
    buf[off]     = type;
    buf[off + 1] = 2;
    memcpy(&buf[off + 2], &val, 2);
    buf[28] += 4;
    return true;
}

static inline bool pkt_add_tlv_u32(uint8_t *buf, uint8_t type, uint32_t val) {
    uint8_t off = BP_HEADER_SIZE + buf[28];
    if (buf[28] + 6 > BP_MAX_TLV_SIZE)
        return false;
    buf[off]     = type;
    buf[off + 1] = 4;
    memcpy(&buf[off + 2], &val, 4);
    buf[28] += 6;
    return true;
}

// Combined log info: u16 entries + u16 size_kb (4 value bytes)
static inline bool pkt_add_tlv_log_info(uint8_t *buf, uint16_t entries, uint16_t size_kb) {
    uint8_t off = BP_HEADER_SIZE + buf[28];
    if (buf[28] + 6 > BP_MAX_TLV_SIZE)
        return false;
    buf[off]     = TLV_LOG_INFO;
    buf[off + 1] = 4;
    memcpy(&buf[off + 2], &entries, 2);
    memcpy(&buf[off + 4], &size_kb, 2);
    buf[28] += 6;
    return true;
}

// Finalize: compute CRC over header+TLV, append 2-byte CRC. Returns total length.
static inline uint8_t pkt_finalize(uint8_t *buf) {
    uint8_t payload_len = BP_HEADER_SIZE + buf[28];
    uint16_t crc = bp_crc16(buf, payload_len);
    memcpy(&buf[payload_len], &crc, 2);
    return payload_len + BP_CRC_SIZE;
}

// ═══════════════════════════════════════════════
// Packet Parsers
// ═══════════════════════════════════════════════

// Validate CRC integrity
static inline bool pkt_validate_crc(const uint8_t *buf, uint8_t total_len) {
    if (total_len < BP_MIN_PACKET_SIZE)
        return false;
    uint8_t tlv_len = buf[28];
    uint8_t expected_len = BP_HEADER_SIZE + tlv_len + BP_CRC_SIZE;
    if (total_len < expected_len)
        return false;
    uint8_t payload_len = BP_HEADER_SIZE + tlv_len;
    uint16_t computed = bp_crc16(buf, payload_len);
    uint16_t received;
    memcpy(&received, &buf[payload_len], 2);
    return computed == received;
}

// ── Header Field Accessors ──
static inline uint8_t  pkt_version(const uint8_t *b)   { return b[0]; }
static inline uint16_t pkt_device_id(const uint8_t *b)  { uint16_t v; memcpy(&v, &b[1],  2); return v; }
static inline uint32_t pkt_msg_seq(const uint8_t *b)    { uint32_t v; memcpy(&v, &b[3],  4); return v; }
static inline uint32_t pkt_time_unix(const uint8_t *b)  { uint32_t v; memcpy(&v, &b[7],  4); return v; }
static inline uint8_t  pkt_status(const uint8_t *b)     { return b[11]; }
static inline uint16_t pkt_flags(const uint8_t *b)      { uint16_t v; memcpy(&v, &b[12], 2); return v; }
static inline int32_t  pkt_lat_e7(const uint8_t *b)     { int32_t  v; memcpy(&v, &b[14], 4); return v; }
static inline int32_t  pkt_lon_e7(const uint8_t *b)     { int32_t  v; memcpy(&v, &b[18], 4); return v; }
static inline uint16_t pkt_batt_mV(const uint8_t *b)    { uint16_t v; memcpy(&v, &b[22], 2); return v; }
static inline uint16_t pkt_acc_m(const uint8_t *b)      { uint16_t v; memcpy(&v, &b[24], 2); return v; }
static inline uint16_t pkt_fix_age_s(const uint8_t *b)  { uint16_t v; memcpy(&v, &b[26], 2); return v; }
static inline uint8_t  pkt_tlv_len(const uint8_t *b)    { return b[28]; }
static inline uint16_t pkt_pkt_type(const uint8_t *b)   { return pkt_flags(b) & PKT_TYPE_MASK; }

// ── TLV Search ──
// Finds a TLV by type. Returns pointer to value and its length.
static inline bool pkt_tlv_find(const uint8_t *buf, uint8_t tlv_type,
                                 const uint8_t **value, uint8_t *vlen) {
    uint8_t tlen = buf[28];
    uint8_t pos = 0;
    const uint8_t *tlv = &buf[BP_HEADER_SIZE];
    while (pos + 2 <= tlen) {
        uint8_t t = tlv[pos];
        uint8_t l = tlv[pos + 1];
        if (pos + 2 + l > tlen)
            break;  // malformed
        if (t == tlv_type) {
            *value = &tlv[pos + 2];
            *vlen  = l;
            return true;
        }
        pos += 2 + l;
    }
    return false;
}

// ── Typed TLV Extractors ──
static inline bool pkt_tlv_get_u8(const uint8_t *buf, uint8_t type, uint8_t *out) {
    const uint8_t *v; uint8_t l;
    if (!pkt_tlv_find(buf, type, &v, &l) || l < 1) return false;
    *out = v[0];
    return true;
}

static inline bool pkt_tlv_get_i8(const uint8_t *buf, uint8_t type, int8_t *out) {
    return pkt_tlv_get_u8(buf, type, (uint8_t *)out);
}

static inline bool pkt_tlv_get_u16(const uint8_t *buf, uint8_t type, uint16_t *out) {
    const uint8_t *v; uint8_t l;
    if (!pkt_tlv_find(buf, type, &v, &l) || l < 2) return false;
    memcpy(out, v, 2);
    return true;
}

static inline bool pkt_tlv_get_u32(const uint8_t *buf, uint8_t type, uint32_t *out) {
    const uint8_t *v; uint8_t l;
    if (!pkt_tlv_find(buf, type, &v, &l) || l < 4) return false;
    memcpy(out, v, 4);
    return true;
}

static inline bool pkt_tlv_get_log_info(const uint8_t *buf, uint16_t *entries, uint16_t *size_kb) {
    const uint8_t *v; uint8_t l;
    if (!pkt_tlv_find(buf, TLV_LOG_INFO, &v, &l) || l < 4) return false;
    memcpy(entries, v, 2);
    memcpy(size_kb, v + 2, 2);
    return true;
}

// ═══════════════════════════════════════════════
// Debug: Hex dump a packet to Serial
// ═══════════════════════════════════════════════
#ifdef ARDUINO
#include <Arduino.h>
static inline void pkt_print_hex(const uint8_t *buf, uint8_t len) {
    Serial.printf("[PKT] %d bytes: ", len);
    for (uint8_t i = 0; i < len; i++) {
        Serial.printf("%02X ", buf[i]);
    }
    Serial.println();
}
#endif

#endif // BP_PROTOCOL_H
