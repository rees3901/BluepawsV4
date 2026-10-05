// Personal XIAO ESP32-S3 firmware. Canonical V4 sources are consumed unchanged.
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <sys/time.h>
#include "hardware.h"
#include "personal_policy.h"
#include "gnss_recovery.h"
#include "finder_flash.h"

#ifdef PERSONAL_COMPILE_CHECK
constexpr uint16_t PERSONAL_DEVICE_ID = 0xFFE1, PERSONAL_HUB_ID = 0xFFF0;
static const uint8_t PERSONAL_HMAC_KEY[32] = {};
#else
#include <personal_config.h>
#endif
static_assert(PERSONAL_DEVICE_ID > 0 && PERSONAL_DEVICE_ID < 0xFFFF && (PERSONAL_DEVICE_ID & 15), "Invalid collar ID");
static_assert(PERSONAL_HUB_ID > 0 && PERSONAL_HUB_ID < 0xFFFF && !(PERSONAL_HUB_ID & 15), "Invalid hub ID");
static_assert(sizeof(PERSONAL_HMAC_KEY) == 32, "HMAC key must be 32 bytes");
#ifndef PERSONAL_HOME_BLE_ADDRESS
#define PERSONAL_HOME_BLE_ADDRESS ""
#endif
#ifndef PERSONAL_D1_LED
#define PERSONAL_D1_LED 0
#endif

namespace {
SPIClass radioSPI(HSPI);
SX1262 radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY, radioSPI);
HardwareSerial gnssSerial(1);
TinyGPSPlus gps;
personal::NmeaDiagnostics nmea;
Preferences prefs;
constexpr uint32_t STATE_MAGIC = 0x42505031;
struct DurableState {
    uint32_t magic = STATE_MAGIC;
    uint8_t profile = PROFILE_NORMAL;
    uint8_t cursor = 0;
    uint16_t owner = PERSONAL_DEVICE_ID;
    uint32_t lostUntil = 0;
    personal::CommandRecord commands[16] = {};
} state;
struct RetainedState {
    uint32_t magic = STATE_MAGIC;
    uint32_t nextSequence = 0, sequenceEnd = 0;
    uint32_t fixTime = 0;
    int32_t lat = 0, lon = 0;
    uint32_t homeCycles = 0;
    uint8_t misses = 2;
    uint8_t satellites = 255;
    bool home = false;
} ;
RTC_DATA_ATTR RetainedState retained;
volatile bool radioEvent = false;
bool radioReady = false, gpsRunning = false, freshFix = false, gpsFailed = false;
bool bleReady = false, bootReport = true, buttonReport = false;
uint32_t lostStartedMs = 0;
uint8_t report[BP_MAX_PACKET_SIZE];
uint8_t reportLength = 0;
personal::FinderFlash finder;
constexpr int FINDER_PIN = 2; // XIAO D1, opt-in for this specific assembly.
void startFinder(uint8_t pattern, uint8_t count = 3) {
#if PERSONAL_D1_LED
    if (pattern == BUZZER_OFF) {
        finder.stop(); digitalWrite(FINDER_PIN, LOW);
        Serial.println("[FINDER LED] off");
    } else if (pattern == BUZZER_CHIRP) {
        finder.start(millis(), count);
        Serial.printf("[FINDER LED] %u flash(es) queued; one cycle only\n", count);
    } else Serial.println("[FINDER LED] unsupported pattern; only OFF/CHIRP supported");
#endif
}
void tickFinder() {
#if PERSONAL_D1_LED
    const bool wasActive = finder.active();
    digitalWrite(FINDER_PIN, finder.tick(millis()) ? HIGH : LOW);
    if (wasActive && !finder.active()) Serial.println("[FINDER LED] flashes complete; off");
    // Local bench control works without satellite UTC; not a remote protocol.
    static char input[24]; static uint8_t used = 0;
    while (Serial.available()) {
        const char c = char(Serial.read());
        if (c == '\n' || c == '\r') {
            input[used] = 0;
            if (!strcmp(input,"led flash")) startFinder(BUZZER_CHIRP);
            else if (!strcmp(input,"led off")) startFinder(BUZZER_OFF);
            used = 0;
        } else if (used < sizeof(input)-1) input[used++] = c;
        else used = 0;
    }
#endif
}
void IRAM_ATTR onRadio() { radioEvent = true; }

uint32_t utc() {
    const time_t value = time(nullptr);
    return value >= 1704067200 && value < 4102444800LL ? uint32_t(value) : 0;
}
void fatal(const char* message) {
    Serial.println(message);
    // Fail closed: do not transmit, initialise other hardware or silently reset NVS.
    while (true) delay(1000);
}
void saveState() {
    if (prefs.putBytes("state", &state, sizeof(state)) != sizeof(state)) fatal("[STATE] Save failed; stopped");
}
#ifndef PERSONAL_SEQUENCE_START
#define PERSONAL_SEQUENCE_START 1
#endif
static_assert(PERSONAL_SEQUENCE_START > 0 && PERSONAL_SEQUENCE_START <= 65535,
              "Replacement sequence must fit the existing packet contract");
uint16_t nextSequence() {
    // Reserve blocks before use: a cold reset skips the unused block, never reuses it.
    if (retained.nextSequence >= retained.sequenceEnd) {
        uint32_t start = prefs.getUInt("seqEnd", PERSONAL_SEQUENCE_START);
        retained.nextSequence = start;
        retained.sequenceEnd = start + 64;
        if (prefs.putUInt("seqEnd", retained.sequenceEnd) != sizeof(uint32_t)) fatal("[STATE] Sequence reservation failed");
    }
    return uint16_t(retained.nextSequence++);
}
void applyProfile(uint8_t profile) {
    state.profile = profile;
    state.lostUntil = profile == PROFILE_LOST && utc() ? utc() + LOST_MODE_MAX_DURATION_S : 0;
    lostStartedMs = millis();
    retained.homeCycles = 0;
    if (radioReady) radio.setOutputPower(bp_profile_config(bp_profile_t(profile))->tx_power_dBm);
}
void enforceLostTimeout() {
    if (state.profile != PROFILE_LOST) return;
    if ((state.lostUntil && utc() >= state.lostUntil) || millis() - lostStartedMs >= LOST_MODE_MAX_DURATION_S * 1000UL) {
        applyProfile(LOST_MODE_FALLBACK);
        saveState();
    }
}
void startGps() {
    if (gpsRunning) return;
    gps = TinyGPSPlus();
    nmea = personal::NmeaDiagnostics();
    digitalWrite(GNSS_WAKE, HIGH);
    delay(500);
    gnssSerial.begin(GNSS_BAUD, SERIAL_8N1, GNSS_RX, GNSS_TX);
    gpsRunning = true;
}
void stopGps(uint16_t seconds) {
    if (gpsRunning) {
        // L76K CASIC timed standby. WAKE_UP remains low for the sleep period.
        char body[24], sentence[32]; uint8_t checksum = 0;
        snprintf(body, sizeof(body), "PCAS12,%u", seconds);
        for (const char* p = body; *p; ++p) checksum ^= uint8_t(*p);
        snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", body, checksum);
        gnssSerial.print(sentence);
        gnssSerial.flush();
        delay(30);
        gnssSerial.end();
        gpsRunning = false;
    }
    pinMode(GNSS_TX, OUTPUT);
    digitalWrite(GNSS_TX, LOW);
    digitalWrite(GNSS_WAKE, LOW);
}
void pumpGps() {
    tickFinder();
    if (!gpsRunning) return;
    while (gnssSerial.available()) {
        const char c = char(gnssSerial.read());
        gps.encode(c); nmea.feed(c);
        if (nmea.txtReady) {
            Serial.printf("[NMEA TXT] %s\n", nmea.txt); nmea.txtReady=false;
        }
    }
    if (gps.date.isValid() && gps.time.isValid() && gps.date.age() < 2000 && gps.time.age() < 2000 &&
        gps.date.year() >= 2024 && gps.date.year() <= 2099 && gps.date.month() >= 1 && gps.date.month() <= 12 &&
        gps.date.day() >= 1 && gps.date.day() <= 31 && gps.time.hour() < 24 && gps.time.minute() < 60 && gps.time.second() < 60) {
        const uint32_t value = bp_gps_to_unix(gps.date.year(), gps.date.month(), gps.date.day(),
                                            gps.time.hour(), gps.time.minute(), gps.time.second());
        if (!utc() || abs(int64_t(utc()) - value) > 2) {
            timeval tv{time_t(value), 0}; settimeofday(&tv, nullptr);
        }
    }
}
bool usableFix() {
    return utc() && gps.location.isValid() && gps.location.age() < 2000 &&
           gps.satellites.isValid() && gps.satellites.age() < 2000 && gps.satellites.value() >= 4 &&
           gps.hdop.isValid() && gps.hdop.age() < 2000 && gps.hdop.hdop() <= 5.0;
}
void acquireGps(bool cold) {
    const uint32_t begin = millis();
    personal::GnssAttempt attempt(begin);
    startGps();
    // An old collar fix does not prove the receiver retained its ephemeris.
    // Keep the same quality gate, allow recovery on every attempt, stop at 60s.
    Serial.printf("[GNSS] attempt=%s limit=60s stable=10s; receiver start type unknown\n",
                  cold ? "boot/first-fix" : "wake/recovery");
    uint32_t diagnosticAt = begin, firstNmeaMs = UINT32_MAX, firstGoodMs = UINT32_MAX;
    freshFix = false;
    while (!attempt.expired(millis())) {
        pumpGps();
        const uint32_t now=millis();
        if (nmea.valid && firstNmeaMs==UINT32_MAX) firstNmeaMs=now-begin;
        const bool good=usableFix();
        if (good && firstGoodMs==UINT32_MAX) firstGoodMs=now-begin;
        if (now-diagnosticAt>=5000) {
            diagnosticAt=now;
            Serial.printf("[NMEA] t=%lus valid=%lu bad=%lu GGA=%lu quality=%d GSA=%lu type=%d RMC=%c GSV=%lu visible(last)=%d maxSNR=%d used=%lu HDOP=%.2f locAge=%lu good=%d\n",
                (now-begin)/1000,nmea.valid,nmea.bad,nmea.gga,nmea.quality,nmea.gsa,nmea.fixType,
                nmea.rmcStatus,nmea.gsv,nmea.visible,nmea.maxSnr,gps.satellites.value(),gps.hdop.hdop(),gps.location.age(),good);
        }
        if (attempt.accept(good,now)) {
                retained.lat = int32_t(llround(gps.location.lat() * 1e7));
                retained.lon = int32_t(llround(gps.location.lng() * 1e7));
                retained.fixTime = utc();
                retained.satellites = uint8_t(min(gps.satellites.value(), uint32_t(254)));
                freshFix = true;
                break;
        }
        delay(10);
    }
    gpsFailed = !freshFix;
    Serial.printf("[GNSS] elapsed=%lums firstNMEA=%lums firstGood=%lums (4294967295=none)\n",
        millis()-begin,firstNmeaMs,firstGoodMs);
    Serial.printf("[GNSS] %s; UTC %s; chars=%lu sats=%lu HDOP=%.2f\n",
        freshFix ? "fresh fix" : "no fresh fix", utc() ? "valid" : "unavailable",
        gps.charsProcessed(), gps.satellites.value(), gps.hdop.hdop());
}
void initBle() {
    if (bleReady) return;
    BLEDevice::init("BluePaws personal collar");
    bleReady = true;
}
bool scanHome() {
    initBle();
    BLEDevice::getAdvertising()->stop();
    auto* scan = BLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(160);
    scan->setWindow(80);
    auto results = scan->start(BLE_SCAN_DURATION_S, false);
    bool seen = false;
    for (int i = 0; i < results.getCount(); ++i) {
        auto device = results.getDevice(i);
        if (!device.haveName() || device.getName() != BLE_HOME_BEACON_NAME || device.getRSSI() < -90) continue;
        if (strlen(PERSONAL_HOME_BLE_ADDRESS) && strcasecmp(device.getAddress().toString().c_str(), PERSONAL_HOME_BLE_ADDRESS)) continue;
        Serial.printf("[BLE] Home beacon %s RSSI=%d\n", device.getAddress().toString().c_str(), device.getRSSI());
        seen = true;
    }
    scan->clearResults();
    if (seen) { retained.home = true; retained.misses = 0; }
    else if (++retained.misses >= 2) { retained.misses = 2; retained.home = false; retained.homeCycles = 0; }
    Serial.printf("[BLE] seen=%u home=%u missed=%u\n", seen, retained.home, retained.misses);
    return seen;
}
void advertiseFind() {
    initBle();
    char name[20]; snprintf(name, sizeof(name), "%s%04X", BLE_FIND_BEACON_PREFIX, PERSONAL_DEVICE_ID);
    BLEAdvertisementData data;
    data.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    data.setName(name);
    auto* adv = BLEDevice::getAdvertising();
    adv->setAdvertisementData(data);
    adv->setMinInterval(320); adv->setMaxInterval(320);
    adv->start();
}
void armReceive() {
    radioEvent = false;
    radio.startReceive();
}
bool transmit(const uint8_t* bytes, uint8_t size) {
    if (!radioReady) return false;
    radio.standby();
    bool clear = false;
    for (int i = 0; i < LORA_LBT_RETRIES; ++i) {
        if (radio.scanChannel() == RADIOLIB_CHANNEL_FREE) { clear = true; break; }
        delay(random(LORA_LBT_BACKOFF_MIN, LORA_LBT_BACKOFF_MAX));
    }
    const int16_t result = clear ? radio.transmit(bytes, size) : RADIOLIB_LORA_DETECTED;
    armReceive();
    Serial.printf("[RADIO] seq=%u bytes=%u result=%d\n", pkt_msg_seq(bytes), size, result);
    return result == RADIOLIB_ERR_NONE;
}
uint8_t buildPacket(uint8_t* p, uint8_t reason, bool homeSeen, uint16_t ack = 0) {
    const uint32_t now = utc();
    const uint16_t age = personal::fixAge(now, retained.fixTime);
    const bool valid = retained.fixTime && age <= GPS_STALE_THRESHOLD_S && !gpsFailed;
    uint8_t flags = homeSeen ? FLAG_HOME_BEACON_SEEN : 0;
    if (valid) flags |= FLAG_GNSS_VALID;
    else if (retained.fixTime) flags |= FLAG_STALE_FIX;
    if (gpsFailed) flags |= FLAG_ERROR_PRESENT;
    uint8_t status = state.profile == PROFILE_LOST ? STATUS_LOST : retained.home ? STATUS_HOME : valid ? STATUS_OUT_AND_ABOUT : STATUS_ERROR;
    pkt_init(p, PERSONAL_DEVICE_ID, PERSONAL_HUB_ID, nextSequence(), now, status, state.profile, flags, reason);
    pkt_set_gps(p, retained.lat, retained.lon);
    // No battery divider/fuel gauge is established on this assembly. Zero is
    // an unmeasured value, NOT an invented voltage. HDOP is not accuracy in metres.
    pkt_set_quality(p, 0, 0, age);
    pkt_set_sat_count(p, retained.fixTime ? retained.satellites : 255);
    pkt_add_tlv_u16(p, TLV_FW_VER, 0x0100);
    if (reason == TX_ACK) pkt_add_tlv_u16(p, TLV_ACKED_MSG_SEQ_ID, ack);
    return personal::sign(p, PERSONAL_HMAC_KEY);
}
void flashFind(uint8_t count) {
    for (uint8_t i = 0; i < min(count, uint8_t(10)); ++i) {
        digitalWrite(USER_LED, HIGH); delay(70);
        digitalWrite(USER_LED, LOW); delay(70);
    }
}
void receiveWindow(bool homeSeen) {
    const uint32_t started = millis();
    bool acknowledged = false, retried = false;
    armReceive();
    while (millis() - started < CMD_LISTEN_WINDOW_MS || finder.active()) {
        pumpGps(); enforceLostTimeout();
        if (radioEvent) {
            radioEvent = false;
            uint8_t incoming[BP_MAX_PACKET_SIZE] = {};
            const size_t size = radio.getPacketLength();
            const int16_t result = size >= BP_MIN_PACKET_SIZE && size <= sizeof(incoming)
                ? radio.readData(incoming, size) : RADIOLIB_ERR_PACKET_TOO_LONG;
            armReceive();
            if (result != RADIOLIB_ERR_NONE) continue;
            if (personal::receipt(incoming, size, PERSONAL_DEVICE_ID, PERSONAL_HUB_ID, pkt_msg_seq(report))) {
                acknowledged = true;
                continue; // Remain listening for the separate cloud command.
            }
            const auto action = personal::command(incoming, size, PERSONAL_DEVICE_ID, PERSONAL_HUB_ID,
                                                    utc(), state.commands, 16);
            if (action == personal::CommandResult::Reject) continue;
            const uint8_t reason = pkt_tx_reason(incoming);
            Serial.printf("[COMMAND] seq=%u reason=%u %s\n", pkt_msg_seq(incoming), reason,
                action == personal::CommandResult::Apply ? "applying" : "duplicate; re-ACK");
            if (action == personal::CommandResult::Apply) {
                if (reason == TX_CONFIG) {
                    uint8_t profile; pkt_tlv_get_u8(incoming, TLV_PROFILE, &profile);
                    applyProfile(profile);
                }
                auto& record = state.commands[state.cursor++ % 16];
                record = {};
                record.sequence = pkt_msg_seq(incoming); record.reason = reason;
                record.length = pkt_tlv_len(incoming); record.seen = utc();
                memcpy(record.bytes, incoming + BP_HEADER_SIZE, record.length);
                // Atomic NVS blob commits profile + duplicate record before ACK.
                saveState();
            }
            uint8_t ack[BP_MAX_PACKET_SIZE];
            const uint8_t len = buildPacket(ack, TX_ACK, homeSeen, pkt_msg_seq(incoming));
            transmit(ack, len);
            if (reason == TX_INTERRUPT && action == personal::CommandResult::Apply) {
                startFinder(BUZZER_OFF); // Stop any previous flash before legacy LED delays.
                uint8_t flashes = 5;
                pkt_tlv_get_u8(incoming, TLV_LED_FLASH, &flashes);
#if !PERSONAL_D1_LED
                flashFind(flashes);
#endif
                uint8_t pattern = BUZZER_CHIRP;
                pkt_tlv_get_u8(incoming, TLV_BUZZER_PATTERN, &pattern);
                startFinder(pattern);
            }
        }
        if (!acknowledged && !retried && millis() - started >= UPLINK_ACK_WAIT_MS) {
            retried = true;
            transmit(report, reportLength); // Exact same bytes, sequence and HMAC.
        }
        delay(5);
    }
    Serial.printf("[RECEIPT] %s\n", acknowledged ? "hub received" : "not received; no cellular fallback");
}
void sleepFor(uint16_t seconds) {
    finder.stop();
#if PERSONAL_D1_LED
    digitalWrite(FINDER_PIN, LOW); // External LED off throughout sleep.
    gpio_hold_en(gpio_num_t(FINDER_PIN));
#endif
    stopGps(seconds);
    if (radioReady) radio.sleep(false);
    if (bleReady) BLEDevice::deinit(false);
    bleReady = false;
    digitalWrite(USER_LED, LOW);
    gpio_hold_en(gpio_num_t(GNSS_WAKE));
    gpio_hold_en(gpio_num_t(GNSS_TX));
    gpio_deep_sleep_hold_en();
    esp_sleep_enable_timer_wakeup(uint64_t(seconds) * 1000000ULL);
    // A held button must not cause an endless deep-sleep reset loop.
    if (digitalRead(USER_BUTTON) == HIGH) {
        rtc_gpio_pullup_en(gpio_num_t(USER_BUTTON));
        rtc_gpio_pulldown_dis(gpio_num_t(USER_BUTTON));
        esp_sleep_enable_ext0_wakeup(gpio_num_t(USER_BUTTON), 0);
    }
    Serial.printf("[SLEEP] %us\n", seconds);
    Serial.flush();
    esp_deep_sleep_start();
}
} // namespace

void setup() {
    Serial.begin(115200);
#ifdef PERSONAL_COMPILE_CHECK
    fatal("Compile-check image: hardware disabled");
#endif
    uint8_t keyBits = 0; for (uint8_t b : PERSONAL_HMAC_KEY) keyBits |= b;
    if (!keyBits) fatal("[AUTH] Missing HMAC key");
    gpio_deep_sleep_hold_dis();
#if PERSONAL_D1_LED
    gpio_hold_dis(gpio_num_t(FINDER_PIN));
    digitalWrite(FINDER_PIN, LOW);
    pinMode(FINDER_PIN, OUTPUT);
    Serial.println("[FINDER LED] D1/GPIO2 active-high; idle off");
#endif
    gpio_hold_dis(gpio_num_t(GNSS_WAKE)); gpio_hold_dis(gpio_num_t(GNSS_TX));
    rtc_gpio_deinit(gpio_num_t(USER_BUTTON));
    pinMode(USER_BUTTON, INPUT_PULLUP); pinMode(USER_LED, OUTPUT);
    pinMode(GNSS_WAKE, OUTPUT); digitalWrite(GNSS_WAKE, LOW);
    const auto wake = esp_sleep_get_wakeup_cause();
    const bool deepWake = wake == ESP_SLEEP_WAKEUP_TIMER || wake == ESP_SLEEP_WAKEUP_EXT0;
    if (!deepWake || retained.magic != STATE_MAGIC) retained = RetainedState{};
    bootReport = !deepWake;
    buttonReport = wake == ESP_SLEEP_WAKEUP_EXT0;
    if (!prefs.begin("bp-personal", false)) fatal("[STATE] Cannot open NVS");
    const size_t stored = prefs.getBytesLength("state");
    if (stored) {
        if (stored != sizeof(state) || prefs.getBytes("state", &state, sizeof(state)) != sizeof(state) ||
            state.magic != STATE_MAGIC || state.owner != PERSONAL_DEVICE_ID || state.profile > PROFILE_LOST)
            fatal("[STATE] Invalid or different-device state; explicit reprovision required");
    }
    // After power loss, an unknown clock cannot safely extend a lost-mode timer.
    if (state.profile == PROFILE_LOST && (!utc() || !state.lostUntil || utc() >= state.lostUntil)) {
        applyProfile(LOST_MODE_FALLBACK); saveState();
    }
    lostStartedMs = millis();
    bool longButton = false;
    if (digitalRead(USER_BUTTON) == LOW) {
        const uint32_t pressed = millis();
        while (digitalRead(USER_BUTTON) == LOW && millis() - pressed < 2000) delay(10);
        if (millis() - pressed >= 2000) {
            longButton = true;
            applyProfile(state.profile == PROFILE_LOST ? PROFILE_ACTIVE : PROFILE_LOST);
            saveState();
        }
        buttonReport = true;
    }
    if (buttonReport && !longButton) {
        startFinder(BUZZER_CHIRP, 1);
        // Complete the immediate feedback before the blocking BLE scan starts.
        while (finder.active()) { tickFinder(); delay(5); }
    }
    Serial.printf("[PERSONAL] collar=%u hub=%04X profile=%s; real GNSS, LoRa only\n",
        PERSONAL_DEVICE_ID, PERSONAL_HUB_ID, bp_profile_name(bp_profile_t(state.profile)));
    radioSPI.begin(RADIO_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);
    const int16_t result = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING,
        LORA_CODING_RATE, LORA_SYNC_WORD, bp_profile_config(bp_profile_t(state.profile))->tx_power_dBm,
        LORA_PREAMBLE_LEN);
    radioReady = result == RADIOLIB_ERR_NONE;
    Serial.printf("[RADIO] Init result=%d\n", result);
    if (!radioReady) { Serial.printf("[RADIO] Init failed %d\n", result); sleepFor(60); }
    if (radio.setCRC(LORA_CRC_ENABLED) != RADIOLIB_ERR_NONE) { radioReady = false; sleepFor(60); }
    radio.setDio1Action(onRadio);
}

void loop() {
    enforceLostTimeout();
    const uint32_t cycleStart = millis();
    freshFix = false; gpsFailed = false;
    const bool seen = state.profile == PROFILE_LOST ? false : scanHome();
    // Count scheduled Home wakes, including the first missed-beacon scan.
    // Hysteresis retains Home for that scan; it must not suppress the check-in.
    if (retained.home) ++retained.homeCycles;
    const auto* profile = bp_profile_config(bp_profile_t(state.profile));
    const bool lost = state.profile == PROFILE_LOST;
    // Home suppression is safe only after at least one real position exists.
    const bool gnssDue = bootReport || buttonReport || !utc() || !retained.fixTime || !retained.home || lost ||
        (retained.home && retained.homeCycles % profile->home_gnss_refresh_ratio == 0);
    if (lost) advertiseFind();
    if (gnssDue) acquireGps(bootReport || !retained.fixTime);
    if (state.profile == PROFILE_LOST && !state.lostUntil && utc()) {
        const uint32_t spent = (millis() - lostStartedMs) / 1000;
        state.lostUntil = utc() + (spent < LOST_MODE_MAX_DURATION_S ? LOST_MODE_MAX_DURATION_S - spent : 0);
        saveState();
    }
    const bool reportDue = bootReport || buttonReport || lost || gnssDue ||
        (retained.home && retained.homeCycles % profile->wake_checkin_ratio == 0);
    // No build-time or invented timestamps: wait for GNSS time on first boot.
    if (reportDue && utc()) {
        const uint8_t reason = bootReport ? TX_BOOT : buttonReport ? TX_INTERRUPT :
            retained.home && !gnssDue ? TX_WAKE_CHECKIN : TX_TELEMETRY;
        reportLength = buildPacket(report, reason, seen);
        transmit(report, reportLength);
        receiveWindow(seen);
        bootReport = false;
    } else if (!utc()) Serial.println("[TIME] Report suppressed until GNSS UTC is available");
    buttonReport = false;
    enforceLostTimeout();
    profile = bp_profile_config(bp_profile_t(state.profile));
    if (state.profile != PROFILE_LOST) sleepFor(profile->sleep_interval_s);
    advertiseFind();
    // Lost mode keeps GNSS running, with bounded reporting and a two-hour exit.
    while (millis() - cycleStart < LOST_MODE_CYCLE_INTERVAL_S * 1000UL) {
        pumpGps(); enforceLostTimeout();
        digitalWrite(USER_LED, (millis() % 2000) < 100 ? HIGH : LOW);
        delay(20);
    }
}
