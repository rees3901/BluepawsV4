"""LED cloud relay support, patched only into the generated personal hub source."""
PATCHES = [
    (r"""
// Command building & ACK tracking
static uint16_t sendCommand(uint16_t target_id, bp_pkt_type_t type, bp_profile_t mode);
static uint16_t sendCommandFind(uint16_t target_id, bp_pkt_type_t type,
                              bp_profile_t mode, uint8_t ledFlash,
                              bp_buzzer_pattern_t buzzerPattern,
                              uint16_t sequenceOverride = 0, uint32_t initialAgeMs = 0);
static bool queueCloudCommandResponse(const String &response, uint16_t expectedDeviceId);
static uint16_t sendStatusCommand(uint16_t target_id);
static void broadcastCommand(const pending_cmd_t &cmd);
""", r"""
// Command building & ACK tracking
static uint16_t sendCommand(uint16_t target_id, bp_pkt_type_t type, bp_profile_t mode);
#include "led_find_command.h"
static uint16_t sendCommandFind(uint16_t target_id, bp_pkt_type_t type,
                              bp_profile_t mode, uint8_t ledFlash,
                              bp_buzzer_pattern_t buzzerPattern,
                              uint16_t sequenceOverride = 0, uint32_t initialAgeMs = 0,
                              uint8_t ledAction = 255, uint16_t ledDuration = 0, uint16_t ledInterval = 60);
static bool queueCloudCommandResponse(const String &response, uint16_t expectedDeviceId);
static uint16_t sendStatusCommand(uint16_t target_id);
static void broadcastCommand(const pending_cmd_t &cmd);
"""),
    (r"""static uint16_t sendCommandFind(uint16_t target_id, bp_pkt_type_t type,
                              bp_profile_t mode, uint8_t ledFlash,
                              bp_buzzer_pattern_t buzzerPattern,
                              uint16_t sequenceOverride, uint32_t initialAgeMs) {
    if (initialAgeMs >= LOCAL_COMMAND_TTL_MS) return 0;
    cmd_entry_t cmd;
    uint8_t txReason = (uint8_t)type;      // Temporary downlink compatibility mapping
""", r"""static uint16_t sendCommandFind(uint16_t target_id, bp_pkt_type_t type,
                              bp_profile_t mode, uint8_t ledFlash,
                              bp_buzzer_pattern_t buzzerPattern,
                              uint16_t sequenceOverride, uint32_t initialAgeMs,
                              uint8_t ledAction, uint16_t ledDuration, uint16_t ledInterval) {
    if (initialAgeMs >= LOCAL_COMMAND_TTL_MS) return 0;
    cmd_entry_t cmd;
    uint8_t txReason = (uint8_t)type;      // Temporary downlink compatibility mapping
"""),
    (r"""        pkt_add_tlv_u8(cmd.buf, TLV_BUZZER_PATTERN, (uint8_t)buzzerPattern);  // Which sound to play
    }

    // Finalize: append the v1.2 auth-tag bytes and return total packet length.
    cmd.len = pkt_finalize(cmd.buf);
    cmd.targetId = target_id;
""", r"""        pkt_add_tlv_u8(cmd.buf, TLV_BUZZER_PATTERN, (uint8_t)buzzerPattern);  // Which sound to play
    }

    if (ledAction <= LED_STOP) {
        pkt_add_tlv_u8(cmd.buf, TLV_LED_ACTION, ledAction);
        pkt_add_tlv_u16(cmd.buf, TLV_LED_DURATION, ledDuration);
        pkt_add_tlv_u16(cmd.buf, TLV_LED_INTERVAL, ledInterval);
    }
    // Finalize: append the v1.2 auth-tag bytes and return total packet length.
    cmd.len = pkt_finalize(cmd.buf);
    cmd.targetId = target_id;
"""),
    (r"""        profile = bp_profile_from_name(fallback);
    }

    if (profile == PROFILE_UNKNOWN) {
        Serial.printf("[CLOUD CMD] Unsupported command '%s' for device %u\n", type, expectedDeviceId);
        return false;
    }
""", r"""        profile = bp_profile_from_name(fallback);
    }

    const bool ledFind = strcmp(type, "led_find") == 0;
    if (profile == PROFILE_UNKNOWN && !ledFind) {
        Serial.printf("[CLOUD CMD] Unsupported command '%s' for device %u\n", type, expectedDeviceId);
        return false;
    }
"""),
    (r"""    time_t expiry = mktime(&expiryTm);
    if (expiry <= now) return false;
    uint32_t remaining = (uint32_t)std::min<time_t>(600, expiry - now) * 1000;
    if (!sendCommandFind(expectedDeviceId, PKT_CMD_MODE, profile, 0, BUZZER_OFF,
                         sequence, LOCAL_COMMAND_TTL_MS - remaining)) return false;
    Serial.printf("[CLOUD CMD] Queued %s seq=%u for device %u\n",
                  bp_profile_name(profile), sequence, expectedDeviceId);
    return true;
""", r"""    time_t expiry = mktime(&expiryTm);
    if (expiry <= now) return false;
    uint32_t remaining = (uint32_t)std::min<time_t>(600, expiry - now) * 1000;
    if (ledFind) {
        const char* action = command["payload"]["action"] | "";
        uint8_t ledAction = strcmp(action,"flash") == 0 ? LED_FLASH_NOW :
            strcmp(action,"repeat") == 0 ? LED_REPEAT : strcmp(action,"stop") == 0 ? LED_STOP : 255;
        uint16_t duration = command["payload"]["duration_s"] | 0;
        uint16_t interval = command["payload"]["interval_s"] | 60;
        if (ledAction == 255 || (ledAction == LED_REPEAT && (duration < 10 || duration > 14400 || interval < 10 || interval > 600))) return false;
        // CONFIG without PROFILE: older firmware rejects without a misleading Find ACK.
        if (!sendCommandFind(expectedDeviceId, PKT_CMD_MODE, PROFILE_UNKNOWN, 0, BUZZER_OFF,
                             sequence, LOCAL_COMMAND_TTL_MS - remaining, ledAction, duration, interval)) return false;
    } else if (!sendCommandFind(expectedDeviceId, PKT_CMD_MODE, profile, 0, BUZZER_OFF,
                               sequence, LOCAL_COMMAND_TTL_MS - remaining)) return false;
    Serial.printf("[CLOUD CMD] Queued %s seq=%u for device %u\n",
                  bp_profile_name(profile), sequence, expectedDeviceId);
    return true;
"""),
]
HEADER = r"""#pragma once
#include <stdint.h>
// LED extension of TX_CONFIG; requires LED Find-compatible firmware.
constexpr uint8_t TLV_LED_ACTION = 0xFA, TLV_LED_DURATION = 0xFB, TLV_LED_INTERVAL = 0xFC;
constexpr uint8_t LED_FLASH_NOW = 0, LED_REPEAT = 1, LED_STOP = 2;
"""
def patch_led_cloud(text, private):
    for old, new in PATCHES:
        if text.count(old) != 1:
            raise RuntimeError("LED cloud adapter upstream contract changed")
        text = text.replace(old, new, 1)
    (private / "led_find_command.h").write_text(HEADER, encoding="utf-8")
    return text
