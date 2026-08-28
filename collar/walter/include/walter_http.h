#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>
#include <bp_config.h>

namespace walter {
inline void fillRequest(JsonDocument& request, const char* base64) {
    request["format"] = "tlv";
    request["ingest_path"] = "cellular_direct";
    request["link_type"] = "lte";
    request["payload_b64"] = base64;
}

// A modem OK or HTTP 2xx is not evidence of ingestion of this packet.
inline bool acceptedReceipt(JsonVariantConst receipt, uint16_t device, uint16_t sequence, const char* hash) {
    return receipt["accepted"].is<bool>() && receipt["accepted"].as<bool>() &&
        receipt["device_id"].is<unsigned>() && receipt["device_id"].as<unsigned>() == device &&
        receipt["message_id"].is<unsigned>() && receipt["message_id"].as<unsigned>() == sequence &&
        receipt["payload_hash"].is<const char*>() && strcmp(receipt["payload_hash"], hash) == 0 &&
        receipt["ingest_path"] == "cellular_direct" && receipt["link_type"] == "lte";
}

struct ProfileCommand {
    char id[37]{};
    uint16_t sequence = 0;
    bp_profile_t profile = PROFILE_UNKNOWN;
};

// Commands arrive over certificate-verified HTTPS, scoped by this collar's
// bearer. Never coerce malformed types, execute expired commands, or ACK an
// unsupported operation just because the HTTP request succeeded.
inline bool parseProfileCommand(JsonVariantConst response, uint16_t device, uint32_t now, ProfileCommand& out) {
    if (!now || !response["device_id"].is<unsigned>() || response["device_id"].as<unsigned>() != device
        || !response["command_pending"].is<bool>() || !response["command_pending"].as<bool>()) return false;
    const auto command = response["command"];
    if (!command["sequence_id"].is<unsigned>() || !command["expires_unix"].is<uint32_t>()
        || command["expires_unix"].as<uint32_t>() <= now) return false;
    const auto sequence = command["sequence_id"].as<unsigned>();
    if (!sequence || sequence > UINT16_MAX || !command["id"].is<const char*>()) return false;
    const char* id = command["id"];
    if (strlen(id) != 36) return false;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f') || (id[i] >= 'A' && id[i] <= 'F'))) return false;
    }
    bp_profile_t profile = PROFILE_UNKNOWN;
    if (command["type"] == "set_profile" && command["payload"]["profile"].is<const char*>())
        profile = bp_profile_from_name(command["payload"]["profile"]);
    else if (command["type"] == "enter_lost_alert") profile = PROFILE_LOST;
    else if (command["type"] == "exit_lost_alert") {
        if (!command["payload"]["fallback_profile"].isNull()
            && !command["payload"]["fallback_profile"].is<const char*>()) return false;
        profile = bp_profile_from_name(command["payload"]["fallback_profile"] | "active");
        if (profile != PROFILE_NORMAL && profile != PROFILE_POWERSAVE && profile != PROFILE_ACTIVE) return false;
    }
    if (profile == PROFILE_UNKNOWN) return false;
    memcpy(out.id, id, sizeof(out.id)); out.sequence = uint16_t(sequence); out.profile = profile;
    return true;
}

inline bool commandPollReceipt(JsonVariantConst response, uint16_t device) {
    return response["format"] == "device_commands" && response["device_id"].is<unsigned>()
        && response["device_id"].as<unsigned>() == device && response["command_pending"].is<bool>();
}

inline bool parseHttpBody(JsonDocument& response, const uint8_t* bytes, size_t capacity, size_t advertised) {
    const size_t length = strnlen(reinterpret_cast<const char*>(bytes), capacity);
    if (!length || length >= capacity || (advertised && advertised != length)) return false;
    response.clear();
    return !deserializeJson(response, bytes, length);
}
} // namespace walter
