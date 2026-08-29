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
#include "walter_console.h"

namespace {
WalterConsole console;
WalterModem modem;
Preferences sequenceStore;
QueueHandle_t gnssEvents, httpEvents;
std::atomic<bool> running{false}, busy{false}, cancelRequested{false}, oneShot{false};
std::atomic<bool> inspectRequested{false};
std::atomic<bool> rawDiagnosticRequested{false};
std::atomic<bool> gnssInspectionRequested{false};
std::atomic<uint8_t> gnssInspectionMode{0}; // 0=single, 1=settle, 2=settle with hot starts.
std::atomic<bool> lteInspectionRequested{false};
std::atomic<bool> assistanceRequested{false};
std::atomic<unsigned> assistanceEvents{0};
std::atomic<int> registrationRatRequested{-1};
std::atomic<bool> offlineBench{true}; // Safe default after every ESP reboot.
std::atomic<uint32_t> loraTxCount{0}, lteSkippedCount{0};
std::atomic<uint32_t> nextWakeUtc{0};
std::atomic<uint8_t> selectedProfile{PROFILE_NORMAL};
std::atomic<bool> simulatedHome{false};
const uint8_t hmacKey[] = WALTER_HMAC_KEY_BYTES;
static_assert(sizeof(hmacKey) == 32, "HMAC key must contain exactly 32 bytes");
bool begun = false, configured = false, stateStoreReady = false, sequenceReady = false, cellularFailure = false;
uint32_t sequenceNext = 0, sequenceEnd = 0;
uint64_t clockAnchorMs = 0;
uint32_t clockAnchorUtc = 0;
walter::Fix lastFix;
char lastCloudCommandId[37]{};
uint16_t lastCloudCommandSequence = 0;
uint64_t lostProfileStartedMs = 0;

constexpr uint32_t WALTER_COMMAND_STATE_MAGIC = 0x4250434dUL; // "BPCM"
struct WalterCommandState {
    uint32_t magic;
    uint16_t sequence;
    uint8_t profile;
    uint8_t reserved;
    char id[37];
    uint32_t checksum;
};

uint64_t monotonicMs() { return uint64_t(esp_timer_get_time()) / 1000; }
uint32_t utcNow() {
    if (!clockAnchorUtc) return 0;
    const uint64_t now = clockAnchorUtc + (monotonicMs() - clockAnchorMs) / 1000;
    return walter::validUtc(now) ? uint32_t(now) : 0;
}
void setUtc(int64_t utc) {
    if (walter::plausibleUtc(utc, BLUEPAWS_BUILD_UNIX_TIME)) {
        clockAnchorUtc = uint32_t(utc); clockAnchorMs = monotonicMs();
    } else console.println("[CLOCK] Ignoring implausible modem/GNSS UTC");
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
bool packetCredentialsReady() {
    uint8_t any = 0;
    for (auto b : hmacKey) any |= b;
    return any != 0;
}
bool credentialsReady() {
    return packetCredentialsReady() && safeAtString(WALTER_APN, 99, true) &&
        safeAtString(WALTER_APN_USER, 63, false) &&
        safeAtString(WALTER_APN_PASSWORD, 63, false) &&
        safeAtString(WALTER_BEARER_TOKEN, 256, true) && strlen(WALTER_BEARER_TOKEN) >= 32 &&
        strstr(WALTER_TLS_CA_PEM, "-----BEGIN CERTIFICATE-----") &&
        strstr(WALTER_TLS_CA_PEM, "-----END CERTIFICATE-----");
}
void onGnss(WMGNSSEventType event, const WMGNSSEventData* data, void*) {
    if (event == WALTER_MODEM_GNSS_EVENT_FIX) xQueueOverwrite(gnssEvents, &data->gnssfix);
    if (event == WALTER_MODEM_GNSS_EVENT_ASSISTANCE && unsigned(data->assistance) < 3)
        assistanceEvents.fetch_or(1u << unsigned(data->assistance));
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
    console.printf("[MODEM] %s: %s\n", label, ok ? "OK" : "FAILED");
    return ok;
}
bool waitForSimReady() {
    const auto deadline = monotonicMs() + 10000;
    WalterModemRsp rsp{};
    do {
        if (cancelRequested.load()) return false;
        if (modem.getSIMState(&rsp)) {
            console.printf("[SIM] state=%u (0=ready)\n", unsigned(rsp.data.simState));
            return rsp.data.simState == WALTER_MODEM_SIM_STATE_READY; // Never guess a PIN.
        }
    } while (monotonicMs() < deadline && pauseMs(500));
    console.printf("[SIM] Not ready: result=%u CME=%u\n", unsigned(rsp.result),
        rsp.type == WALTER_MODEM_RSP_DATA_TYPE_CME_ERROR ? unsigned(rsp.data.cmeError) : 0);
    return false;
}
void diagnoseRaw() {
    // Fresh-boot-only, fixed read-only queries. No arbitrary AT passthrough,
    // credential queries or concurrent access to WalterModem's UART tasks.
    console.println("[DIAG] Reading modem diagnostics without resetting its rejection history");
    Serial2.begin(115200, SERIAL_8N1, 14, 48);
    Serial2.setPins(14, 48, 47, 21);
    Serial2.setHwFlowCtrlMode(UART_HW_FLOWCTRL_CTS_RTS, 64);
    while (Serial2.available()) Serial2.read();
    for (const char* query : {"AT", "AT+CGMR", "AT+CFUN?", "AT+CPIN?", "AT+CEREG?",
                              "AT+CEER", "AT+SQNMONI=0", "AT+SQNBANDSEL?"}) {
        if (cancelRequested.load()) break;
        console.printf("[DIAG] %s\n", query);
        Serial2.print(query); Serial2.print("\r\n");
        String line;
        const auto deadline = monotonicMs() + 4000;
        while (monotonicMs() < deadline && !cancelRequested.load()) {
            if (!Serial2.available()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            const char c = Serial2.read();
            if (c == '\n') {
                line.trim();
                if (line.length()) console.printf("[DIAG] %s\n", line.c_str());
                const bool done = line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR:");
                line = "";
                if (done) break;
            } else if (line.length() < 220) line += c;
        }
    }
    Serial2.end();
}
void inspectModem() {
    console.println("[INSPECT] Initializing modem; no telemetry or network registration requested");
    if (!beginModem() || !modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF)) {
        console.println("[INSPECT] Modem communication/RF-off failed"); return;
    }
    WalterModemRsp rsp{};
    if (modem.getIdentity(&rsp)) console.printf("[INSPECT] Modem SVN=%s (IMEI withheld)\n", rsp.data.identity.svn);
    waitForSimReady(); // SIM initialization can lag a CFUN transition.
    if (modem.getRAT(&rsp)) console.printf("[INSPECT] RAT=%u (0=LTE-M, 1=NB-IoT, 2=auto)\n", unsigned(rsp.data.rat));
    if (modem.getClock(&rsp)) console.printf("[INSPECT] Modem UTC=%lld\n", (long long)rsp.data.clock.epochTime);
    if (!radioOff()) console.println("[INSPECT] Could not confirm radio off; check board");
}
bool buildPdpAuthCommand(char* out, size_t capacity) {
    if (WALTER_APN_AUTH < 0 || WALTER_APN_AUTH > 2 ||
        !safeAtString(WALTER_APN_USER, 63, false) || !safeAtString(WALTER_APN_PASSWORD, 63, false)) return false;
    // Omit optional credential fields when both are empty (Sequans p341).
    const int n = (!strlen(WALTER_APN_USER) && !strlen(WALTER_APN_PASSWORD))
        ? snprintf(out, capacity, "AT+CGAUTH=1,%u", unsigned(WALTER_APN_AUTH))
        : snprintf(out, capacity, "AT+CGAUTH=1,%u,\"%s\",\"%s\"",
            unsigned(WALTER_APN_AUTH), WALTER_APN_USER, WALTER_APN_PASSWORD);
    return n > 0 && size_t(n) < capacity;
}
bool configurePdpAuth() {
    // Pinned driver's setter returns early when its cached protocol is NONE,
    // before assigning the requested protocol. Send explicitly; never log secrets.
    char command[160]{};
    return buildPdpAuthCommand(command, sizeof(command)) && modem.sendCmd(command);
}
bool selectRat(uint8_t requested) {
    if (requested > 1 || cancelRequested.load()) return false;
    WalterModemRsp rsp{};
    if (!modem.getRAT(&rsp)) return false;
    if (rsp.data.rat == requested) return true;
    // Sequans LR8.2 p90: CFUN0 is required before SQNMODEACTIVE, not CFUN4.
    return radioOff() && !cancelRequested.load() &&
        modem.setRAT(WalterModemRAT(requested)) && modem.softReset() &&
        modem.configCMEErrorReports() &&
        modem.configCEREGReports(WALTER_MODEM_CEREG_REPORTS_ENABLED_UE_PSM_WITH_LOCATION_EMM_CAUSE) &&
        modem.sendCmd("AT+SQNMODEACTIVE?") && modem.getRAT(&rsp) && rsp.data.rat == requested &&
        modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF) && waitForSimReady();
}
int registrationState(const char* line) {
    if (strncmp(line, "+CEREG:", 7)) return -1;
    char* tail = nullptr;
    const char* first = line + 7;
    const long value = strtol(first, &tail, 10);
    if (tail == first) return -1;
    while (*tail == ' ') ++tail;
    if (*tail == ',') {
        ++tail; while (*tail == ' ') ++tail;
        // Query response has numeric <n>,<stat>; extended URC has quoted TAC.
        if (*tail >= '0' && *tail <= '9') return int(strtol(tail, nullptr, 10));
    }
    return int(value);
}
bool rawAt(const char* command, String& response, uint32_t timeoutMs = 4000, bool cleanup = false,
           bool resetting = false) {
    response = "";
    Serial2.print(command); Serial2.print("\r\n");
    String line;
    static String lastCereg;
    const auto deadline = monotonicMs() + timeoutMs;
    while (monotonicMs() < deadline && (cleanup || !cancelRequested.load())) {
        if (!Serial2.available()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        const char c = Serial2.read();
        if (c != '\n') { if (line.length() < 240) line += c; continue; }
        line.trim();
        // Only fixed diagnostic responses: never echo transmitted auth strings.
        const bool diagnostic = line.startsWith("+CEREG:") || line.startsWith("+CEER:") ||
            line.startsWith("+SQNMONI:") || line.startsWith("+CESQ:") || line.startsWith("+CSQ:") ||
            line.startsWith("+CFUN:") || line.startsWith("+CPIN:") || line.startsWith("+SQNCTM:") ||
            line.startsWith("+SQNMODEACTIVE:") || line.startsWith("+CGATT:") || line.startsWith("+CGPADDR:");
        if (diagnostic) {
            if (response.length() + line.length() < 1000) { response += line; response += '\n'; }
            if (!line.startsWith("+CEREG:") || line != lastCereg) console.printf("[REG-RAW] %s\n", line.c_str());
            if (line.startsWith("+CEREG:")) lastCereg = line;
        }
        if (line == "ERROR" || line.startsWith("+CME ERROR:")) {
            console.printf("[REG-RAW] %s\n", line.c_str()); return false;
        }
        if (resetting ? line == "+SYSSTART" : line == "OK") return true;
        line = "";
    }
    console.println("[REG-RAW] Command cancelled or timed out"); return false;
}
void registerOnly(uint8_t rat) {
    // No library UART tasks may exist here. No TLS, GNSS, packet or cloud calls.
    if (begun || rat > 1) return;
    Serial2.begin(115200, SERIAL_8N1, 14, 48);
    Serial2.setPins(14, 48, 47, 21);
    Serial2.setHwFlowCtrlMode(UART_HW_FLOWCTRL_CTS_RTS, 64);
    while (Serial2.available()) Serial2.read();
    String response;
    int originalMode = 0;
    bool changed = false;
    const bool registered = [&]() {
        console.printf("[REG] Registration-only %s; APN auth=%u; no GNSS/LoRa/HTTP\n", rat ? "NB-IoT" : "LTE-M", unsigned(WALTER_APN_AUTH));
        // Same reset pulse/hold as WalterModem::begin/reset, without starting
        // its UART tasks. A fresh ESP flash does not guarantee an awake modem.
        gpio_hold_dis(GPIO_NUM_45);
        gpio_set_direction(GPIO_NUM_45, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(GPIO_NUM_45, GPIO_FLOATING);
        gpio_set_level(GPIO_NUM_45, 0); vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(GPIO_NUM_45, 1); gpio_hold_en(GPIO_NUM_45);
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!rawAt("", response, 30000, false, true) || !rawAt("ATE0", response) || !rawAt("AT+CMEE=1", response) ||
            !rawAt("AT+CFUN=0", response, 30000) || !rawAt("AT+SQNMODEACTIVE?", response)) return false;
        int modeOffset = response.indexOf("+SQNMODEACTIVE:");
        if (modeOffset < 0 || sscanf(response.c_str() + modeOffset, "+SQNMODEACTIVE: %d", &originalMode) != 1 ||
            (originalMode != 1 && originalMode != 2)) return false;
        if (originalMode != rat + 1) {
            changed = true; // Restore even if the mode command times out ambiguously.
            const char* mode = rat ? "AT+SQNMODEACTIVE=2" : "AT+SQNMODEACTIVE=1";
            if (!rawAt(mode, response) || !rawAt("AT^RESET", response, 30000, false, true) ||
                !rawAt("ATE0", response) || !rawAt("AT+CMEE=1", response)) return false;
        }
        if (!rawAt("AT+SQNMODEACTIVE?", response)) return false;
        int active = 0;
        modeOffset = response.indexOf("+SQNMODEACTIVE:");
        if (modeOffset < 0 || sscanf(response.c_str() + modeOffset, "+SQNMODEACTIVE: %d", &active) != 1 || active != rat + 1) return false;
        if (!rawAt("AT+SQNCTM?", response) || !rawAt("AT+CFUN=4", response, 30000)) return false;
        bool ready = false;
        const auto simDeadline = monotonicMs() + 10000;
        do {
            if (rawAt("AT+CPIN?", response) && response.indexOf("+CPIN: READY") >= 0) { ready = true; break; }
            if (response.indexOf("PIN") >= 0 || response.indexOf("PUK") >= 0) break;
        } while (monotonicMs() < simDeadline && pauseMs(500));
        if (!ready) return false;
        char command[180]{};
        if (!safeAtString(WALTER_APN, 99, true)) return false;
        snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"", WALTER_APN);
        if (!modemStep("Registration APN", rawAt(command, response))) return false;
        if (!buildPdpAuthCommand(command, sizeof(command)) || !modemStep("Registration CGAUTH", rawAt(command, response))) return false;
        console.println("[REG] Explicit IPv4 APN and CGAUTH accepted by modem");
        if (!rawAt("AT+CEREG=5", response) || !rawAt("AT+CFUN=1", response, 30000) ||
            !rawAt("AT+COPS=0", response, 30000)) return false;
        const auto deadline = monotonicMs() + 300000; // Controlled five-minute test, not a production policy.
        uint64_t nextMetrics = 0;
        int previous = -1;
        while (!cancelRequested.load() && monotonicMs() < deadline) {
            if (!rawAt("AT+CEREG?", response)) return false;
            const int offset = response.lastIndexOf("+CEREG:");
            const int state = offset < 0 ? -1 : registrationState(response.c_str() + offset);
            if (state == 1 || state == 5) {
                rawAt("AT+CGATT?", response); rawAt("AT+CGPADDR=1", response);
                rawAt("AT+CESQ", response); rawAt("AT+SQNMONI=0", response);
                return true;
            }
            if (state == 3 && previous != 3) rawAt("AT+CEER", response);
            previous = state;
            if (monotonicMs() >= nextMetrics) {
                console.printf("[REG] Elapsed=%lus\n", (unsigned long)((monotonicMs() + 300000 - deadline) / 1000));
                rawAt("AT+CESQ", response); rawAt("AT+SQNMONI=0", response);
                nextMetrics = monotonicMs() + 30000;
            }
            if (!pauseMs(5000)) break;
        }
        if (!cancelRequested.load()) { rawAt("AT+CEER", response); rawAt("AT+CESQ", response); rawAt("AT+SQNMONI=0", response); }
        return false;
    }();
    console.printf("[REG] Result=%s; no cloud transmission\n", registered ? "REGISTERED" : "NOT REGISTERED");
    bool off = rawAt("AT+CFUN=0", response, 30000, true);
    if (off && changed) {
        off = rawAt(originalMode == 1 ? "AT+SQNMODEACTIVE=1" : "AT+SQNMODEACTIVE=2", response, 4000, true) &&
            rawAt("AT^RESET", response, 30000, true, true) && rawAt("AT+CFUN=0", response, 30000, true);
        int restored = 0;
        const bool queried = off && rawAt("AT+SQNMODEACTIVE?", response, 4000, true);
        const int offset = response.indexOf("+SQNMODEACTIVE:");
        off = queried && offset >= 0 && sscanf(response.c_str() + offset, "+SQNMODEACTIVE: %d", &restored) == 1 && restored == originalMode;
    }
    off = off && rawAt("AT+CFUN?", response, 4000, true) && response.indexOf("+CFUN: 0") >= 0;
    console.printf("[REG] Cleanup=%s; original RAT %s\n", off ? "RF OFF" : "UNCONFIRMED - check board",
        changed ? (off ? "restored" : "restoration unconfirmed") : "unchanged");
    Serial2.end();
}
bool prepareModem() {
    if (!beginModem()) return false;
    if (configured) return true;
    if (cancelRequested.load()) return false;
    console.println("[WALTER] Configuring SIM, APN and TLS (CA slot12/profile2)");
    if (!modem.checkComm() || !modem.setOpState(WALTER_MODEM_OPSTATE_NO_RF) ||
        !waitForSimReady()) {
        console.println("[WALTER] SIM/modem not ready; no PIN attempts made"); return false;
    }
    if (!selectRat(WALTER_RAT)) return false;
    if (!modemStep("PDP context", modem.definePDPContext(1, WALTER_APN)) ||
        !modemStep("PDP authentication", configurePdpAuth()) ||
        !modemStep("Trusted CA slot", modem.tlsWriteCredential(false, WALTER_CA_SLOT, WALTER_TLS_CA_PEM)) ||
        !modemStep("TLS CA/hostname validation", modem.tlsConfigProfile(WALTER_TLS_PROFILE, WALTER_MODEM_TLS_VALIDATION_URL_AND_CA,
                               WALTER_MODEM_TLS_VERSION_12, WALTER_CA_SLOT)) ||
        !modemStep("HTTPS profile", modem.httpConfigProfile(WALTER_HTTP_PROFILE, WALTER_HTTPS_HOST, 443, WALTER_TLS_PROFILE))) return false;
    configured = true;
    return !cancelRequested.load();
}
void reportSignal() {
    WalterModemRsp signal{};
    if (!cancelRequested.load() && modem.getSignalQuality(&signal)) {
        const auto& quality = signal.data.signalQuality;
        if (walter::signalQualityAvailable(quality.rsrp, quality.rsrq))
            console.printf("[LTE] Signal RSRP=%ddBm RSRQ=%d/10dB (CESQ estimate)\n", quality.rsrp, quality.rsrq);
        else console.println("[LTE] Signal unavailable (unknown/out-of-range CESQ result)");
    }
}
bool networkOn() {
    if (cancelRequested.load()) return false;
    console.println("[LTE] Registering on configured SIM/RAT");
    if (!modem.setOpState(WALTER_MODEM_OPSTATE_FULL) ||
        !modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC)) return false;
    const auto deadline = monotonicMs() + WALTER_NETWORK_TIMEOUT_MS;
    auto previous = WALTER_MODEM_NETWORK_REG_UNKNOWN;
    while (true) {
        const auto state = modem.getNetworkRegState();
        if (state != previous) {
            console.printf("[LTE] Registration state=%u\n", unsigned(state)); previous = state;
            WalterModemRsp mode{};
            if (modem.getOpState(&mode)) console.printf("[LTE] Operational state=%u (1=full)\n", unsigned(mode.data.opState));
        }
        if (state == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME || state == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING) break;
        // Roaming can reject one PLMN before automatic selection finds another.
        if (monotonicMs() >= deadline || !pauseMs(500)) {
            console.printf("[LTE] Registration incomplete; state=%u\n", unsigned(state));
            reportSignal();
            return false;
        }
    }
    WalterModemRsp rsp{};
    if (modem.getClock(&rsp)) setUtc(rsp.data.clock.epochTime);
    console.printf("[LTE] Registered; UTC=%lu\n", (unsigned long)utcNow());
    reportSignal();
    return !cancelRequested.load();
}
bool refreshGnssAssistance() {
    if (cancelRequested.load() || !utcNow() || !prepareModem() || !radioOff()
        || !modem.gnssSetUTCTime(utcNow()) || !modem.gnssConfig()) return false;
    WalterModemRsp status{};
    if (!modem.gnssGetAssistanceStatus(&status)
        || status.type != WALTER_MODEM_RSP_DATA_TYPE_GNSS_ASSISTANCE_DATA) return false;
    bool needs[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        const auto& item = status.data.gnssAssistance[i];
        needs[i] = !item.available || item.timeToUpdate <= 0;
        console.printf("[ASSIST] type=%u available=%u update_in=%lds expires_in=%lds\n",
            i, item.available, long(item.timeToUpdate), long(item.timeToExpire));
    }
    if ((needs[0] || needs[1]) && !networkOn()) return false;
    for (unsigned i = 0; i < 2; ++i) {
        if (!needs[i]) continue;
        assistanceEvents.fetch_and(~(1u << i));
        if (cancelRequested.load() || !modem.gnssUpdateAssistance(WMGNSSAssistanceType(i))) return false;
        const auto deadline = monotonicMs() + 60000;
        while (!(assistanceEvents.load() & (1u << i))) {
            if (monotonicMs() >= deadline || !pauseMs(100)) return false;
        }
        console.printf("[ASSIST] Download event type=%u\n", i);
    }
    if (!modem.gnssGetAssistanceStatus(&status)
        || status.type != WALTER_MODEM_RSP_DATA_TYPE_GNSS_ASSISTANCE_DATA) return false;
    for (unsigned i = 0; i < 2; ++i) {
        const auto& item = status.data.gnssAssistance[i];
        console.printf("[ASSIST] Verified type=%u available=%u expires_in=%lds\n",
            i, item.available, long(item.timeToExpire));
        if (!item.available || item.timeToExpire <= 0) return false;
    }
    return !cancelRequested.load();
}
bool usableGnssSnapshot(const WMGNSSFixEvent& fix) {
    const auto now = utcNow();
    return fix.status == WALTER_MODEM_GNSS_FIX_STATUS_READY &&
        walter::plausibleUtc(fix.timestamp, BLUEPAWS_BUILD_UNIX_TIME) &&
        now >= fix.timestamp && int64_t(now) - fix.timestamp < GPS_STALE_THRESHOLD_S &&
        std::isfinite(fix.latitude) && std::isfinite(fix.longitude) &&
        fabs(fix.latitude) <= 90 && fabs(fix.longitude) <= 180 && fix.satCount >= 4 &&
        fix.satCount <= WALTER_MODEM_GNSS_MAX_SATS &&
        std::isfinite(fix.estimatedConfidence) && fix.estimatedConfidence > 0 && fix.estimatedConfidence <= 1000;
}
double gnssSeparationM(const walter::Fix& a, const walter::Fix& b) {
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const double latA = a.latE7 / 1e7 * radians, latB = b.latE7 / 1e7 * radians;
    const double dLat = latB - latA, dLon = (double(b.lonE7) - a.lonE7) / 1e7 * radians;
    const double h = pow(sin(dLat / 2), 2) + cos(latA) * cos(latB) * pow(sin(dLon / 2), 2);
    return 6371000.0 * 2 * asin(sqrt(fmax(0.0, fmin(1.0, h))));
}
bool settleGnss(bool hot) {
    // Diagnostic only: keep LTE off, retain each real snapshot, never fabricate a refined fix.
    lastFix = {};
    if (!utcNow() || cancelRequested.load() || !radioOff() || !modem.gnssConfig()) return false;
    constexpr unsigned maxAttempts = 20;
    walter::Fix candidates[maxAttempts]{};
    double uncertainties[maxAttempts]{};
    unsigned attempts = 0, count = 0, consistent = 0;
    walter::Fix previous{};
    const auto started = monotonicMs(), deadline = started + 60000;
    console.printf("[GNSS SETTLE] OPEN budget=60000ms mode=%s target=50m consistent=3 max_step=25m\n",
                   hot ? "hot-after-valid" : "cold-warm");
    while (!cancelRequested.load() && monotonicMs() < deadline && attempts < maxAttempts) {
        // Only opt into hot starts after a real fresh fix from this session, never a hardcoded position.
        if (hot && previous.valid && !modem.gnssConfig(WALTER_MODEM_GNSS_SENS_MODE_HIGH,
                                                     WALTER_MODEM_GNSS_ACQ_MODE_HOT_START)) break;
        xQueueReset(gnssEvents);
        if (!modem.gnssSetUTCTime(utcNow()) || cancelRequested.load() || monotonicMs() >= deadline) break;
        const auto attemptStarted = monotonicMs();
        if (!modem.gnssPerformAction()) break;
        ++attempts;
        WMGNSSFixEvent fix{};
        bool received = false;
        while (!cancelRequested.load() && monotonicMs() < deadline) {
            if (xQueueReceive(gnssEvents, &fix, pdMS_TO_TICKS(100)) == pdTRUE) { received = true; break; }
        }
        if (!received) {
            if (!modem.gnssPerformAction(WALTER_MODEM_GNSS_ACTION_CANCEL)) {
                console.println("[GNSS SETTLE] Cancellation unconfirmed; stop session and check modem");
                running = false; cancelRequested = true; return false;
            }
            break;
        }
        unsigned strong = 0;
        const unsigned satCount = unsigned(fix.satCount) < WALTER_MODEM_GNSS_MAX_SATS
            ? unsigned(fix.satCount) : WALTER_MODEM_GNSS_MAX_SATS;
        for (unsigned i = 0; i < satCount; ++i) if (fix.sats[i].signalStrength >= 30) ++strong;
        console.printf("[GNSS SAMPLE] n=%u elapsed_ms=%llu ttf_ms=%lu status=%u uncertainty=%.1fm lat=%.7f lon=%.7f utc=%lld entries=%u cn0_ge30=%u\n",
            attempts, (unsigned long long)(monotonicMs() - attemptStarted), (unsigned long)fix.timeToFix,
            unsigned(fix.status), fix.estimatedConfidence, fix.latitude, fix.longitude,
            (long long)fix.timestamp, unsigned(fix.satCount), strong);
        for (unsigned i = 0; i < satCount; ++i)
            console.printf("[GNSS CN0] sample=%u entry=%u satellite=%u cn0=%u\n", attempts, i,
                           unsigned(fix.sats[i].satNo), unsigned(fix.sats[i].signalStrength));
        if (usableGnssSnapshot(fix) && !cancelRequested.load()) {
            walter::Fix candidate{true, int32_t(llround(fix.latitude * 1e7)), int32_t(llround(fix.longitude * 1e7)),
                                 uint32_t(fix.timestamp), uint16_t(ceil(fix.estimatedConfidence)), fix.satCount};
            candidates[count] = candidate; uncertainties[count++] = fix.estimatedConfidence;
            const bool target = fix.estimatedConfidence <= 50;
            consistent = target ? (previous.valid && candidate.utc > previous.utc &&
                gnssSeparationM(previous, candidate) <= 25 ? consistent + 1 : 1) : 0;
            previous = candidate;
            if (consistent >= 3) break;
        } else { consistent = 0; previous = {}; }
        if (monotonicMs() + 1000 >= deadline || !pauseMs(1000)) break;
    }
    // Selection uses the original timestamp. Never rewind the host clock to the best earlier snapshot.
    double best = 1001;
    if (!cancelRequested.load()) for (unsigned i = 0; i < count; ++i) {
        const auto now = utcNow();
        if (now >= candidates[i].utc && now - candidates[i].utc < GPS_STALE_THRESHOLD_S && uncertainties[i] <= best) {
            lastFix = candidates[i]; best = uncertainties[i];
        }
    }
    console.printf("[GNSS SETTLE] CLOSED elapsed_ms=%llu attempts=%u usable=%u consistent=%u selected=%u uncertainty=%.1fm utc=%lu (no upload)\n",
        (unsigned long long)(monotonicMs() - started), attempts, count, consistent, lastFix.valid,
        lastFix.valid ? best : 0, (unsigned long)lastFix.utc);
    return lastFix.valid && !cancelRequested.load();
}
bool acquireFix() {
    // GNSS and LTE share the modem: never operate them concurrently.
    if (!utcNow() || !radioOff() || cancelRequested.load()) return false;
    console.println("[GNSS] LTE off; requesting a real fix");
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
        console.println("[GNSS] Could not confirm cancellation; stopping before LTE");
        running = false; cancelRequested = true; return false;
    }
    if (received) console.printf("[GNSS] Event status=%u satellites=%u accuracy=%.1fm UTC=%lld\n",
        unsigned(fix.status), fix.satCount, fix.estimatedConfidence, (long long)fix.timestamp);
    if (!received || cancelRequested.load() || !usableGnssSnapshot(fix)) {
        console.println("[GNSS] No usable fix; not fabricating coordinates"); return false;
    }
    // An acquisition result is a timestamped sample, not the current wall clock.
    // Preserve the established UTC anchor and report the sample's real age.
    lastFix = {true, int32_t(llround(fix.latitude * 1e7)), int32_t(llround(fix.longitude * 1e7)),
               uint32_t(fix.timestamp), uint16_t(ceil(fix.estimatedConfidence)), fix.satCount};
    console.printf("[GNSS] Valid fix: satellites=%u accuracy=%um UTC=%lu\n",
                  lastFix.satellites, lastFix.accuracyM, (unsigned long)lastFix.utc);
    return true;
}
bool ensureStateStore() {
    if (stateStoreReady) return true;
    stateStoreReady = sequenceStore.begin("bp-walter", false);
    return stateStoreReady;
}
uint32_t commandStateChecksum(WalterCommandState state) {
    state.checksum = 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&state);
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < sizeof(state); ++i) {
        hash ^= bytes[i];
        hash *= 16777619UL;
    }
    return hash;
}
bool restoreCommandState() {
    if (!ensureStateStore() || sequenceStore.getBytesLength("last-cmd") != sizeof(WalterCommandState)) return false;
    WalterCommandState state{};
    if (sequenceStore.getBytes("last-cmd", &state, sizeof(state)) != sizeof(state)
        || state.magic != WALTER_COMMAND_STATE_MAGIC || !state.sequence
        || state.profile > PROFILE_DEBUG || state.id[36] != '\0'
        || state.checksum != commandStateChecksum(state)) return false;
    memcpy(lastCloudCommandId, state.id, sizeof(lastCloudCommandId));
    lastCloudCommandSequence = state.sequence;
    selectedProfile = state.profile;
    lostProfileStartedMs = monotonicMs();
    return true;
}
bool persistCommandState(const walter::ProfileCommand& command) {
    if (!ensureStateStore()) return false;
    WalterCommandState state{};
    state.magic = WALTER_COMMAND_STATE_MAGIC;
    state.sequence = command.sequence;
    state.profile = command.profile;
    memcpy(state.id, command.id, sizeof(state.id));
    state.id[36] = '\0';
    state.checksum = commandStateChecksum(state);
    return sequenceStore.putBytes("last-cmd", &state, sizeof(state)) == sizeof(state);
}
bool nextSequence(uint16_t& result) {
    if (!sequenceReady) {
        if (!ensureStateStore()) return false;
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
// One bounded HTTPS exchange. Sequans may report zero content length for a
// chunked response; actually read and strictly parse the bounded body instead
// of treating HTTP 201 alone (or content-length zero) as an ingestion receipt.
bool postJson(JsonDocument& request, JsonDocument& response) {
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
        console.printf("[LTE] HTTP status=%u response_bytes=%u\n", event.status, event.data_len);
        constexpr size_t capacity = 1500; // Pinned WalterModem receive-buffer limit.
        if (event.status < 200 || event.status >= 300 || event.data_len >= capacity) return false;
        uint8_t bytes[capacity + 1]{};
        if (!modem.httpReceive(WALTER_HTTP_PROFILE, bytes, capacity)) return false;
        return walter::parseHttpBody(response, bytes, capacity, event.data_len);
    }
    return false;
}
bool sendPacket(const uint8_t* packet, uint8_t length, JsonDocument& receipt) {
    char b64[89]{};
    size_t encoded = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64), sizeof(b64), &encoded, packet, length)) return false;
    uint8_t digest[32]; char hash[65]{};
    bp_sha256_ctx_t sha;
    bp_sha256_init(&sha); bp_sha256_update(&sha, packet, length); bp_sha256_final(&sha, digest);
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(hash + i * 2, 3, "%02x", digest[i]);
    JsonDocument request;
    walter::fillRequest(request, b64);
    if (!postJson(request, receipt)) return false;
    const bool accepted = walter::acceptedReceipt(receipt, WALTER_DEVICE_ID, pkt_msg_seq(packet), hash);
    if (accepted) console.printf("[LTE] ACCEPTED device=%u seq=%u hash=%s\n", WALTER_DEVICE_ID, pkt_msg_seq(packet), hash);
    return accepted;
}
void applyCloudCommand(JsonDocument& receipt) {
    walter::ProfileCommand command;
    if (!walter::parseProfileCommand(receipt, WALTER_DEVICE_ID, utcNow(), command)) {
        if (receipt["command_pending"] == true) console.println("[LTE CMD] Invalid/expired/unsupported command; not ACKed");
        return;
    }
    // Reserve the ACK identity before changing state. A failed reservation
    // must not consume a command we cannot acknowledge.
    uint16_t sequence;
    if (cancelRequested.load() || !nextSequence(sequence)) return;
    const bool duplicate = strcmp(lastCloudCommandId, command.id) == 0;
    if ((duplicate && lastCloudCommandSequence != command.sequence)
        || (!duplicate && lastCloudCommandSequence == command.sequence)) {
        console.println("[LTE CMD] Command identity conflict; not applied or ACKed"); return;
    }
    if (duplicate && selectedProfile.load() != command.profile) {
        console.println("[LTE CMD] Superseded locally; old command not ACKed"); return;
    }
    if (!duplicate) {
        // Persist the applied identity and resulting profile before changing
        // runtime state. A reboot or a delivery over another path can then
        // re-ACK the command without applying it twice.
        if (!persistCommandState(command)) {
            console.println("[LTE CMD] NVS command reservation failed; not applied or ACKed"); return;
        }
        selectedProfile = command.profile;
        lostProfileStartedMs = monotonicMs();
        memcpy(lastCloudCommandId, command.id, sizeof(lastCloudCommandId));
        lastCloudCommandSequence = command.sequence;
        console.printf("[LTE CMD] APPLIED seq=%u profile=%s\n", command.sequence, bp_profile_name(command.profile));
    }
    uint8_t packet[BP_MAX_PACKET_SIZE]{};
    const auto length = walter::buildCommandAck(packet, WALTER_DEVICE_ID, WALTER_HOME_HUB_ID,
        sequence, command.sequence, utcNow(), bp_profile_t(selectedProfile.load()), simulatedHome.load(), hmacKey);
    JsonDocument acknowledgement;
    if (!length || !sendPacket(packet, length, acknowledgement)) {
        console.println("[LTE CMD] Applied; ACK delivery unconfirmed (can retry on next poll)"); return;
    }
    if (acknowledgement["acked_command"]["sequence_id"].is<unsigned>()
        && acknowledgement["acked_command"]["sequence_id"].as<unsigned>() == command.sequence
        && acknowledgement["acked_command"]["status"] == "acked")
        console.printf("[LTE CMD] CLOUD ACK CONFIRMED seq=%u\n", command.sequence);
    // A further command may be claimed by this ACK response. The next poll
    // redelivers it; do not recursively open another ten-second window.
}
void listenForLteCommands(JsonDocument& initialReceipt) {
    const auto deadline = monotonicMs() + CMD_LISTEN_WINDOW_MS;
    console.printf("[LTE CMD] Window OPEN minimum=%ums; authenticated polling\n", unsigned(CMD_LISTEN_WINDOW_MS));
    applyCloudCommand(initialReceipt);
    // Include a final poll at the deadline, so commands queued near the end
    // are not missed. In-flight HTTPS/ACKs may extend the window; stop cancels.
    while (!cancelRequested.load()) {
        const auto now = monotonicMs();
        if (now < deadline && !pauseMs(uint32_t(std::min<uint64_t>(1000, deadline - now)))) break;
        JsonDocument request, receipt;
        request["format"] = "device_commands";
        request["ingest_path"] = "cellular_direct";
        request["device_id"] = WALTER_DEVICE_ID;
        if (!postJson(request, receipt) || !walter::commandPollReceipt(receipt, WALTER_DEVICE_ID)) {
            console.println("[LTE CMD] Poll failed; command delivery unavailable");
            if (monotonicMs() < deadline) pauseMs(uint32_t(deadline - monotonicMs()));
            break;
        }
        applyCloudCommand(receipt);
        if (monotonicMs() >= deadline) break;
    }
    console.println("[LTE CMD] Window CLOSED");
}
bool upload(const uint8_t* packet, uint8_t length) {
    if (!networkOn()) return false;
    JsonDocument receipt;
    if (!sendPacket(packet, length, receipt)) return false;
    listenForLteCommands(receipt);
    return true; // Telemetry receipt remains valid even if a later poll fails.
}
bool testLteOnly() {
    // Explicit diagnostic: independent of bench mode, without GNSS or LoRa.
    if (cancelRequested.load() || !credentialsReady() || !utcNow()) return false;
    console.println("[LTE] One-shot LTE-only test; no GNSS acquisition or LoRa stub");
    if (!prepareModem() || cancelRequested.load()) return false;
    uint16_t sequence;
    if (!nextSequence(sequence)) { console.println("[LTE] NVS reservation failed; upload blocked"); return false; }
    uint8_t packet[BP_MAX_PACKET_SIZE]{};
    const uint8_t length = walter::buildPacket(packet, WALTER_DEVICE_ID, WALTER_HOME_HUB_ID, sequence,
        utcNow(), bp_profile_t(selectedProfile.load()), simulatedHome.load(), TX_INTERRUPT, walter::Fix{},
        false, cellularFailure, uint32_t(monotonicMs() / 1000), hmacKey);
    if (!length) return false;
    char hex[BP_MAX_PACKET_SIZE * 2 + 1]{};
    for (uint8_t i = 0; i < length; ++i) snprintf(hex + i * 2, 3, "%02X", packet[i]);
    console.printf("[LTE] No-fix test packet seq=%u bytes=%u hex=%s\n", sequence, length, hex);
    const bool accepted = upload(packet, length); // Registers once before attempting HTTPS.
    cellularFailure = !accepted;
    return accepted;
}
bool transmitLoraStub(const uint8_t* packet, uint8_t length) {
    if (cancelRequested.load() || !packet || length < BP_HEADER_SIZE + BP_AUTH_TAG_SIZE ||
        length > BP_MAX_PACKET_SIZE) return false;
    char b64[89]{};
    size_t encoded = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64), sizeof(b64), &encoded, packet, length)) return false;
    char hex[BP_MAX_PACKET_SIZE * 2 + 1]{};
    for (uint8_t i = 0; i < length; ++i) snprintf(hex + i * 2, 3, "%02X", packet[i]);
    console.printf("[LORA-STUB] SIMULATED TX only; no RF, no ACK. seq=%u bytes=%u hex=%s\n", pkt_msg_seq(packet), length, hex);
    console.printf("[LORA-STUB] base64=%s\n", b64);
    ++loraTxCount;
    console.printf("[LORA-STUB] TX_COMPLETE result=OK seq=%u count=%lu (simulated local send; delivery unknown)\n",
        pkt_msg_seq(packet), (unsigned long)loraTxCount.load());
    return true; // Transport completion only, never a hub ACK/cloud receipt.
}
void cycle(bool boot, bool force, bp_profile_t profile, uint32_t count, uint32_t homeCount, uint64_t& lastLteMs) {
    if (cancelRequested.load()) return;
    const auto decision = walter::decide(profile, count, homeCount, simulatedHome.load(), boot, force,
                                       uint32_t((monotonicMs() - lastLteMs) / 1000));
    if (!decision.packet) return;
    const bool offline = offlineBench.load();
    if (offline && !utcNow()) {
        console.println("[BENCH] Seed actual UTC with clock <epoch> before sending; no modem access");
        running = false; return;
    }
    if (!offline && !utcNow()) {
        // Online cold start can acquire UTC over LTE. Explicit host UTC bypasses this dependency.
        if (!prepareModem() || !networkOn()) {
            cellularFailure = true; console.println("[WALTER] Network/time bootstrap failed"); return;
        }
    }
    bool gpsFailure = false;
    if (decision.gnss) {
        if (offline) console.println("[GNSS] Skipped in offline bench; reporting no valid fix");
        else gpsFailure = !beginModem() || !radioOff() || !modem.gnssConfig() || !acquireFix();
    }
    if (cancelRequested.load()) return;
    if (!utcNow()) { console.println("[WALTER] No reliable UTC; packet blocked"); return; }
    uint16_t sequence;
    if (!nextSequence(sequence)) { console.println("[WALTER] NVS reservation failed; stopping session"); running = false; return; }
    uint8_t packet[BP_MAX_PACKET_SIZE]{};
    const uint8_t length = walter::buildPacket(packet, WALTER_DEVICE_ID, WALTER_HOME_HUB_ID, sequence,
        utcNow(), profile, simulatedHome.load(), decision.reason, offline ? walter::Fix{} : lastFix, gpsFailure,
        cellularFailure, uint32_t(monotonicMs() / 1000), hmacKey);
    if (!length) { console.println("[WALTER] Packet validation failed"); return; }
    if (!transmitLoraStub(packet, length)) { console.println("[LORA-STUB] Local send failed"); return; }
    console.printf("[LORA-STUB] Listen window=%lums; no receiver/hub ACK simulated\n", (unsigned long)CMD_LISTEN_WINDOW_MS);
    if (!pauseMs(CMD_LISTEN_WINDOW_MS)) return;
    if (decision.lte) {
        if (offline) {
            ++lteSkippedCount;
            lastLteMs = monotonicMs(); // Advance simulated schedule, not a real LTE attempt.
            console.printf("[LTE] Fallback due seq=%u; SKIPPED (offline bench; no modem/cloud traffic)\n", sequence);
            return;
        }
        const bool accepted = prepareModem() && upload(packet, length); // Same immutable bytes, including HMAC.
        lastLteMs = monotonicMs(); // Attempt cadence, not an invented cloud receipt.
        cellularFailure = !accepted;
        if (!accepted) console.println("[LTE] No matching acceptance; delivery unconfirmed (no automatic retry)");
        if (begun) modem.httpClose(WALTER_HTTP_PROFILE);
    }
}
bool waitForNextCycle() {
    const auto profile = bp_profile_t(selectedProfile.load());
    const auto seconds = walter::sleepSeconds(profile);
    nextWakeUtc = utcNow() ? utcNow() + seconds : 0;
    console.printf("[CYCLE] SLEEP profile=%s seconds=%lu next_wake_utc=%lu mode=freertos_rf_off\n",
                   bp_profile_name(profile), (unsigned long)seconds, (unsigned long)nextWakeUtc.load());
    const bool elapsed = pauseMs(seconds * 1000UL);
    nextWakeUtc = 0;
    return elapsed;
}
void worker(void*) {
    for (;;) {
        if (!running.load() && !oneShot.load() && !inspectRequested.load()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        busy = true;
        if (inspectRequested.exchange(false)) {
            const bool raw = rawDiagnosticRequested.exchange(false);
            const bool gnss = gnssInspectionRequested.exchange(false);
            const bool lte = lteInspectionRequested.exchange(false);
            const bool assist = assistanceRequested.exchange(false);
            const int registrationRat = registrationRatRequested.exchange(-1);
            if (!cancelRequested.load()) {
                if (registrationRat >= 0) registerOnly(uint8_t(registrationRat));
                else if (raw) diagnoseRaw();
                else if (assist) {
                    const bool updated = refreshGnssAssistance();
                    console.printf("[ASSIST] Result=%s (no telemetry)\n", updated ? "READY" : "UNAVAILABLE");
                    if (begun && !radioOff()) console.println("[ASSIST] Could not confirm RF off");
                } else if (gnss) {
                    if (beginModem() && radioOff() && modem.gnssConfig()) {
                        const auto mode = gnssInspectionMode.load();
                        const bool fixed = mode ? settleGnss(mode == 2) : acquireFix();
                        if (mode == 2 && !modem.gnssConfig()) console.println("[GNSS] Could not restore cold/warm configuration; check modem");
                        console.printf("[GNSS] Inspection result=%s (no upload)\n", fixed ? "VALID FIX" : "NO FIX");
                    }
                    if (begun && !radioOff()) console.println("[GNSS] Could not confirm RF off");
                } else if (lte) {
                    const bool accepted = testLteOnly();
                    console.printf("[LTE] Test result=%s (no automatic retry)\n", accepted ? "CLOUD ACCEPTED" : "DELIVERY UNCONFIRMED");
                    if (begun) {
                        modem.httpClose(WALTER_HTTP_PROFILE);
                        if (radioOff()) console.println("[LTE] RF-off cleanup confirmed");
                        else console.println("[LTE] Could not confirm RF off; check board");
                    }
                } else inspectModem();
            }
            busy = false; console.println("[WALTER] Idle after inspection"); continue;
        }
        const bool single = oneShot.exchange(false);
        uint32_t count = 0, homeCount = 0;
        uint64_t lastLteMs = monotonicMs();
        do {
            auto profile = bp_profile_t(selectedProfile.load());
            if (profile == PROFILE_LOST && monotonicMs() - lostProfileStartedMs >= uint64_t(LOST_MODE_MAX_DURATION_S) * 1000) {
                profile = LOST_MODE_FALLBACK; selectedProfile = profile;
                console.println("[WALTER] Lost timeout: switching to Active");
            }
            ++count; if (simulatedHome.load()) ++homeCount;
            const auto cycleStarted = monotonicMs();
            console.printf("[CYCLE] START n=%lu profile=%s home=%u offline=%u utc=%lu\n",
                (unsigned long)count, bp_profile_name(profile), simulatedHome.load(), offlineBench.load(), (unsigned long)utcNow());
            cycle(count == 1 && !single, single, profile, count, homeCount, lastLteMs);
            if (!offlineBench.load() && begun && !radioOff()) {
                console.println("[WALTER] Could not confirm radio off; session stopped, check board"); running = false;
            }
            console.printf("[CYCLE] RETURN n=%lu profile=%s elapsed_ms=%llu cancelled=%u\n",
                (unsigned long)count, bp_profile_name(bp_profile_t(selectedProfile.load())),
                (unsigned long long)(monotonicMs() - cycleStarted), cancelRequested.load());
            if (single || !running.load() || cancelRequested.load()) break;
            if (!waitForNextCycle()) break;
        } while (running.load());
        running = false;
        busy = false;
        console.println("[WALTER] Idle. No further scheduled transmissions");
    }
}
void command(const String& text) {
    if (text == "stop") {
        cancelRequested = true; running = false; oneShot = false; inspectRequested = false;
        console.println("[WALTER] Stop requested; completing/cancelling current modem operation"); return;
    }
    if (text == "status") {
        console.printf("[WALTER] device=%u hub=%u busy=%u running=%u profile=%s home_stub=%u credentials=%s\n",
            WALTER_DEVICE_ID, WALTER_HOME_HUB_ID, busy.load(), running.load(),
            bp_profile_name(bp_profile_t(selectedProfile.load())), simulatedHome.load(), credentialsReady() ? "configured" : "missing");
        console.printf("[SCHEDULE] next_wake_utc=%lu\n", (unsigned long)nextWakeUtc.load());
        console.printf("[WALTER] uptime=%lus reset_reason=%u free_heap=%u\n", (unsigned long)(monotonicMs()/1000),
            unsigned(esp_reset_reason()), unsigned(ESP.getFreeHeap()));
        console.printf("[BENCH] offline=%u utc=%lu lora_tx_complete=%lu lte_skipped=%lu\n",
            offlineBench.load(), (unsigned long)utcNow(), (unsigned long)loraTxCount.load(),
            (unsigned long)lteSkippedCount.load()); return;
    }
    if (busy.load() || running.load() || oneShot.load() || inspectRequested.load()) { console.println("[WALTER] Stop and wait for Idle before changing settings"); return; }
    if (text == "register ltem" || text == "register nbiot") {
        if (begun) { console.println("[REG] Requires fresh ESP boot before library modem initialization"); return; }
        cancelRequested = false; rawDiagnosticRequested = false; gnssInspectionRequested = false; lteInspectionRequested = false;
        registrationRatRequested = text == "register nbiot" ? 1 : 0; inspectRequested = true; return;
    }
    if (text == "bench on" || text == "bench off") {
        offlineBench = text == "bench on";
        console.printf("[BENCH] %s; LoRa always simulated; UTC must be seeded again after ESP reboot\n",
            offlineBench.load() ? "OFFLINE: cycles skip GNSS/LTE modem access" : "ONLINE: real GNSS/LTE enabled for next cycle");
        return;
    }
    if (text.startsWith("clock ")) {
        const String value = text.substring(6);
        if (value.length() != 10) { console.println("[CLOCK] Supply a 10-digit current UTC epoch"); return; }
        for (char c : value) if (c < '0' || c > '9') { console.println("[CLOCK] Invalid UTC epoch"); return; }
        const int64_t epoch = strtoll(value.c_str(), nullptr, 10);
        if (!walter::plausibleUtc(epoch, BLUEPAWS_BUILD_UNIX_TIME)) { console.println("[CLOCK] Implausible UTC epoch"); return; }
        setUtc(epoch);
        console.println("[CLOCK] Explicit host UTC seed accepted; no coordinates or GNSS validity fabricated"); return;
    }
    const bool gnssCommand = text == "gnss" || text == "gnss settle" || text == "gnss hot";
    if (text == "inspect" || text == "diagnose" || gnssCommand || text == "lte" || text == "assist") {
        if (text == "diagnose" && begun) { console.println("[DIAG] Requires a fresh ESP boot before inspect/start/send"); return; }
        if ((gnssCommand || text == "assist") && !utcNow()) { console.println("[GNSS] Seed actual UTC with clock <epoch> first"); return; }
        if (text == "lte" && (!utcNow() || !credentialsReady())) {
            console.println("[LTE] Seed actual UTC with clock <epoch>; APN, bearer, CA and HMAC required"); return;
        }
        cancelRequested = false; rawDiagnosticRequested = text == "diagnose";
        registrationRatRequested = -1;
        gnssInspectionRequested = gnssCommand;
        gnssInspectionMode = text == "gnss hot" ? 2 : text == "gnss settle" ? 1 : 0;
        lteInspectionRequested = text == "lte";
        assistanceRequested = text == "assist";
        inspectRequested = true; return;
    }
    if (text == "start" || text == "send") {
        if (!packetCredentialsReady() || (!offlineBench.load() && !credentialsReady())) {
            console.println("[WALTER] Blocked: packet HMAC required; online mode also requires APN, bearer and CA"); return;
        }
        cancelRequested = false;
        if (text == "start") running = true; else oneShot = true;
    } else if (text.startsWith("profile ")) {
        const auto profile = bp_profile_from_name(text.substring(8).c_str());
        if (profile == PROFILE_UNKNOWN) console.println("[WALTER] Unknown profile"); else { selectedProfile = profile; lostProfileStartedMs = monotonicMs(); }
    } else if (text == "home on" || text == "home off") {
        simulatedHome = text == "home on";
        console.println("[WALTER] Home is an explicit simulation, not a received BLE beacon");
    } else console.println("Commands: status | bench on/off | inspect | diagnose | register ltem/nbiot | clock <UTC epoch> | gnss [settle/hot] | assist | lte | start | stop | send | profile normal/powersave/active/lost/debug | home on/off");
}
} // namespace

void setup() {
    Serial.setTxBufferSize(4096);
    Serial.setTxTimeoutMs(1000);
    Serial.begin(115200);
    if (!console.begin()) for (;;) delay(1000);
    if (restoreCommandState()) {
        console.printf("[LTE CMD] Restored seq=%u profile=%s for duplicate protection\n",
            lastCloudCommandSequence, bp_profile_name(bp_profile_t(selectedProfile.load())));
    }
    gnssEvents = xQueueCreate(1, sizeof(WMGNSSFixEvent));
    httpEvents = xQueueCreate(1, sizeof(WMHTTPEventData));
    if (!gnssEvents || !httpEvents || xTaskCreate(worker, "walter-test", 16384, nullptr, 1, nullptr) != pdPASS) {
        console.println("[WALTER] Task/queue allocation failed");
        for (;;) delay(1000);
    }
    console.println("[WALTER] Independent testbed. OFFLINE BENCH by default; seed clock then start/send. LoRa is simulated; Wi-Fi is unused.");
}

void loop() {
    static String input;
    static bool overflow = false;
    while (Serial.available()) {
        const char c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (overflow) console.println("[WALTER] Command too long");
            else if (input.length()) { input.trim(); command(input); }
            input = ""; overflow = false;
        } else if (input.length() < 80 && !overflow) input += c;
        else overflow = true;
    }
    delay(10);
}
