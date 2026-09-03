#include "bluepaws/qr_payload.h"

#include <cctype>
#include <cstring>

namespace bluepaws::qr {
namespace {

bool copy_unescaped(const char *begin, const char *end, char *output, std::size_t capacity)
{
    if (capacity == 0) return false;
    std::size_t used = 0;
    bool escaped = false;
    for (const char *cursor = begin; cursor < end; ++cursor) {
        const char value = *cursor;
        if (!escaped && value == '\\') {
            escaped = true;
            continue;
        }
        if (used + 1 >= capacity) return false;
        output[used++] = value;
        escaped = false;
    }
    if (escaped) return false;
    output[used] = '\0';
    return true;
}

const char *field_end(const char *value)
{
    bool escaped = false;
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        if (!escaped && *cursor == ';') return cursor;
        if (!escaped && *cursor == '\\') {
            escaped = true;
        } else {
            escaped = false;
        }
    }
    return nullptr;
}

bool equals_ignore_case(const char *value, const char *expected)
{
    while (*value != '\0' && *expected != '\0') {
        if (std::tolower(static_cast<unsigned char>(*value)) !=
            std::tolower(static_cast<unsigned char>(*expected))) return false;
        ++value;
        ++expected;
    }
    return *value == '\0' && *expected == '\0';
}

bool parse_wifi(const char *payload, ParsedPayload &parsed)
{
    WifiCredentials wifi{};
    bool have_ssid = false;
    const char *cursor = payload + 5;
    while (*cursor != '\0') {
        if (*cursor == ';') {
            ++cursor;
            continue;
        }
        if (cursor[0] == '\0' || cursor[1] != ':') return false;
        const char key = cursor[0];
        const char *value = cursor + 2;
        const char *end = field_end(value);
        if (end == nullptr) return false;
        char decoded[hub::kWifiPasswordBytes]{};
        if (!copy_unescaped(value, end, decoded, sizeof(decoded))) return false;
        switch (key) {
        case 'S':
            if (std::strlen(decoded) >= sizeof(wifi.ssid)) return false;
            std::strcpy(wifi.ssid, decoded);
            have_ssid = true;
            break;
        case 'P':
            if (std::strlen(decoded) >= sizeof(wifi.password)) return false;
            std::strcpy(wifi.password, decoded);
            break;
        case 'T':
            if (equals_ignore_case(decoded, "WPA") || equals_ignore_case(decoded, "WPA2") ||
                equals_ignore_case(decoded, "WPA3") || equals_ignore_case(decoded, "SAE")) {
                wifi.security = WifiSecurity::Wpa;
            } else if (equals_ignore_case(decoded, "WEP")) {
                // The P4 network adapter intentionally supports only modern
                // WPA-family and open networks.
                return false;
            } else if (equals_ignore_case(decoded, "nopass")) {
                wifi.security = WifiSecurity::Open;
            } else {
                return false;
            }
            break;
        case 'H':
            wifi.hidden = equals_ignore_case(decoded, "true");
            break;
        default:
            break;
        }
        cursor = end + 1;
    }
    if (!have_ssid || !hub::validSsid(wifi.ssid)) return false;
    if (wifi.security != WifiSecurity::Open && !hub::validPassword(wifi.password)) return false;
    parsed = {};
    parsed.type = PayloadType::Wifi;
    parsed.wifi = wifi;
    return true;
}

}  // namespace

bool parse(const char *payload, ParsedPayload &parsed)
{
    parsed = {};
    if (payload == nullptr) return false;
    if (std::strncmp(payload, "WIFI:", 5) == 0) return parse_wifi(payload, parsed);

    // Reserved BluePaws format for the collar-provisioning follow-up. Accepting
    // only the identifier here prevents arbitrary QR data entering provisioning.
    constexpr char prefix[] = "BLUEPAWS:COLLAR:";
    if (std::strncmp(payload, prefix, sizeof(prefix) - 1) == 0) {
        const char *id = payload + sizeof(prefix) - 1;
        const std::size_t length = std::strlen(id);
        if (length == 0 || length >= sizeof(parsed.collar_id)) return false;
        for (std::size_t i = 0; i < length; ++i) {
            const unsigned char value = static_cast<unsigned char>(id[i]);
            if (!std::isalnum(value) && value != '-' && value != '_') return false;
        }
        parsed.type = PayloadType::Collar;
        std::strcpy(parsed.collar_id, id);
        return true;
    }
    return false;
}

}  // namespace bluepaws::qr
