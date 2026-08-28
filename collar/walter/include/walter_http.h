#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

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
} // namespace walter
