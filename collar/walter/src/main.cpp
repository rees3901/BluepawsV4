#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WalterModem.h>
#include <atomic>
#include <cmath>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include "walter_config.h"
#include "walter_policy.h"
#include "walter_http.h"

namespace {
WalterModem modem;
Preferences sequenceStore;
QueueHandle_t gnssEvents, httpEvents;
std::atomic<bool> running{false}, busy{false}, cancelRequested{false}, oneShot{false};
std::atomic<bool> inspectRequested{false};
std::atomic<bool> rawDiagnosticRequested{false};
std::atomic<bool> gnssInspectionRequested{false};
std::atomic<uint8_t> selectedProfile{PROFILE_NORMAL};
std::atomic<bool> simulatedHome{false};
const uint8_t hmacKey[] = WALTER_HMAC_KEY_BYTES;
static_assert(sizeof(hmacKey) == 32, "HMAC key must contain exactly 32 bytes");
bool begun = false, configured = false, sequenceReady = false, cellularFailure = false;
uint32_t sequenceNext = 0, sequenceEnd = 0;
uint64_t clockAnchorMs = 0;
uint32_t clockAnchorUtc = 0;
walter::Fix lastFix;

uint64_t monotonicMs() { return uint64_t(esp_timer_get_time()) / 1000; }
uint32_t utcNow() {
    if (!clockAnchorUtc) return 0;
    const uint64_t now = clockAnchorUtc + (monotonicMs() - clockAnchorMs) / 1000;
    return walter::validUtc(now) ? uint32_t(now) : 0;
}
void setUtc(int64_t utc) {
    if (walter::plausibleUtc(utc, BLUEPAWS_BUILD_UNIX_TIME)) {
        clockAnchorUtc = uint32_t(utc); clockAnchorMs = monotonicMs();
    } else Serial.println("[CLOCK] Ignoring implausible modem/GNSS UTC");
}
bool pauseMs(uint32_t duration) {
    const auto until = monotonicMs() + duration;
    while (monotonicMs() < until) {
        if (cancelRequested.load()) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return !cancelRequested.load();
}
bool safeAtString(const char* value, size_t maxLength, bool required) {
    const size_t n = strlen(value);
    if ((required && !n) || n > maxLength) return false;
    for (size_t i = 0; i < n; ++i)
        if (value[i] < 32 || value[i] > 126 || value[i] == '"' || value[i] == '\\') return false;
    return true;
}
bool credentialsReady() {
    uint8_t any = 0;
    for (auto b : hmacKey) any |= b;
    return any && safeAtString(WALTER_APN, 99, true) &&
        safeAtString(WALTER_APN_USER, 63, WALTER_APN_AUTH != 0) &&
        safeAtString(WALTER_APN_PASSWORD, 63, WALTER_APN_AUTH != 0) &&
        safeAtString(WALTER_BEARER_TOKEN, 256, true) && strlen(WALTER_BEARER_TOKEN) >= 32 &&
        strstr(WALTER_TLS_CA_PEM, "-----BEGIN CERTIFICATE-----") &&
        strstr(WALTER_TLS_CA_PEM, "-----END CERTIFICATE-----");
}
void onGnss(WMGNSSEventType event, const WMGNSSEventData* data, void*) {
    if (event == WALTER_MODEM_GNSS_EVENT_FIX) xQueueOverwrite(gnssEvents, &data->gnssfix);
}
void onHttp(WMHTTPEventType event, const WMHTTPEventData* data, void*) {
    if (event == WALTER_MODEM_HTTP_EVENT_RING && data->profile_id == WALTER_HTTP_PROFILE)
        xQueueOverwrite(httpEvents, data);
}
bool radioOff() {
    if (!modem.setOpState(WALTER_MODEM_OPSTATE_MINIMUM)) return false;
    const auto deadline = monotonicMs() + 30000;
    while (modem.getNetworkRegState() != WALTER_MODEM_NETWORK_REG_NOT_SEARCHING) {
        if (monotonicMs() >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(100)); // Cleanup must still run after stop.
    }
    return true;
}
bool beginModem() {
    if (!begun) {
        if (!modem.begin(&Serial2)) return false;
        begun = true;
        modem.setGNSSEventHandler(onGnss, nullptr);
        modem.setHTTPEventHandler(onHttp, nullptr);
    }
    return true;
}
bool modemStep(const char* label, bool ok) {
    Serial.printf("[MODEM] %s: %s\n", label, ok ? "OK" : "FAILED");
    return ok;
}
bool waitForSimReady() {
    const auto deadline = monotonicMs() + 10000;
    WalterModemRsp rsp{};
    do {
        if (cancelRequested.load()) return false;
        if (modem.getSIMState(&rsp)) {
            Serial.printf("[SIM] state=%u (0=ready)\n", unsigned(rsp.data.simState));
            return rsp.data.simState == WALTER_MODEM_SIM_STATE_READY; // Never guess a PIN.
        }
    } while (monotonicMs() < deadline && pauseMs(500));
    Serial.printf("[SIM] Not ready: result=%u CME=%u\n", unsigned(rsp.result),
        rsp.type == WALTER_MODEM_RSP_DATA_TYPE_CME_ERROR ? unsigned(rsp.data.cmeError) : 0);
    return false;
}
void diagnoseRaw() {
    // Fresh-boot-only, fixed read-only queries. No arbitrary AT passthrough,
    // credential queries or concurrent access to WalterModem's UART tasks.
    Serial.println("[DIAG] Reading modem diagnostics without resetting its rejection history");
    Serial2.begin(115200, SERIAL_8N1, 14, 48);
    Serial2.setPins(14, 48, 47, 21);
    Serial2.setHwFlowCtrlMode(UART_HW_FLOWCTRL_CTS_RTS, 64);
    while (Serial2.available()) Serial2.read();
    for (const char* query : {"AT", "AT+CGMR", "AT+CFUN?", "AT+CPIN?", "AT+CEREG?",
                              "AT+CEER", "AT+SQNMONI=0", "AT+SQNBANDSEL?"}) {
        if (cancelRequested.load()) break;
        Serial.printf("[DIAG] %s\n", query);
        Serial2.print(query); Serial2.print("\r\n");
        String line;
        const auto deadline = monotonicMs() + 4000;
        while (monotonicMs() < deadline && !cancelRequested.load()) {
            if (!Serial2.available()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            const char c = Serial2.read();
            if (c == '\n') {
                line.trim();
                if (line.length()) Serial.printf("[DIAG] %s\n", line.c_str());
                const bool done = line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR:");
                line = "";
                if (done) break;
            } else if (line.length() < 220) line += c;
        }
    }
    Serial2.end();
}
void inspectModem() {
    Serial.println("[INSPECT] Initializing modem; no telemetry or network registration requested");
    if (!beginModem() || !modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        Serial.println("[INSPECT] Modem communication/RF-off failed"); return;
    }
    WalterModemRsp rsp{};
    if (modem.getIdentity(&rsp)) Serial.printf("[INSPECT] Modem SVN=%s (IMEI withheld)\n", rsp.data.identity.svn);
    waitForSimReady(); // SIM initialization can lag a CFUN transition.
    if (modem.getRAT(&rsp)) Serial.printf("[INSPECT] RAT=%u (0=LTE-M, 1=NB-IoT, 2=auto)\n", unsigned(rsp.data.rat));
    if (modem.getClock(&rsp)) Serial.printf("[INSPECT] Modem UTC=%lld\n", (long long)rsp.data.clock.epochTime);
    if (!radioOff()) Serial.println("[INSPECT] Could not confirm radio off; check board");
}
bool prepareModem() {
    if (!beginModem()) return false;
    if (configured) return true;
    if (cancelRequested.load()) return false;
    Serial.println("[WALTER] Configuring SIM, APN, GNSS and TLS (CA slot12/profile2)");
    WalterModemRsp rsp{};
    if (!modem.checkComm() || !modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF) ||
        !waitForSimReady()) {
        Serial.println("[WALTER] SIM/modem not ready; no PIN attempts made"); return false;
    }
    if (!modem.getRAT(&rsp)) return false;
    if (rsp.data.rat != WALTER_RAT) {
        // SQNMODEACTIVE takes effect after a modem restart. Refresh the driver's
        // cached RAT explicitly rather than trusting its pre-reset value.
        if (!modem.setRAT(WalterModemRAT(WALTER_RAT)) || !modem.softReset() ||
            !modem.configCMEErrorReports() ||
            !modem.configCEREGReports(WALTER_MODEM_CEREG_REPORTS_ENABLED_UE_PSM_WITH_LOCATION_EMM_CAUSE) ||
            !modem.sendCmd("AT+SQNMODEACTIVE?") || !modem.getRAT(&rsp) || rsp.data.rat != WALTER_RAT ||
            !modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF) || !waitForSimReady()) return false;
    }
    if (!modemStep("PDP context", modem.definePDPContext(1, WALTER_APN)) ||
        !modemStep("PDP authentication", modem.setPDPAuthParams(WalterModemPDPAuthProtocol(WALTER_APN_AUTH),
                               WALTER_APN_USER, WALTER_APN_PASSWORD, 1)) ||
        !modemStep("GNSS configuration", modem.gnssConfig()) ||
        !modemStep("Trusted CA slot", modem.tlsWriteCredential(false, WALTER_CA_SLOT, WALTER_TLS_CA_PEM)) ||
        !modemStep("TLS CA/hostname validation", modem.tlsConfigProfile(WALTER_TLS_PROFILE, WALTER_MODEM_TLS_VALIDATION_URL_AND_CA,
                               WALTER_MODEM_TLS_VERSION_12, WALTER_CA_SLOT)) ||
        !modemStep("HTTPS profile", modem.httpConfigProfile(WALTER_HTTP_PROFILE, WALTER_HTTPS_HOST, 443, WALTER_TLS_PROFILE))) return false;
    configured = true;
    return !cancelRequested.load();
}
bool networkOn() {
    if (cancelRequested.load()) return false;
    Serial.println("[LTE] Registering on configured SIM/RAT");
    if (!modem.setOpState(WALTER_MODEM_OPSTATE_FULL) ||
        !modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) return false;
    const auto deadline = monotonicMs() + WALTER_NETWORK_TIMEOUT_MS;
    auto previous = WALTER_MODEM_NETWORK_REG_UNKNOWN;
    while (true) {
        const auto state = modem.getNetworkRegState();
        if (state != previous) {
            Serial.printf("[LTE] Registration state=%u\n", unsigned(state)); previous = state;
            WalterModemRsp mode{};
            if (modem.getOpState(&mode)) Serial.printf("[LTE] Operational state=%u (1=full)\n", unsigned(mode.data.opState));
        }
        if (state == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME || state == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING) break;
        // Roaming can reject one PLMN before automatic selection finds another.
        if (monotonicMs() >= deadline || !pauseMs(500)) {
            Serial.printf("[LTE] Registration incomplete; state=%u\n", unsigned(state));
            WalterModemRsp signal{};
            if (!cancelRequested.load() && modem.getSignalQuality(&signal))
                Serial.printf("[LTE] Signal RSRP=%ddBm RSRQ=%d/10dB\n", signal.data.signalQuality.rsrp, signal.data.signalQuality.rsrq);
            return false;
        }
    }
    WalterModemRsp rsp{};
    if (modem.getClock(&rsp)) setUtc(rsp.data.clock.epochTime);
    Serial.printf("[LTE] Registered; UTC=%lu\n", (unsigned long)utcNow());
    return !cancelRequested.load();
}
bool acquireFix() {
    // GNSS and LTE share the modem: never operate them concurrently.
    if (!radioOff() || cancelRequested.load()) return false;
    Serial.println("[GNSS] LTE off; requesting a real fix");
    xQueueReset(gnssEvents);
    if (utcNow() && !modem.gnssSetUTCTime(utcNow())) return false;
    if (!modem.gnssPerformAction()) return false;
    WMGNSSFixEvent fix{};
    bool received = false;
    const auto deadline = monotonicMs() + WALTER_GNSS_TIMEOUT_MS;
    while (!cancelRequested.load() && monotonicMs() < deadline) {
        if (xQueueReceive(gnssEvents, &fix, pdMS_TO_TICKS(100)) == pdTRUE) { received = true; break; }
    }
    if (!received && !modem.gnssPerformAction(WALTER_MODEM_GNSS_ACTION_CANCEL)) {
        Serial.println("[GNSS] Could not confirm cancellation; stopping before LTE");
        running = false; cancelRequested = true; return false;
    }
    if (received) Serial.printf("[GNSS] Event status=%u satellites=%u accuracy=%.1fm UTC=%lld\n",
        unsigned(fix.status), fix.satCount, fix.estimatedConfidence, (long long)fix.timestamp);
    if (!received || cancelRequested.load() || fix.status != WALTER_MODEM_GNSS_FIX_STATUS_READY ||
        !walter::plausibleUtc(fix.timestamp, BLUEPAWS_BUILD_UNIX_TIME) || !std::isfinite(fix.latitude) || !std::isfinite(fix.longitude) ||
        fabs(fix.latitude) > 90 || fabs(fix.longitude) > 180 || fix.satCount < 4 ||
        !std::isfinite(fix.estimatedConfidence) || fix.estimatedConfidence <= 0 || fix.estimatedConfidence > 1000) {
        Serial.println("[GNSS] No usable fix; not fabricating coordinates"); return false;
    }
    setUtc(fix.timestamp);
    lastFix = {true, int32_t(llround(fix.latitude * 1e7)), int32_t(llround(fix.longitude * 1e7)),
               uint32_t(fix.timestamp), uint16_t(ceil(fix.estimatedConfidence)), fix.satCount};
    Serial.printf("[GNSS] Valid fix: satellites=%u accuracy=%um UTC=%lu\n",
                  lastFix.satellites, lastFix.accuracyM, (unsigned long)lastFix.utc);
    return true;
}
bool nextSequence(uint16_t& result) {
    if (!sequenceReady) {
        if (!sequenceStore.begin("bp-walter", false)) return false;
        sequenceNext = sequenceEnd = sequenceStore.getUInt("next", 1);
        sequenceReady = true;
    }
    if (sequenceNext == sequenceEnd) {
        // Reserve before use, so power loss cannot reuse the current block.
        const uint32_t end = sequenceNext + 256;
        if (sequenceStore.putUInt("next", end) != sizeof(end)) return false;
        sequenceEnd = end;
    }
    result = uint16_t(sequenceNext++);
    return true;
}
bool upload(const uint8_t* packet, uint8_t length) {
    if (!networkOn()) return false;
    char b64[89]{};
    size_t encoded = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64), sizeof(b64), &encoded, packet, length)) return false;
    uint8_t digest[32]; char hash[65]{};
    bp_sha256_ctx_t sha;
    bp_sha256_init(&sha); bp_sha256_update(&sha, packet, length); bp_sha256_final(&sha, digest);
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(hash + i * 2, 3, "%02x", digest[i]);
    JsonDocument request;
    walter::fillRequest(request, b64);
    String body; serializeJson(request, body);
    const String authorization = String("Authorization: Bearer ") + WALTER_BEARER_TOKEN;
    xQueueReset(httpEvents);
    if (cancelRequested.load() || !modem.httpSend(WALTER_HTTP_PROFILE, WALTER_HTTPS_PATH,
        reinterpret_cast<uint8_t*>(const_cast<char*>(body.c_str())), body.length(),
        WALTER_MODEM_HTTP_SEND_CMD_POST, WALTER_MODEM_HTTP_POST_PARAM_JSON,
        nullptr, 0, authorization.c_str())) return false;
    WMHTTPEventData event{};
    const auto deadline = monotonicMs() + WALTER_HTTP_TIMEOUT_MS;
    while (!cancelRequested.load() && monotonicMs() < deadline) {
        if (xQueueReceive(httpEvents, &event, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        Serial.printf("[LTE] HTTP status=%u response_bytes=%u\n", event.status, event.data_len);
        if (event.status < 200 || event.status >= 300 || event.data_len == 0 || event.data_len > 1500) return false;
        uint8_t response[1501]{};
        if (!modem.httpReceive(WALTER_HTTP_PROFILE, response, 1500)) return false;
        JsonDocument receipt;
        if (deserializeJson(receipt, response, event.data_len)) return false;
        const bool accepted = walter::acceptedReceipt(receipt, WALTER_DEVICE_ID, pkt_msg_seq(packet), hash);
        if (accepted) Serial.printf("[LTE] ACCEPTED device=%u seq=%u hash=%s\n", WALTER_DEVICE_ID, pkt_msg_seq(packet), hash);
        if (receipt["command_pending"] == true) Serial.println("[LTE] Pending command NOT executed or ACKed by this testbed");
        return accepted;
    }
    return false;
}
void cycle(bool boot, bool force, bp_profile_t profile, uint32_t count, uint32_t homeCount, uint64_t& lastLteMs) {
    if (cancelRequested.load()) return;
    const auto decision = walter::decide(profile, count, homeCount, simulatedHome.load(), boot, force,
                                       uint32_t((monotonicMs() - lastLteMs) / 1000));
    if (!decision.packet) return;
    if (!prepareModem()) { Serial.println("[WALTER] Modem setup failed; stopping session"); running = false; return; }
    // Acquire network UTC on a cold start; never substitute the build timestamp.
    if (!utcNow() && !networkOn()) { cellularFailure = true; Serial.println("[WALTER] Network/time bootstrap failed"); return; }
    const bool gpsFailure = decision.gnss && !acquireFix();
    if (cancelRequested.load()) return;
    if (!utcNow()) { Serial.println("[WALTER] No reliable UTC; packet blocked"); return; }
    uint16_t sequence;
    if (!nextSequence(sequence)) { Serial.println("[WALTER] NVS reservation failed; stopping session"); running = false; return; }
    uint8_t packet[BP_MAX_PACKET_SIZE]{};
    const uint8_t length = walter::buildPacket(packet, WALTER_DEVICE_ID, WALTER_HOME_HUB_ID, sequence,
        utcNow(), profile, simulatedHome.load(), decision.reason, lastFix, gpsFailure,
        cellularFailure, uint32_t(monotonicMs() / 1000), hmacKey);
    if (!length) { Serial.println("[WALTER] Packet validation failed"); return; }
    Serial.printf("[LORA-STUB] SIMULATED TX only; no RF, no ACK. seq=%u bytes=%u hex=", sequence, length);
    for (uint8_t i = 0; i < length; ++i) Serial.printf("%02X", packet[i]);
    Serial.println();
    if (!pauseMs(CMD_LISTEN_WINDOW_MS)) return;
    if (decision.lte) {
        const bool accepted = upload(packet, length); // Immutable LoRa-stub bytes, including HMAC.
        lastLteMs = monotonicMs(); // Attempt cadence, not an invented cloud receipt.
        cellularFailure = !accepted;
        if (!accepted) Serial.println("[LTE] No matching acceptance; delivery unconfirmed (no automatic retry)");
        modem.httpClose(WALTER_HTTP_PROFILE);
    }
}
void worker(void*) {
    for (;;) {
        if (!running.load() && !oneShot.load() && !inspectRequested.load()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        busy = true;
        if (inspectRequested.exchange(false)) {
            const bool raw = rawDiagnosticRequested.exchange(false);
            const bool gnss = gnssInspectionRequested.exchange(false);
            if (!cancelRequested.load()) {
                if (raw) diagnoseRaw();
                else if (gnss) {
                    if (beginModem() && radioOff() && modem.gnssConfig()) {
                        const bool fixed = acquireFix();
                        Serial.printf("[GNSS] Inspection result=%s (no upload)\n", fixed ? "VALID FIX" : "NO FIX");
                    }
                    if (begun && !radioOff()) Serial.println("[GNSS] Could not confirm RF off");
                } else inspectModem();
            }
            busy = false; Serial.println("[WALTER] Idle after inspection"); continue;
        }
        const bool single = oneShot.exchange(false);
        uint32_t count = 0, homeCount = 0;
        uint64_t lastLteMs = monotonicMs(), lostStartMs = monotonicMs();
        do {
            auto profile = bp_profile_t(selectedProfile.load());
            if (profile == PROFILE_LOST && monotonicMs() - lostStartMs >= uint64_t(LOST_MODE_MAX_DURATION_S) * 1000) {
                profile = LOST_MODE_FALLBACK; selectedProfile = profile;
                Serial.println("[WALTER] Lost timeout: switching to Active");
            }
            ++count; if (simulatedHome.load()) ++homeCount;
            cycle(count == 1 && !single, single, profile, count, homeCount, lastLteMs);
            if (begun && !radioOff()) {
                Serial.println("[WALTER] Could not confirm radio off; session stopped, check board"); running = false;
            }
            if (single || !running.load() || cancelRequested.load()) break;
            if (!pauseMs(walter::sleepSeconds(profile) * 1000UL)) break;
        } while (running.load());
        running = false;
        busy = false;
        Serial.println("[WALTER] Idle. No further scheduled transmissions");
    }
}
void command(const String& text) {
    if (text == "stop") {
        cancelRequested = true; running = false; oneShot = false; inspectRequested = false;
        Serial.println("[WALTER] Stop requested; completing/cancelling current modem operation"); return;
    }
    if (text == "status") {
        Serial.printf("[WALTER] device=%u hub=%u busy=%u running=%u profile=%s home_stub=%u credentials=%s\n",
            WALTER_DEVICE_ID, WALTER_HOME_HUB_ID, busy.load(), running.load(),
            bp_profile_name(bp_profile_t(selectedProfile.load())), simulatedHome.load(), credentialsReady() ? "configured" : "missing");
        Serial.printf("[WALTER] uptime=%lus reset_reason=%u free_heap=%u\n", (unsigned long)(monotonicMs()/1000),
            unsigned(esp_reset_reason()), unsigned(ESP.getFreeHeap())); return;
    }
    if (busy.load() || running.load() || oneShot.load() || inspectRequested.load()) { Serial.println("[WALTER] Stop and wait for Idle before changing settings"); return; }
    if (text.startsWith("clock ")) {
        const String value = text.substring(6);
        if (value.length() != 10) { Serial.println("[CLOCK] Supply a 10-digit current UTC epoch"); return; }
        for (char c : value) if (c < '0' || c > '9') { Serial.println("[CLOCK] Invalid UTC epoch"); return; }
        const int64_t epoch = strtoll(value.c_str(), nullptr, 10);
        if (!walter::plausibleUtc(epoch, BLUEPAWS_BUILD_UNIX_TIME)) { Serial.println("[CLOCK] Implausible UTC epoch"); return; }
        setUtc(epoch);
        Serial.println("[CLOCK] Explicit host UTC seed accepted; no coordinates or GNSS validity fabricated"); return;
    }
    if (text == "inspect" || text == "diagnose" || text == "gnss") {
        if (text == "diagnose" && begun) { Serial.println("[DIAG] Requires a fresh ESP boot before inspect/start/send"); return; }
        if (text == "gnss" && !utcNow()) { Serial.println("[GNSS] Seed actual UTC with clock <epoch> first"); return; }
        cancelRequested = false; rawDiagnosticRequested = text == "diagnose";
        gnssInspectionRequested = text == "gnss"; inspectRequested = true; return;
    }
    if (text == "start" || text == "send") {
        if (!credentialsReady()) { Serial.println("[WALTER] Blocked: configure separate APN, bearer, HMAC and trusted CA first"); return; }
        cancelRequested = false;
        if (text == "start") running = true; else oneShot = true;
    } else if (text.startsWith("profile ")) {
        const auto profile = bp_profile_from_name(text.substring(8).c_str());
        if (profile == PROFILE_UNKNOWN) Serial.println("[WALTER] Unknown profile"); else selectedProfile = profile;
    } else if (text == "home on" || text == "home off") {
        simulatedHome = text == "home on";
        Serial.println("[WALTER] Home is an explicit simulation, not a received BLE beacon");
    } else Serial.println("Commands: status | inspect | diagnose | clock <UTC epoch> | gnss | start | stop | send | profile normal/powersave/active/lost/debug | home on/off");
}
} // namespace

void setup() {
    Serial.setTxBufferSize(4096);
    Serial.setTxTimeoutMs(1000);
    Serial.begin(115200);
    gnssEvents = xQueueCreate(1, sizeof(WMGNSSFixEvent));
    httpEvents = xQueueCreate(1, sizeof(WMHTTPEventData));
    if (!gnssEvents || !httpEvents || xTaskCreate(worker, "walter-test", 16384, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("[WALTER] Task/queue allocation failed");
        for (;;) delay(1000);
    }
    Serial.println("[WALTER] Independent LTE/GNSS testbed. Idle until start/send. LoRa is simulated; Wi-Fi is unused.");
}

void loop() {
    static String input;
    static bool overflow = false;
    while (Serial.available()) {
        const char c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (overflow) Serial.println("[WALTER] Command too long");
            else if (input.length()) { input.trim(); command(input); }
            input = ""; overflow = false;
        } else if (input.length() < 80 && !overflow) input += c;
        else overflow = true;
    }
    delay(10);
}
