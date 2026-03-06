/*
  ┌──────────────────────────────────────────────────────────┐
  │  BLUEPAWS V4 — COLLAR FIRMWARE                           │
  │  nRF52840 + SX1262 LoRa + L76K GNSS + BG77 NB-IoT      │
  │  FreeRTOS task-based architecture                        │
  └──────────────────────────────────────────────────────────┘

  NORMAL / POWERSAVE / ACTIVE cycle:
    1. Wake from deep sleep (only RTC running)
    2. BLE scan for home beacon (10s)
    3. If home → go back to sleep (no GPS, no TX)
    4. If NOT home → GPS two-phase acquisition:
       a) Phase 1: TTFF — wait up to 20s for initial fix
       b) Phase 2: Stabilisation — wait 10s for accuracy
    5. Build TLV packet, transmit via LoRa
    6. Listen for commands from hub (2s RX window)
    7. Every Nth cycle → also send via NB-IoT cellular
    8. Power down all peripherals, deep sleep until next cycle

  LOST MODE (emergency):
    - No sleep — stays awake for up to 2 hours
    - GPS continuous (kept on between transmissions)
    - LoRa at full power (22 dBm), TX every 30s
    - Cellular every 3rd cycle (increased over normal)
    - LED beacon flashing continuously
    - Auto-reverts to ACTIVE after 2-hour safety timer

  SLEEP DISCIPLINE:
    During sleep only the nRF52840 RTC/ULP is running.
    GPS module: sleep pin LOW
    LoRa SX1262: sleep mode
    BLE SoftDevice: disabled
    BG77 cellular: PSM + eDRX configured
*/

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <TinyGPS++.h>
#include <bluefruit.h>

#include <bp_protocol.h>
#include <bp_config.h>
#include <bp_crypto.h>
#include "collar_pins.h"

// FreeRTOS (built into Adafruit nRF52 BSP)
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

// ═══════════════════════════════════════════════
// Device Identity — set per collar at provisioning
// ═══════════════════════════════════════════════
#ifndef MY_DEVICE_ID
#define MY_DEVICE_ID  0x0001
#endif

// ═══════════════════════════════════════════════
// Hardware Instances
// ═══════════════════════════════════════════════
SPIClass loraSPI(NRF_SPIM2, PIN_LORA_MISO, PIN_LORA_SCK, PIN_LORA_MOSI);
SX1262   lora = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);

TinyGPSPlus   gps;
Uart          gpsSerial(Serial1);   // nRF52 hardware UART

// ═══════════════════════════════════════════════
// Global State (protected by mutex where needed)
// ═══════════════════════════════════════════════
static SemaphoreHandle_t gpsMutex;

// Profile & mode
static volatile bp_profile_t currentProfile = PROFILE_NORMAL;
static const bp_profile_config_t *currentConfig = &BP_PROFILES[0];

// Counters
static uint32_t messageSeq     = 0;
static uint32_t cycleCount     = 0;   // total wake cycles
static uint8_t  homeCycleCount = 0;   // consecutive BLE home detections

// GPS state
static volatile bool gpsAwake     = false;
static volatile bool gpsWarmStart = false;
static volatile bool gpsFix       = false;

// BLE state
static volatile bool bleHomeFound = false;

// Command deduplication — ignore retransmitted commands from hub
static uint32_t lastProcessedCmdSeq = 0;

// Lost mode
static volatile bool     inLostMode      = false;
static volatile uint32_t lostModeStartMs = 0;

// AES-128 encryption key
static const uint8_t aesKey[16] = LORA_AES_KEY;

// Cellular
static volatile bool cellularPending = false;
static uint8_t lastTxPacket[BP_MAX_PACKET_SIZE];
static uint8_t lastTxPacketLen = 0;
static bool cellularInitialised = false;

// ═══════════════════════════════════════════════
// Task Handles
// ═══════════════════════════════════════════════
static TaskHandle_t cycleTaskHandle  = NULL;
static TaskHandle_t gpsTaskHandle    = NULL;
static TaskHandle_t cellTaskHandle   = NULL;

#define STACK_CYCLE  4096
#define STACK_GPS    2048
#define STACK_CELL   3072

#define PRIO_CYCLE   3
#define PRIO_GPS     2
#define PRIO_CELL    1

// ═══════════════════════════════════════════════
// Forward Declarations
// ═══════════════════════════════════════════════
static void cycleTask(void *param);
static void gpsFeederTask(void *param);
static void cellularTask(void *param);

// Cycle phases
static bool     bleScanForHome();
static bool     gpsAcquireFix();
static void     listenForCommands();
static void     enterDeepSleep();
static void     runLostMode();

// Peripheral power management
static void     peripheralsWake();
static void     peripheralsSleep();

// Packet builders
static void     sendTelemetry();
static void     sendModeAck(uint32_t cmdMsgSeq);
static void     sendStatusResponse(uint32_t cmdMsgSeq);
static void     sendLostModeAlert();
static void     transmitPacket(uint8_t *buf, uint8_t len);

// Command handling
static void     handleReceivedCommand(const uint8_t *buf, uint8_t len);
static void     applyProfile(bp_profile_t profile);
static void     sendFindAck(uint32_t cmdMsgSeq);

// GPS helpers
static void     gpsModuleWake();
static void     gpsModuleSleep();
static uint32_t gpsGetUnixTime();

// LED
static void     ledFlicker(uint8_t count, uint16_t onMs, uint16_t offMs);
static void     ledBeacon();

// Buzzer (passive piezo via PWM)
static void     buzzerInit();
static void     buzzerPlayPattern(bp_buzzer_pattern_t pattern);
static void     buzzerTone(uint16_t freqHz, uint16_t durationMs);
static void     buzzerOff();

// Cellular helpers
static void     cellularSendTlv(const uint8_t *pkt, uint8_t len);
static void     cellularConfigurePSM();
static bool     cellularSendAT(const char *cmd, const char *expect, uint16_t timeoutMs);

// ═══════════════════════════════════════════════
// BLE Scan Callback
// ═══════════════════════════════════════════════
static void bleScanCallback(ble_gap_evt_adv_report_t *report) {
    uint8_t buf[32];
    uint8_t len = Bluefruit.Scanner.parseReportByType(
        report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, buf, sizeof(buf));

    if (len > 0) {
        buf[len] = '\0';
        if (strcmp((const char *)buf, BLE_HOME_BEACON_NAME) == 0) {
            bleHomeFound = true;
            Bluefruit.Scanner.stop();
            Serial.println("[BLE] Home beacon found!");
        }
    }

    Bluefruit.Scanner.resume();
}

// ═══════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) {}

    Serial.println("══════════════════════════════════");
    Serial.println("  Bluepaws V4 — Collar");
    Serial.printf("  Device: %s (0x%04X)\n", bp_device_name(MY_DEVICE_ID), MY_DEVICE_ID);
    Serial.printf("  Protocol v%d | Max %dB packet\n", BP_PROTOCOL_VERSION, BP_MAX_PACKET_SIZE);
    Serial.println("══════════════════════════════════");

    // ── Mutex ──
    gpsMutex = xSemaphoreCreateMutex();

    // ── LED ──
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    ledFlicker(3, 30, 30);  // boot indicator

    // ── Button ──
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // ── Buzzer (passive piezo) ──
    buzzerInit();

    // ── GPS Init (start sleeping) ──
    pinMode(PIN_GPS_SLEEP, OUTPUT);
    pinMode(PIN_GPS_RESET, OUTPUT);
    digitalWrite(PIN_GPS_RESET, HIGH);
    digitalWrite(PIN_GPS_SLEEP, LOW);
    gpsAwake = false;
    gpsSerial.begin(GPS_BAUD_RATE);
    Serial.println("[GPS] UART initialised (sleeping)");

    // ── LoRa Init ──
    loraSPI.begin();
    Serial.println("[LORA] Initialising SX1262...");

    int state = lora.begin(LORA_FREQUENCY);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] FATAL: init failed (%d)\n", state);
        ledFlicker(12, 80, 80);
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    lora.setSpreadingFactor(LORA_SPREADING);
    lora.setBandwidth(LORA_BANDWIDTH);
    lora.setCodingRate(LORA_CODING_RATE);
    lora.setPreambleLength(LORA_PREAMBLE_LEN);
    lora.setSyncWord(LORA_SYNC_WORD);
    lora.setCRC(LORA_CRC_ENABLED);
    lora.setOutputPower(currentConfig->tx_power_dBm);
    lora.sleep();  // start in sleep mode

    Serial.printf("[LORA] Ready: %.1fMHz SF%d BW%.0fkHz %ddBm\n",
                  LORA_FREQUENCY, LORA_SPREADING, LORA_BANDWIDTH,
                  currentConfig->tx_power_dBm);
    Serial.printf("[LORA] AES-128: %s\n",
                  bp_aes_key_is_zero(aesKey) ? "OFF (key all zeros)" : "ENABLED");

    // ── BLE Init ──
    Bluefruit.begin(0, 1);  // 0 peripheral, 1 central
    Bluefruit.setName("BP_COLLAR");
    Bluefruit.Scanner.setRxCallback(bleScanCallback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.setInterval(160, 80);  // 100ms interval, 50ms window
    Bluefruit.Scanner.useActiveScan(true);
    Serial.printf("[BLE] Ready, beacon: \"%s\"\n", BLE_HOME_BEACON_NAME);

    // ── Cellular Init (UART only — modem stays off until needed) ──
    Serial1.begin(CELLULAR_BAUD_RATE);
    pinMode(PIN_CELL_PWR, OUTPUT);
    pinMode(PIN_CELL_RST, OUTPUT);
    digitalWrite(PIN_CELL_PWR, LOW);
    digitalWrite(PIN_CELL_RST, HIGH);
    Serial.println("[CELL] BG77 UART ready (modem off)");

    // ── Create Tasks ──
    xTaskCreate(cycleTask,     "cycle", STACK_CYCLE, NULL, PRIO_CYCLE, &cycleTaskHandle);
    xTaskCreate(gpsFeederTask, "gps",   STACK_GPS,   NULL, PRIO_GPS,   &gpsTaskHandle);
    xTaskCreate(cellularTask,  "cell",  STACK_CELL,  NULL, PRIO_CELL,  &cellTaskHandle);

    Serial.println("[INIT] Tasks created. Entering first cycle.");
    Serial.println("──────────────────────────────────");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}

// ═══════════════════════════════════════════════════════════
// MAIN CYCLE TASK
// Orchestrates the collar's wake/sense/transmit/sleep loop.
// ═══════════════════════════════════════════════════════════
static void cycleTask(void *param) {
    (void)param;
    vTaskDelay(pdMS_TO_TICKS(500));

    for (;;) {
        // ── Lost mode runs its own continuous loop ──
        if (inLostMode) {
            runLostMode();
            continue;  // after lost mode ends, resume normal cycle
        }

        cycleCount++;
        Serial.printf("\n[CYCLE %lu] %s | interval %ds\n",
                      cycleCount, bp_profile_name(currentProfile),
                      currentConfig->sleep_interval_s);

        // ── Wake peripherals for this cycle ──
        peripheralsWake();

        // ── Phase 1: BLE scan for home beacon ──
        bool atHome = bleScanForHome();

        if (atHome) {
            homeCycleCount++;
            Serial.printf("[CYCLE] Home (x%d). Skipping GPS/TX.\n", homeCycleCount);
            peripheralsSleep();
            enterDeepSleep();
            continue;
        }

        homeCycleCount = 0;

        // ── Phase 2: GPS two-phase acquisition ──
        bool haveFix = gpsAcquireFix();

        // ── Phase 3: Build TLV and transmit via LoRa ──
        sendTelemetry();

        // ── Phase 4: Listen for commands from hub ──
        listenForCommands();

        // ── Phase 5: Cellular (every Nth cycle per profile) ──
        uint8_t cellRatio = currentConfig->cellular_ratio;
        if (cellRatio > 0 && (cycleCount % cellRatio == 0)) {
            Serial.printf("[CELL] Triggering (cycle %lu, ratio 1:%d)\n",
                          cycleCount, cellRatio);
            cellularPending = true;
            xTaskNotifyGive(cellTaskHandle);
            // Cellular runs async — don't block
        }

        // ── Phase 6: Power down everything, deep sleep ──
        peripheralsSleep();
        enterDeepSleep();
    }
}

// ═══════════════════════════════════════════════════════════
// LOST MODE — Continuous operation (no sleep)
// Runs until 2-hour safety timer expires or mode changed.
// GPS stays on, LoRa at max power, LED beacon active.
// ═══════════════════════════════════════════════════════════
static void runLostMode() {
    Serial.println("\n[LOST] ═══ ENTERING LOST MODE ═══");
    Serial.println("[LOST] GPS continuous, LoRa max power, no sleep");
    Serial.printf("[LOST] Safety timer: %ds, TX every %ds\n",
                  LOST_MODE_MAX_DURATION_S, LOST_MODE_CYCLE_INTERVAL_S);

    // Ensure everything is awake
    peripheralsWake();
    gpsModuleWake();  // GPS stays on for entire lost mode

    uint32_t lastTxTime = 0;
    uint32_t lostCycleCount = 0;

    while (inLostMode) {
        // ── Safety timer check ──
        uint32_t elapsed = (millis() - lostModeStartMs) / 1000;
        if (elapsed >= LOST_MODE_MAX_DURATION_S) {
            Serial.println("[LOST] 2-hour timeout — reverting to active");
            sendLostModeAlert();
            applyProfile(LOST_MODE_FALLBACK);
            peripheralsSleep();
            break;
        }

        // ── LED beacon (continuous flash) ──
        ledBeacon();

        // ── TX cycle every LOST_MODE_CYCLE_INTERVAL_S ──
        uint32_t now = millis();
        if (now - lastTxTime >= LOST_MODE_CYCLE_INTERVAL_S * 1000UL) {
            lostCycleCount++;
            cycleCount++;

            Serial.printf("[LOST] TX cycle %lu (elapsed %lus / %ds)\n",
                          lostCycleCount, elapsed, LOST_MODE_MAX_DURATION_S);

            sendTelemetry();
            listenForCommands();

            // Cellular at increased rate (per profile cellular_ratio)
            uint8_t cellRatio = currentConfig->cellular_ratio;
            if (cellRatio > 0 && (lostCycleCount % cellRatio == 0)) {
                Serial.printf("[CELL] Lost mode cellular TX (1:%d)\n", cellRatio);
                cellularPending = true;
                xTaskNotifyGive(cellTaskHandle);
            }

            lastTxTime = now;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    Serial.println("[LOST] ═══ EXITED LOST MODE ═══");
}

// ═══════════════════════════════════════════════
// Peripheral Power Management
// ═══════════════════════════════════════════════

// Wake all peripherals for a cycle
static void peripheralsWake() {
    // LoRa: wake from sleep to standby
    lora.standby();

    // BLE SoftDevice is managed by Bluefruit — scanner starts in bleScanForHome()
    // GPS is woken only if needed (not home) — managed in gpsAcquireFix()
    // Cellular modem stays off until triggered
}

// Power down all peripherals for deep sleep
static void peripheralsSleep() {
    // GPS module sleep
    gpsModuleSleep();

    // LoRa into sleep mode (sub-uA)
    lora.sleep();

    // BLE scanner stop
    Bluefruit.Scanner.stop();

    // Cellular modem should already be in PSM after any transmission
    // No action needed here — BG77 PSM handles its own sleep
}

// ═══════════════════════════════════════════════
// BLE Scan for Home Beacon
// Returns true if home beacon detected within scan window.
// ═══════════════════════════════════════════════
static bool bleScanForHome() {
    Serial.printf("[BLE] Scanning %ds for \"%s\"...\n",
                  BLE_SCAN_DURATION_S, BLE_HOME_BEACON_NAME);

    bleHomeFound = false;
    Bluefruit.Scanner.start(BLE_SCAN_DURATION_S * 100);  // units of 10ms

    uint32_t scanStart = millis();
    uint32_t scanTimeoutMs = BLE_SCAN_DURATION_S * 1000UL;
    while (!bleHomeFound && (millis() - scanStart < scanTimeoutMs)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    Bluefruit.Scanner.stop();

    if (bleHomeFound) {
        Serial.println("[BLE] Home beacon DETECTED");
    } else {
        Serial.println("[BLE] Home beacon NOT found");
    }

    return bleHomeFound;
}

// ═══════════════════════════════════════════════
// GPS Two-Phase Acquisition
//
// Phase 1 — TTFF:
//   Wake GPS, wait up to timeout for initial fix.
//   Warm start: 15s, Cold start: 20s.
//
// Phase 2 — Stabilisation:
//   Once initial fix detected, wait 10s for the
//   position to settle before reading coordinates.
//
// Returns true if usable fix obtained.
// ═══════════════════════════════════════════════
static bool gpsAcquireFix() {
    gpsModuleWake();
    gpsFix = false;

    uint32_t ttffTimeoutS = gpsWarmStart ? GPS_TTFF_WARM_TIMEOUT_S : GPS_TTFF_COLD_TIMEOUT_S;
    Serial.printf("[GPS] Phase 1: TTFF (%s, %lus timeout)\n",
                  gpsWarmStart ? "warm" : "cold", ttffTimeoutS);

    // ── Phase 1: Wait for initial fix ──
    uint32_t phase1Start = millis();
    uint32_t ttffTimeoutMs = ttffTimeoutS * 1000UL;
    bool initialFixFound = false;

    while (millis() - phase1Start < ttffTimeoutMs) {
        if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool locValid = gps.location.isValid() && gps.location.age() < 5000;
            if (locValid) {
                initialFixFound = true;
                gpsWarmStart = true;
                xSemaphoreGive(gpsMutex);
                Serial.printf("[GPS] Initial fix after %lums\n",
                              millis() - phase1Start);
                break;
            }
            xSemaphoreGive(gpsMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!initialFixFound) {
        Serial.println("[GPS] TTFF timeout — no fix");
        return false;
    }

    // ── Phase 2: Stabilisation (10s) ──
    Serial.printf("[GPS] Phase 2: Stabilising %ds...\n", GPS_STABILISATION_S);
    uint32_t stabStart = millis();
    uint32_t stabMs = GPS_STABILISATION_S * 1000UL;

    while (millis() - stabStart < stabMs) {
        // GPS feeder task continues parsing in background
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Check fix is still valid after stabilisation
    if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool stillValid = gps.location.isValid() && gps.location.age() < 5000;
        if (stillValid) {
            gpsFix = true;
            Serial.printf("[GPS] Fix stabilised: %.6f, %.6f (sats: %d)\n",
                          gps.location.lat(), gps.location.lng(),
                          gps.satellites.isValid() ? gps.satellites.value() : 0);
        } else {
            // Fix was lost during stabilisation — use what we had
            gpsFix = gps.location.isValid();
            Serial.println("[GPS] Fix degraded during stabilisation");
        }
        xSemaphoreGive(gpsMutex);
    }

    if (!gpsFix) {
        Serial.println("[GPS] No stable fix — TX without position");
    }

    return gpsFix;
}

// ═══════════════════════════════════════════════
// Build and Send Telemetry (PKT_TELEMETRY)
// ═══════════════════════════════════════════════
static void sendTelemetry() {
    messageSeq++;

    bp_status_t status;
    uint16_t flags = PKT_TELEMETRY;

    if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool locValid = gps.location.isValid() &&
                        gps.location.age() < (GPS_STALE_THRESHOLD_S * 1000UL);

        if (locValid) {
            status = STATUS_OUT_AND_ABOUT;
            flags |= FLAG_HAS_GPS;
        } else {
            status = STATUS_INVALID_GPS;
        }

        if (gpsWarmStart) flags |= FLAG_GPS_WARM;

        uint32_t unixTime = gpsGetUnixTime();

        // Build packet
        uint8_t buf[BP_MAX_PACKET_SIZE];
        pkt_init(buf, MY_DEVICE_ID, messageSeq, unixTime, status, flags);

        if (flags & FLAG_HAS_GPS) {
            int32_t lat_e7 = (int32_t)(gps.location.lat() * 1e7);
            int32_t lon_e7 = (int32_t)(gps.location.lng() * 1e7);
            pkt_set_gps(buf, lat_e7, lon_e7);

            uint16_t acc_m = gps.hdop.isValid() ? (uint16_t)(gps.hdop.hdop() * 5) : 0;
            uint16_t fix_age_s = (uint16_t)(gps.location.age() / 1000);
            uint16_t batt_mV = 3700;  // TODO: ADC reading
            pkt_set_quality(buf, batt_mV, acc_m, fix_age_s);
        } else {
            uint16_t batt_mV = 3700;  // TODO: ADC reading
            pkt_set_quality(buf, batt_mV, 0, 0);
        }

        xSemaphoreGive(gpsMutex);

        // TLV payload
        pkt_add_tlv_u8(buf,  TLV_PROFILE,        currentProfile);
        pkt_add_tlv_i8(buf,  TLV_TX_POWER,       currentConfig->tx_power_dBm);
        pkt_add_tlv_u16(buf, TLV_SLEEP_INTERVAL,  currentConfig->sleep_interval_s);
        pkt_add_tlv_u8(buf,  TLV_GPS_WARM,        gpsWarmStart ? 1 : 0);
        pkt_add_tlv_u8(buf,  TLV_HOME_CYCLES,     homeCycleCount);

        if (inLostMode) {
            uint32_t lostElapsed = (millis() - lostModeStartMs) / 1000;
            pkt_add_tlv_u32(buf, TLV_LOST_MODE_S, lostElapsed);
        }

        uint8_t pktLen = pkt_finalize(buf);

        Serial.printf("[TX] TELEMETRY seq=%lu status=%s size=%dB\n",
                      messageSeq, bp_status_display(status), pktLen);
        pkt_print_hex(buf, pktLen);

        transmitPacket(buf, pktLen);

        // Stash copy for cellular task
        memcpy(lastTxPacket, buf, pktLen);
        lastTxPacketLen = pktLen;
    } else {
        Serial.println("[TX] GPS mutex timeout — skipping");
    }
}

// ═══════════════════════════════════════════════
// Send Mode ACK (PKT_MODE_ACK)
// ═══════════════════════════════════════════════
static void sendModeAck(uint32_t cmdMsgSeq) {
    messageSeq++;
    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, messageSeq, 0, STATUS_OK, PKT_MODE_ACK);

    pkt_add_tlv_u8(buf,  TLV_PROFILE,       currentProfile);
    pkt_add_tlv_i8(buf,  TLV_TX_POWER,      currentConfig->tx_power_dBm);
    pkt_add_tlv_u16(buf, TLV_SLEEP_INTERVAL, currentConfig->sleep_interval_s);
    pkt_add_tlv_u32(buf, TLV_CMD_MSG_ID,    cmdMsgSeq);

    uint8_t pktLen = pkt_finalize(buf);
    Serial.printf("[TX] MODE_ACK for cmd seq %lu\n", cmdMsgSeq);
    transmitPacket(buf, pktLen);
}

// ═══════════════════════════════════════════════
// Send Status Response (PKT_STATUS_RESP)
// ═══════════════════════════════════════════════
static void sendStatusResponse(uint32_t cmdMsgSeq) {
    messageSeq++;
    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, messageSeq, 0, STATUS_OK, PKT_STATUS_RESP);

    pkt_add_tlv_u8(buf,  TLV_PROFILE,        currentProfile);
    pkt_add_tlv_i8(buf,  TLV_TX_POWER,       currentConfig->tx_power_dBm);
    pkt_add_tlv_u16(buf, TLV_SLEEP_INTERVAL,  currentConfig->sleep_interval_s);
    pkt_add_tlv_u8(buf,  TLV_GPS_WARM,        gpsWarmStart ? 1 : 0);
    pkt_add_tlv_u8(buf,  TLV_HOME_CYCLES,     homeCycleCount);
    pkt_add_tlv_u32(buf, TLV_CMD_MSG_ID,     cmdMsgSeq);

    uint8_t pktLen = pkt_finalize(buf);
    Serial.printf("[TX] STATUS_RESP for cmd seq %lu\n", cmdMsgSeq);
    transmitPacket(buf, pktLen);
}

// ═══════════════════════════════════════════════
// Send Find ACK (PKT_FIND_ACK)
// ═══════════════════════════════════════════════
static void sendFindAck(uint32_t cmdMsgSeq) {
    messageSeq++;
    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, messageSeq, 0, STATUS_OK, PKT_FIND_ACK);

    pkt_add_tlv_u32(buf, TLV_CMD_MSG_ID, cmdMsgSeq);
    pkt_add_tlv_u8(buf,  TLV_PROFILE,    currentProfile);

    uint8_t pktLen = pkt_finalize(buf);
    Serial.printf("[TX] FIND_ACK for cmd seq %lu\n", cmdMsgSeq);
    transmitPacket(buf, pktLen);
}

// ═══════════════════════════════════════════════
// Send Lost Mode Timeout Alert (PKT_ALERT)
// ═══════════════════════════════════════════════
static void sendLostModeAlert() {
    messageSeq++;
    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, messageSeq, 0, STATUS_LOST_TIMEOUT, PKT_ALERT);

    uint32_t duration = (millis() - lostModeStartMs) / 1000;
    pkt_add_tlv_u32(buf, TLV_DURATION_S, duration);
    pkt_add_tlv_u8(buf,  TLV_NEW_MODE,  (uint8_t)LOST_MODE_FALLBACK);

    uint8_t pktLen = pkt_finalize(buf);
    Serial.printf("[TX] ALERT: lost mode timeout after %lus\n", duration);
    transmitPacket(buf, pktLen);
}

// ═══════════════════════════════════════════════
// LoRa Transmit
// ═══════════════════════════════════════════════
static void transmitPacket(uint8_t *buf, uint8_t len) {
    // Encrypt payload if AES key is configured
    if (!bp_aes_key_is_zero(aesKey)) {
        bp_aes_ctr_apply(buf, len, aesKey);
    }

    lora.standby();
    int state = lora.transmit(buf, len);

    if (state == RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] TX OK (%d bytes)\n", len);
        ledFlicker(currentConfig->led_flashes, 50, 50);
    } else if (state == RADIOLIB_ERR_TX_TIMEOUT) {
        Serial.println("[LORA] TX timeout");
        ledFlicker(2, 200, 200);
    } else {
        Serial.printf("[LORA] TX failed: %d\n", state);
        ledFlicker(6, 80, 80);
    }
}

// ═══════════════════════════════════════════════
// Listen for Commands (2s RX window)
// ═══════════════════════════════════════════════
static void listenForCommands() {
    Serial.printf("[RX] Listening %dms...\n", CMD_LISTEN_WINDOW_MS);

    int rxState = lora.startReceive();
    if (rxState != RADIOLIB_ERR_NONE) {
        Serial.printf("[RX] startReceive failed: %d\n", rxState);
        return;
    }

    uint32_t listenStart = millis();
    while (millis() - listenStart < CMD_LISTEN_WINDOW_MS) {
        uint16_t irq = lora.getIrqStatus();
        if (irq & RADIOLIB_SX126X_IRQ_RX_DONE) {
            uint8_t rxBuf[BP_MAX_PACKET_SIZE];
            int state = lora.readData(rxBuf, sizeof(rxBuf));
            if (state == RADIOLIB_ERR_NONE) {
                uint8_t rxLen = lora.getPacketLength();
                Serial.printf("[RX] Received %d bytes\n", rxLen);

                // Decrypt if AES key is configured
                if (!bp_aes_key_is_zero(aesKey)) {
                    bp_aes_ctr_apply(rxBuf, rxLen, aesKey);
                }

                pkt_print_hex(rxBuf, rxLen);

                if (rxLen >= BP_MIN_PACKET_SIZE && rxBuf[0] == BP_PROTOCOL_VERSION) {
                    handleReceivedCommand(rxBuf, rxLen);
                }
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    lora.standby();
}

// ═══════════════════════════════════════════════
// Command Handler
// ═══════════════════════════════════════════════
static void handleReceivedCommand(const uint8_t *buf, uint8_t len) {
    if (!pkt_validate_crc(buf, len)) {
        Serial.println("[RX] CRC failed — dropping");
        return;
    }

    uint16_t targetId = pkt_device_id(buf);
    if (targetId != MY_DEVICE_ID && targetId != DEVICE_ID_BROADCAST) {
        Serial.printf("[RX] Not for us (0x%04X)\n", targetId);
        return;
    }

    uint16_t pktType = pkt_pkt_type(buf);
    uint32_t cmdSeq  = pkt_msg_seq(buf);

    // ── Deduplication: hub retries up to 3x, ignore already-processed commands ──
    if (cmdSeq != 0 && cmdSeq == lastProcessedCmdSeq) {
        Serial.printf("[RX] Duplicate cmd seq %lu — ignoring\n", cmdSeq);
        return;
    }
    lastProcessedCmdSeq = cmdSeq;

    switch (pktType) {
    case PKT_CMD_MODE: {
        uint8_t newProfile;
        if (pkt_tlv_get_u8(buf, TLV_PROFILE, &newProfile)) {
            Serial.printf("[RX] CMD_MODE → %s (seq %lu)\n",
                          bp_profile_name((bp_profile_t)newProfile), cmdSeq);
            applyProfile((bp_profile_t)newProfile);
            sendModeAck(cmdSeq);
        } else {
            Serial.println("[RX] CMD_MODE missing TLV_PROFILE");
        }
        break;
    }
    case PKT_CMD_STATUS:
        Serial.printf("[RX] CMD_STATUS (seq %lu)\n", cmdSeq);
        sendStatusResponse(cmdSeq);
        break;
    case PKT_CMD_FIND: {
        Serial.printf("[RX] CMD_FIND (seq %lu)\n", cmdSeq);

        // LED flash — run ledFlicker once per command
        uint8_t flashCount = 5;  // default
        pkt_tlv_get_u8(buf, TLV_LED_FLASH, &flashCount);
        if (flashCount > 0) {
            ledFlicker(flashCount, 80, 80);
        }

        // Buzzer pattern
        uint8_t pattern = BUZZER_CHIRP;  // default
        pkt_tlv_get_u8(buf, TLV_BUZZER_PATTERN, &pattern);
        if (pattern != BUZZER_OFF) {
            buzzerPlayPattern((bp_buzzer_pattern_t)pattern);
        }

        sendFindAck(cmdSeq);
        break;
    }
    default:
        Serial.printf("[RX] Unknown type: 0x%04X\n", pktType);
        break;
    }
}

// ═══════════════════════════════════════════════
// Apply Operating Profile
// ═══════════════════════════════════════════════
static void applyProfile(bp_profile_t profile) {
    Serial.printf("[MODE] %s → %s\n",
                  bp_profile_name(currentProfile), bp_profile_name(profile));

    currentProfile = profile;
    currentConfig  = bp_profile_config(profile);

    lora.setOutputPower(currentConfig->tx_power_dBm);

    // Lost mode tracking
    if (profile == PROFILE_LOST) {
        if (!inLostMode) {
            inLostMode = true;
            lostModeStartMs = millis();
            Serial.println("[MODE] LOST MODE ACTIVATED — 2hr safety timer");
        }
    } else {
        if (inLostMode) {
            Serial.println("[MODE] Lost mode deactivated");
        }
        inLostMode = false;
        lostModeStartMs = 0;
    }

    Serial.printf("[MODE] %ddBm | %ds interval | cell 1:%d | GPS %s\n",
                  currentConfig->tx_power_dBm,
                  currentConfig->sleep_interval_s,
                  currentConfig->cellular_ratio,
                  currentConfig->gps_continuous ? "continuous" : "on-demand");
}

// ═══════════════════════════════════════════════
// GPS Feeder Task
// Background NMEA parsing into TinyGPS++.
// ═══════════════════════════════════════════════
static void gpsFeederTask(void *param) {
    (void)param;
    for (;;) {
        if (gpsAwake) {
            if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                while (gpsSerial.available() > 0) {
                    gps.encode(gpsSerial.read());
                }
                xSemaphoreGive(gpsMutex);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

// ═══════════════════════════════════════════════
// Cellular Task
// Blocks on notification. Wakes BG77, configures
// PSM/eDRX, POSTs TLV, then lets modem enter PSM.
// ═══════════════════════════════════════════════
static void cellularTask(void *param) {
    (void)param;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!cellularPending || lastTxPacketLen == 0) continue;

        Serial.println("[CELL] ── Cellular transmission start ──");
        cellularSendTlv(lastTxPacket, lastTxPacketLen);
        cellularPending = false;
        Serial.println("[CELL] ── Cellular transmission complete ──");
    }
}

// ═══════════════════════════════════════════════
// Cellular: Send TLV via NB-IoT
// Wakes BG77, configures PSM/eDRX on first use,
// POSTs the TLV binary, then returns (modem enters PSM).
// ═══════════════════════════════════════════════
static void cellularSendTlv(const uint8_t *pkt, uint8_t len) {
    // ── Power on BG77 ──
    Serial.println("[CELL] Powering on BG77...");
    digitalWrite(PIN_CELL_PWR, HIGH);
    vTaskDelay(pdMS_TO_TICKS(600));
    digitalWrite(PIN_CELL_PWR, LOW);
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Wait for modem ready
    if (!cellularSendAT("AT", "OK", 5000)) {
        Serial.println("[CELL] Modem not responding — aborting");
        return;
    }

    // ── First-time PSM/eDRX configuration ──
    if (!cellularInitialised) {
        cellularConfigurePSM();
        cellularInitialised = true;
    }

    // ── POST the TLV payload ──
    // The TLV binary is sent as-is with FLAG_CELLULAR set.
    // For now, this is a placeholder AT sequence.
    // The server endpoint and auth will be configured at provisioning.

    // TODO: Full AT+QHTTPPOST sequence:
    //   AT+CEREG?                           → check registration
    //   AT+QHTTPCFG="contextid",1
    //   AT+QHTTPCFG="contenttype",4         → application/octet-stream
    //   AT+QHTTPURL=<url_len>,80
    //   <server_url>                         → CONNECT / OK
    //   AT+QHTTPPOST=<len>,80,80
    //   <raw TLV binary with FLAG_CELLULAR>
    //   → +QHTTPPOST: 0,200

    Serial.printf("[CELL] TODO: POST %d bytes TLV\n", len);

    // After POST, modem will enter PSM automatically
    // (configured via AT+CPSMS and AT+CEDRXS)
    Serial.println("[CELL] Modem entering PSM");
}

// ═══════════════════════════════════════════════
// Configure BG77 PSM and eDRX
// Called once on first cellular transmission.
// ═══════════════════════════════════════════════
static void cellularConfigurePSM() {
    Serial.println("[CELL] Configuring PSM/eDRX...");

    // Enable PSM (Power Saving Mode)
    // TAU timer: how often modem does tracking area update
    // Active timer: how long modem stays reachable after activity
    char psmCmd[64];
    snprintf(psmCmd, sizeof(psmCmd),
             "AT+CPSMS=1,,,\"%s\",\"%s\"",
             CELLULAR_PSM_TAU, CELLULAR_PSM_ACTIVE);
    cellularSendAT(psmCmd, "OK", 2000);

    // Enable eDRX (Extended Discontinuous Reception)
    // Reduces how often modem listens for paging during idle
    char edrxCmd[64];
    snprintf(edrxCmd, sizeof(edrxCmd),
             "AT+CEDRXS=1,5,\"%s\"",
             CELLULAR_EDRX_VALUE);
    cellularSendAT(edrxCmd, "OK", 2000);

    Serial.printf("[CELL] PSM: TAU=%s Active=%s\n",
                  CELLULAR_PSM_TAU, CELLULAR_PSM_ACTIVE);
    Serial.printf("[CELL] eDRX: %s (PTW %s)\n",
                  CELLULAR_EDRX_VALUE, CELLULAR_EDRX_PTW);
}

// ═══════════════════════════════════════════════
// Send AT command and wait for expected response
// ═══════════════════════════════════════════════
static bool cellularSendAT(const char *cmd, const char *expect, uint16_t timeoutMs) {
    // Flush any pending data
    while (Serial1.available()) Serial1.read();

    Serial1.println(cmd);
    Serial.printf("[CELL] > %s\n", cmd);

    uint32_t start = millis();
    String response = "";

    while (millis() - start < timeoutMs) {
        while (Serial1.available()) {
            char c = Serial1.read();
            response += c;
        }
        if (response.indexOf(expect) >= 0) {
            Serial.printf("[CELL] < %s\n", expect);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    Serial.printf("[CELL] Timeout waiting for '%s'\n", expect);
    return false;
}

// ═══════════════════════════════════════════════
// GPS Module Wake / Sleep
// ═══════════════════════════════════════════════
static void gpsModuleWake() {
    if (!gpsAwake) {
        digitalWrite(PIN_GPS_SLEEP, HIGH);
        gpsAwake = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        Serial.println("[GPS] Module woken");
    }
}

static void gpsModuleSleep() {
    if (gpsAwake && !currentConfig->gps_continuous) {
        digitalWrite(PIN_GPS_SLEEP, LOW);
        gpsAwake = false;
        Serial.println("[GPS] Module sleeping");
    }
}

static uint32_t gpsGetUnixTime() {
    if (gps.time.isValid() && gps.date.isValid() && gps.time.age() < 60000) {
        return bp_gps_to_unix(gps.date.year(), gps.date.month(), gps.date.day(),
                              gps.time.hour(), gps.time.minute(), gps.time.second());
    }
    return 0;
}

// ═══════════════════════════════════════════════
// Deep Sleep
// Powers down everything except nRF52840 RTC.
// FreeRTOS tickless idle handles the actual
// low-power state — vTaskDelay triggers system-on
// sleep with only RTC running.
// ═══════════════════════════════════════════════
static void enterDeepSleep() {
    uint32_t sleepMs = currentConfig->sleep_interval_s * 1000UL;

    Serial.printf("[SLEEP] %lus (all peripherals off, RTC only)\n",
                  currentConfig->sleep_interval_s);
    Serial.flush();

    // FreeRTOS tickless idle will put the nRF52840 into
    // system-on sleep mode. Only the RTC and ULP remain active.
    // All GPIOs retain state (GPS sleep pin stays LOW, etc).
    vTaskDelay(pdMS_TO_TICKS(sleepMs));

    Serial.println("[WAKE] ──────────────────────");
}

// ═══════════════════════════════════════════════
// LED Helpers
// ═══════════════════════════════════════════════
static void ledFlicker(uint8_t count, uint16_t onMs, uint16_t offMs) {
    for (uint8_t i = 0; i < count; i++) {
        digitalWrite(PIN_LED, HIGH);
        vTaskDelay(pdMS_TO_TICKS(onMs));
        digitalWrite(PIN_LED, LOW);
        vTaskDelay(pdMS_TO_TICKS(offMs));
    }
}

static void ledBeacon() {
    digitalWrite(PIN_LED, HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));
    digitalWrite(PIN_LED, LOW);
    vTaskDelay(pdMS_TO_TICKS(900));  // ~1 flash per second in lost mode
}

// ═══════════════════════════════════════════════
// Buzzer — Passive Piezo (PWM)
// Uses tone() for frequency generation on nRF52840.
// Different patterns let users distinguish collars.
// ═══════════════════════════════════════════════

static void buzzerInit() {
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    Serial.println("[BUZZ] Passive piezo on A4 ready");
}

static void buzzerTone(uint16_t freqHz, uint16_t durationMs) {
    tone(PIN_BUZZER, freqHz, durationMs);
    vTaskDelay(pdMS_TO_TICKS(durationMs));
}

static void buzzerOff() {
    noTone(PIN_BUZZER);
    digitalWrite(PIN_BUZZER, LOW);
}

static void buzzerPlayPattern(bp_buzzer_pattern_t pattern) {
    Serial.printf("[BUZZ] Playing pattern %d\n", pattern);

    switch (pattern) {
    case BUZZER_CHIRP:
        // 3 short chirps — quick "I'm here"
        for (uint8_t i = 0; i < 3; i++) {
            buzzerTone(BUZZER_DEFAULT_FREQ_HZ, BUZZER_NOTE_DURATION_MS);
            vTaskDelay(pdMS_TO_TICKS(BUZZER_PAUSE_MS));
        }
        break;

    case BUZZER_TRILL:
        // Rising trill — ascending 5 notes
        for (uint16_t f = 1800; f <= 3400; f += 400) {
            buzzerTone(f, 100);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        break;

    case BUZZER_SIREN:
        // Two-tone siren — alternating high/low x4
        for (uint8_t i = 0; i < 4; i++) {
            buzzerTone(2200, 200);
            buzzerTone(3200, 200);
        }
        break;

    case BUZZER_MELODY_A:
        // Melody A — collar 1 identifier (short jingle)
        buzzerTone(2637, 150);  // E
        vTaskDelay(pdMS_TO_TICKS(50));
        buzzerTone(2093, 150);  // C
        vTaskDelay(pdMS_TO_TICKS(50));
        buzzerTone(2349, 150);  // D
        vTaskDelay(pdMS_TO_TICKS(50));
        buzzerTone(3136, 300);  // G (long)
        break;

    case BUZZER_MELODY_B:
        // Melody B — collar 2 identifier (different jingle)
        buzzerTone(3136, 150);  // G
        vTaskDelay(pdMS_TO_TICKS(50));
        buzzerTone(2637, 150);  // E
        vTaskDelay(pdMS_TO_TICKS(50));
        buzzerTone(2093, 300);  // C (long)
        vTaskDelay(pdMS_TO_TICKS(50));
        buzzerTone(2093, 150);  // C
        break;

    default:
        break;
    }

    buzzerOff();
}
