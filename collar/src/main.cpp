/*
  ┌──────────────────────────────────────────────────────────┐
  │  BLUEPAWS V4 — COLLAR FIRMWARE                           │
  │  nRF52840 + SX1262 LoRa + L76K GNSS + BG77 NB-IoT      │
  │  FreeRTOS task-based architecture                        │
  └──────────────────────────────────────────────────────────┘

  Main cycle (orchestrated by cycleTask):
    1. Wake from sleep
    2. BLE scan for home beacon (10s)
    3. If home → go back to sleep
    4. If NOT home → start GPS, acquire fix
    5. Build TLV packet, transmit via LoRa
    6. Listen for commands from hub (2s window)
    7. Every Nth cycle → also send via NB-IoT cellular
    8. Go back to sleep

  GPS feeder task runs continuously when GPS is awake,
  parsing NMEA sentences into TinyGPS++ in the background.
*/

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <TinyGPS++.h>
#include <bluefruit.h>

#include <bp_protocol.h>
#include <bp_config.h>
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
static uint32_t messageSeq    = 0;
static uint32_t cycleCount    = 0;   // total wake cycles (for cellular ratio)
static uint8_t  homeCycleCount = 0;  // consecutive BLE home detections

// GPS state
static volatile bool gpsAwake    = false;
static volatile bool gpsWarmStart = false;
static volatile bool gpsFix      = false;  // set by feeder when fix is valid+stable

// BLE state
static volatile bool bleHomeFound = false;

// Lost mode
static volatile bool     inLostMode       = false;
static volatile uint32_t lostModeStartMs  = 0;

// Cellular trigger
static volatile bool cellularPending = false;
static uint8_t lastTxPacket[BP_MAX_PACKET_SIZE];
static uint8_t lastTxPacketLen = 0;

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
static void     buildAndTransmit();
static void     listenForCommands();
static void     enterSleep();

// Packet builders
static void     sendTelemetry();
static void     sendModeAck(uint32_t cmdMsgSeq);
static void     sendStatusResponse(uint32_t cmdMsgSeq);
static void     sendLostModeAlert();
static void     transmitPacket(uint8_t *buf, uint8_t len);

// Command handling
static void     handleReceivedCommand(const uint8_t *buf, uint8_t len);
static void     applyProfile(bp_profile_t profile);

// GPS helpers
static void     gpsWake();
static void     gpsSleep();
static uint32_t gpsGetUnixTime();

// LED
static void     ledFlicker(uint8_t count, uint16_t onMs, uint16_t offMs);
static void     ledBeacon();

// Cellular helpers
static void     cellularSendTlv(const uint8_t *pkt, uint8_t len);

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

    // ── GPS Init ──
    pinMode(PIN_GPS_SLEEP, OUTPUT);
    pinMode(PIN_GPS_RESET, OUTPUT);
    digitalWrite(PIN_GPS_RESET, HIGH);
    digitalWrite(PIN_GPS_SLEEP, LOW);  // start sleeping
    gpsAwake = false;
    gpsSerial.begin(GPS_BAUD_RATE);
    Serial.println("[GPS] UART initialised");

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
    lora.standby();

    Serial.printf("[LORA] Ready: %.1fMHz SF%d BW%.0fkHz %ddBm\n",
                  LORA_FREQUENCY, LORA_SPREADING, LORA_BANDWIDTH,
                  currentConfig->tx_power_dBm);

    // ── BLE Init ──
    Bluefruit.begin(0, 1);  // 0 peripheral, 1 central
    Bluefruit.setName("BP_COLLAR");
    Bluefruit.Scanner.setRxCallback(bleScanCallback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.setInterval(160, 80);  // 100ms interval, 50ms window
    Bluefruit.Scanner.useActiveScan(true);
    Serial.printf("[BLE] Ready, scanning for \"%s\"\n", BLE_HOME_BEACON_NAME);

    // ── Cellular Init ──
    Serial1.begin(CELLULAR_BAUD_RATE);
    pinMode(PIN_CELL_PWR, OUTPUT);
    pinMode(PIN_CELL_RST, OUTPUT);
    digitalWrite(PIN_CELL_PWR, LOW);
    digitalWrite(PIN_CELL_RST, HIGH);
    Serial.println("[CELL] BG77 UART initialised");

    // ── Create Tasks ──
    xTaskCreate(cycleTask,     "cycle", STACK_CYCLE, NULL, PRIO_CYCLE, &cycleTaskHandle);
    xTaskCreate(gpsFeederTask, "gps",   STACK_GPS,   NULL, PRIO_GPS,   &gpsTaskHandle);
    xTaskCreate(cellularTask,  "cell",  STACK_CELL,  NULL, PRIO_CELL,  &cellTaskHandle);

    Serial.println("[INIT] All tasks created. Starting cycle.");
    Serial.println("──────────────────────────────────");
}

void loop() {
    // Arduino loop() yields to FreeRTOS scheduler
    vTaskDelay(pdMS_TO_TICKS(10000));
}

// ═══════════════════════════════════════════════
// MAIN CYCLE TASK
// The core collar operating loop.
// ═══════════════════════════════════════════════
static void cycleTask(void *param) {
    (void)param;

    // Short delay for peripherals to settle
    vTaskDelay(pdMS_TO_TICKS(1000));

    for (;;) {
        cycleCount++;
        Serial.printf("\n[CYCLE %lu] Profile: %s | Interval: %ds\n",
                      cycleCount, bp_profile_name(currentProfile),
                      currentConfig->sleep_interval_s);

        // ── Lost mode timeout check ──
        if (inLostMode) {
            uint32_t elapsed = (millis() - lostModeStartMs) / 1000;
            if (elapsed >= LOST_MODE_MAX_DURATION_S) {
                Serial.println("[LOST] 2-hour timeout reached, reverting");
                sendLostModeAlert();
                applyProfile(LOST_MODE_FALLBACK);
            }
        }

        // ── Phase 1: BLE scan for home beacon ──
        bool atHome = bleScanForHome();

        if (atHome) {
            // Cat is home — no need for GPS or LoRa
            homeCycleCount++;
            Serial.printf("[CYCLE] Home (consecutive: %d). Sleeping.\n", homeCycleCount);
            gpsSleep();
            enterSleep();
            continue;
        }

        // Not home — reset consecutive counter
        homeCycleCount = 0;

        // ── Phase 2: GPS acquisition ──
        bool haveFix = gpsAcquireFix();

        // ── Phase 3: Build and transmit telemetry via LoRa ──
        sendTelemetry();

        // ── Phase 4: Listen for commands from hub ──
        listenForCommands();

        // ── Phase 5: Cellular check (every Nth cycle) ──
        if (cycleCount % CELLULAR_TX_RATIO == 0) {
            Serial.printf("[CELL] Triggering cellular TX (cycle %lu, ratio 1:%d)\n",
                          cycleCount, CELLULAR_TX_RATIO);
            cellularPending = true;
            xTaskNotifyGive(cellTaskHandle);
            // Don't wait — cellular task runs async while we sleep
        }

        // ── Phase 6: Sleep ──
        gpsSleep();
        enterSleep();
    }
}

// ═══════════════════════════════════════════════
// Phase 1: BLE Scan for Home Beacon
// Returns true if home beacon detected within scan window.
// ═══════════════════════════════════════════════
static bool bleScanForHome() {
    Serial.printf("[BLE] Scanning %ds for \"%s\"...\n",
                  BLE_SCAN_DURATION_S, BLE_HOME_BEACON_NAME);

    bleHomeFound = false;
    Bluefruit.Scanner.start(BLE_SCAN_DURATION_S * 100);  // units of 10ms

    // Wait for scan to complete or beacon found
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
// Phase 2: GPS Acquisition
// Wakes GPS, waits for stable fix within timeout.
// Returns true if we got a usable fix.
// ═══════════════════════════════════════════════
static bool gpsAcquireFix() {
    gpsWake();
    gpsFix = false;

    uint32_t timeoutS = gpsWarmStart ? GPS_WARM_START_TIMEOUT_S : GPS_COLD_START_TIMEOUT_S;
    Serial.printf("[GPS] Acquiring fix (%s start, %lus timeout)...\n",
                  gpsWarmStart ? "warm" : "cold", timeoutS);

    uint32_t fixStart = millis();
    uint32_t timeoutMs = timeoutS * 1000UL;
    bool firstFixDetected = false;
    uint32_t firstFixTime = 0;

    while (millis() - fixStart < timeoutMs) {
        // GPS feeder task is parsing NMEA in the background
        if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool locValid = gps.location.isValid() && gps.location.age() < 5000;

            if (!firstFixDetected && locValid) {
                firstFixDetected = true;
                firstFixTime = millis();
                gpsWarmStart = true;
                Serial.println("[GPS] Initial fix! Stabilising...");
            }

            if (firstFixDetected &&
                (millis() - firstFixTime >= GPS_STABILISATION_S * 1000UL)) {
                gpsFix = true;
                xSemaphoreGive(gpsMutex);
                Serial.println("[GPS] Fix stabilised");
                break;
            }

            xSemaphoreGive(gpsMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!gpsFix && firstFixDetected) {
        gpsFix = true;  // use best available
        Serial.println("[GPS] Using initial fix (stabilisation incomplete)");
    }

    if (!gpsFix) {
        Serial.println("[GPS] Fix timeout — transmitting without position");
    }

    return gpsFix;
}

// ═══════════════════════════════════════════════
// Phase 3: Build and Send Telemetry (PKT_TELEMETRY)
// ═══════════════════════════════════════════════
static void sendTelemetry() {
    messageSeq++;

    // Determine status and flags
    bp_status_t status;
    uint16_t flags = PKT_TELEMETRY;

    if (xSemaphoreTake(gpsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool locValid = gps.location.isValid() && gps.location.age() < (GPS_STALE_THRESHOLD_S * 1000UL);

        if (locValid) {
            status = STATUS_OUT_AND_ABOUT;
            flags |= FLAG_HAS_GPS;
        } else {
            status = STATUS_INVALID_GPS;
        }

        if (gpsWarmStart) flags |= FLAG_GPS_WARM;

        // Build timestamp
        uint32_t unixTime = gpsGetUnixTime();

        // Build packet
        uint8_t buf[BP_MAX_PACKET_SIZE];
        pkt_init(buf, MY_DEVICE_ID, messageSeq, unixTime, status, flags);

        // GPS coordinates
        if (flags & FLAG_HAS_GPS) {
            int32_t lat_e7 = (int32_t)(gps.location.lat() * 1e7);
            int32_t lon_e7 = (int32_t)(gps.location.lng() * 1e7);
            pkt_set_gps(buf, lat_e7, lon_e7);

            // Quality metrics
            uint16_t acc_m = gps.hdop.isValid() ? (uint16_t)(gps.hdop.hdop() * 5) : 0;
            uint16_t fix_age_s = (uint16_t)(gps.location.age() / 1000);
            uint16_t batt_mV = 3700;  // TODO: read actual ADC
            pkt_set_quality(buf, batt_mV, acc_m, fix_age_s);
        } else {
            uint16_t batt_mV = 3700;  // TODO: read actual ADC
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

        // Stash for cellular task
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
// LoRa Transmit (with standby bookend)
// ═══════════════════════════════════════════════
static void transmitPacket(uint8_t *buf, uint8_t len) {
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
// Phase 4: Listen for Commands (2s RX window)
// ═══════════════════════════════════════════════
static void listenForCommands() {
    Serial.printf("[RX] Listening %dms for commands...\n", CMD_LISTEN_WINDOW_MS);

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
                pkt_print_hex(rxBuf, rxLen);

                if (rxLen >= BP_MIN_PACKET_SIZE && rxBuf[0] == BP_PROTOCOL_VERSION) {
                    handleReceivedCommand(rxBuf, rxLen);
                }
            }
            break;  // one packet per window
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    lora.standby();
    Serial.println("[RX] Listen window closed");
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
        Serial.printf("[RX] Not for us (target: 0x%04X)\n", targetId);
        return;
    }

    uint16_t pktType = pkt_pkt_type(buf);
    uint32_t cmdSeq  = pkt_msg_seq(buf);

    switch (pktType) {
    case PKT_CMD_MODE: {
        uint8_t newProfile;
        if (pkt_tlv_get_u8(buf, TLV_PROFILE, &newProfile)) {
            Serial.printf("[RX] CMD_MODE: %s (seq %lu)\n",
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
    default:
        Serial.printf("[RX] Unknown pkt type: 0x%04X\n", pktType);
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

    // Apply LoRa TX power immediately
    lora.setOutputPower(currentConfig->tx_power_dBm);

    // Lost mode tracking
    if (profile == PROFILE_LOST) {
        if (!inLostMode) {
            inLostMode = true;
            lostModeStartMs = millis();
            Serial.println("[MODE] Lost mode ACTIVATED — 2hr timer started");
        }
    } else {
        if (inLostMode) {
            Serial.println("[MODE] Lost mode deactivated");
        }
        inLostMode = false;
        lostModeStartMs = 0;
    }

    Serial.printf("[MODE] Power: %ddBm | Interval: %ds | LED: %d | Beacon: %s\n",
                  currentConfig->tx_power_dBm, currentConfig->sleep_interval_s,
                  currentConfig->led_flashes, currentConfig->beacon_enabled ? "ON" : "OFF");
}

// ═══════════════════════════════════════════════
// GPS Feeder Task
// Continuously reads NMEA from UART into TinyGPS++
// when GPS module is awake.
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
            vTaskDelay(pdMS_TO_TICKS(50));  // ~20 Hz parse rate
        } else {
            vTaskDelay(pdMS_TO_TICKS(500)); // idle when GPS sleeping
        }
    }
}

// ═══════════════════════════════════════════════
// Cellular Task
// Waits for notification, then sends the last TLV
// packet via NB-IoT REST POST.
// ═══════════════════════════════════════════════
static void cellularTask(void *param) {
    (void)param;
    for (;;) {
        // Block until notified by cycle task
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!cellularPending || lastTxPacketLen == 0) {
            continue;
        }

        Serial.println("[CELL] Waking BG77 modem...");
        cellularSendTlv(lastTxPacket, lastTxPacketLen);
        cellularPending = false;
    }
}

// ═══════════════════════════════════════════════
// Cellular: Send TLV via NB-IoT AT commands
// Placeholder — AT command sequence TBD
// ═══════════════════════════════════════════════
static void cellularSendTlv(const uint8_t *pkt, uint8_t len) {
    // Power on BG77
    digitalWrite(PIN_CELL_PWR, HIGH);
    vTaskDelay(pdMS_TO_TICKS(500));
    digitalWrite(PIN_CELL_PWR, LOW);
    vTaskDelay(pdMS_TO_TICKS(3000));  // wait for boot

    // TODO: AT command sequence:
    //   AT              → OK
    //   AT+QHTTPCFG="contextid",1
    //   AT+QHTTPCFG="requestheader",1
    //   AT+QHTTPURL=<url_len>,80
    //   <server_url>    → CONNECT / OK
    //   AT+QHTTPPOST=<len>,80,80
    //   <raw TLV binary payload>
    //   → +QHTTPPOST: 0,200
    //
    // The payload is the same TLV binary as LoRa, but with
    // FLAG_CELLULAR set in the flags field to indicate origin.

    Serial.printf("[CELL] TODO: POST %d bytes TLV via NB-IoT\n", len);

    // Power down modem after transmission
    // AT+QPOWD=1
    Serial.println("[CELL] Modem powered down");
}

// ═══════════════════════════════════════════════
// GPS Wake / Sleep
// ═══════════════════════════════════════════════
static void gpsWake() {
    if (!gpsAwake) {
        digitalWrite(PIN_GPS_SLEEP, HIGH);
        gpsAwake = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        Serial.println("[GPS] Module woken");
    }
}

static void gpsSleep() {
    if (gpsAwake) {
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
// Sleep
// Uses nRF52840 system-on sleep with RTC wakeup.
// FreeRTOS tickless idle handles the low-power state.
// ═══════════════════════════════════════════════
static void enterSleep() {
    uint32_t sleepMs = currentConfig->sleep_interval_s * 1000UL;

    // In lost mode, run LED beacon during sleep period
    if (inLostMode && currentConfig->beacon_enabled) {
        Serial.printf("[SLEEP] Lost mode beacon — sleeping %lums with LED\n", sleepMs);
        uint32_t sleepStart = millis();
        while (millis() - sleepStart < sleepMs) {
            ledBeacon();
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    } else {
        Serial.printf("[SLEEP] Sleeping %lums...\n", sleepMs);
        // FreeRTOS tickless idle will put MCU into low-power mode
        vTaskDelay(pdMS_TO_TICKS(sleepMs));
    }

    Serial.println("[WAKE] Cycle starting");
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
}
