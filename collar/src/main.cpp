/*
  ┌──────────────────────────────────────────────────────────┐
  │  BLUEPAWS V4 — COLLAR FIRMWARE                           │
  │  RAK4630 (nRF52840 + SX1262) + Sequans GM02SP           │
  │  FreeRTOS task-based architecture                        │
  └──────────────────────────────────────────────────────────┘

  NORMAL / POWERSAVE / ACTIVE cycle:
    1. Wake from deep sleep (only RTC running)
    2. BLE scan for home beacon (10s)
    3. If home → skip GNSS/LTE, send a short WAKE_CHECKIN raw TLV over LoRa,
       open the command RX window, then go back to sleep.
    4. If NOT home → GPS two-phase acquisition:
       a) Phase 1: TTFF — wait up to 20s for initial fix
       b) Phase 2: Stabilisation — wait 10s for accuracy
    5. Build TLV packet, transmit via LoRa
    6. Listen for commands from hub (10s RX window)
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
    GNSS: disabled via AT command (integrated in GM02SP)
    LoRa SX1262: sleep mode
    BLE SoftDevice: disabled
    GM02SP cellular: PSM + eDRX configured
*/

// ── Core libraries ──
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>        // SX1262 LoRa radio driver
#include <bluefruit.h>       // Adafruit nRF52 BLE library (scanner for home beacon)

// ── BluePaws shared protocol library ──
#include <bp_protocol.h>     // Binary TLV packet format, builder & parser
#include <bp_config.h>       // LoRa params, profiles, AES key, timing constants
#include <bp_crypto.h>       // Legacy AES helper retained for downlink experiments
#include "collar_pins.h"     // GPIO pin assignments for this board

// FreeRTOS (built into Adafruit nRF52 BSP — no separate install needed)
#include <FreeRTOS.h>
#include <task.h>            // xTaskCreate, vTaskDelay
#include <semphr.h>          // xSemaphoreCreateMutex, Take/Give

// ═══════════════════════════════════════════════
// Device Identity — set per collar at provisioning
// Each collar gets a unique ID (e.g. 0x0001, 0x0002).
// Override via build flag: -DMY_DEVICE_ID=0x0002
// ═══════════════════════════════════════════════
#ifndef MY_DEVICE_ID
#define MY_DEVICE_ID  0x0001
#endif

// ═══════════════════════════════════════════════
// Hardware Instances
// ═══════════════════════════════════════════════
// LoRa radio on SPI2 (nRF52840 has multiple SPI peripherals)
SPIClass loraSPI(NRF_SPIM2, PIN_LORA_MISO, PIN_LORA_SCK, PIN_LORA_MOSI);
SX1262   lora = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);

// GPS — provided by GM02SP's integrated GNSS receiver via AT commands.
// No separate GPS module or UART. Coordinates are fetched via AT+SQNGNSS
// over the same Serial1 used for cellular.

// ── Parsed GNSS fix (populated by gnssRequestFix) ──
static double   gnssLat       = 0.0;
static double   gnssLon       = 0.0;
static uint16_t gnssHdop      = 0;     // HDOP × 100
static uint8_t  gnssSats      = 0;
static uint32_t gnssUnixTime  = 0;     // UTC from GNSS response
static uint32_t gnssFixAgeMs  = 0;     // millis() at last valid parse

// ── Operating Profile ──
// Controls sleep interval, TX power, GPS mode, cellular ratio.
// Changed via PKT_CMD_MODE commands from the hub.
static volatile bp_profile_t currentProfile = PROFILE_NORMAL;          // Current profile enum
static const bp_profile_config_t *currentConfig = bp_profile_config(PROFILE_NORMAL); // Pointer to profile params

// ── Counters ──
static uint32_t messageSeq     = 0;    // Incrementing sequence number for outgoing packets
static uint32_t cycleCount     = 0;    // Total wake/transmit cycles since boot
static uint8_t  homeCycleCount = 0;    // Consecutive cycles where BLE home beacon was detected

// ── GPS State ──
static volatile bool gpsAwake     = false;   // true = GPS module is powered on
static volatile bool gpsWarmStart = false;   // true = we had a fix before (ephemeris cached)
static volatile bool gpsFix       = false;   // true = valid fix obtained this cycle

// ── BLE State ──
static volatile bool bleHomeFound = false;   // Set by BLE scan callback when home beacon found
static bool bleAdvertising = false;          // true = BLE find beacon is advertising

// ── Error State ──
// Tracks the most recent subsystem fault. Auto-clears on success.
static volatile bp_error_t lastError = ERROR_NONE;

// ── Command Deduplication ──
// The hub retries commands up to 3 times if no ACK received.
// We store the last processed command sequence number and ignore
// any duplicates, so the collar doesn't execute the same command twice.
static uint32_t lastProcessedCmdSeq = 0;

// ── Lost Mode Tracking ──
static volatile bool     inLostMode      = false;  // true = currently in emergency lost mode
static volatile uint32_t lostModeStartMs = 0;      // millis() when lost mode started

// ── Legacy AES-128 key ──
// TLV v1.1 uplink packets are sent as raw authenticated TLV over private LoRa.
// Keep this only until the downlink command protocol is revised.
static const uint8_t aesKey[16] = LORA_AES_KEY;

// ── Cellular (Sequans Monarch 2 GM02SP — LTE-M/NB-IoT + GNSS) ──
static volatile bool cellularPending = false;          // true = cellular TX requested
static uint8_t lastTxPacket[BP_MAX_PACKET_SIZE];       // Copy of last LoRa packet (for cellular re-send)
static uint8_t lastTxPacketLen = 0;                    // Length of lastTxPacket
static bool cellularInitialised = false;               // true = PSM/eDRX already configured

// ═══════════════════════════════════════════════
// FreeRTOS Task Handles & Config
// ═══════════════════════════════════════════════
static TaskHandle_t cycleTaskHandle  = NULL;  // Main cycle orchestrator
static TaskHandle_t cellTaskHandle   = NULL;  // Cellular transmission (notification-based)

#define STACK_CYCLE  4096   // Main cycle — needs room for packet building + LoRa TX
#define STACK_CELL   3072   // Cellular — AT command strings

#define PRIO_CYCLE   3      // Highest — orchestrates the whole wake/sleep cycle
#define PRIO_CELL    1      // Lowest — cellular runs async, not time-critical

// ═══════════════════════════════════════════════
// Forward Declarations
// ═══════════════════════════════════════════════

// FreeRTOS task entry points
static void cycleTask(void *param);        // Main cycle: wake → sense → TX → sleep
static void cellularTask(void *param);     // Notification-driven: wakes GM02SP → POSTs TLV

// Cycle phases (called in sequence by cycleTask)
static bool     bleScanForHome();          // BLE scan for hub's home beacon
static bool     gnssAcquireFix();          // Request GNSS fix via AT+SQNGNSS
static void     listenForCommands();       // Open 10s LoRa RX window for hub commands
static void     enterDeepSleep();          // Power down, sleep until next cycle
static void     runLostMode();             // Emergency continuous operation loop

// Peripheral power management
static void     peripheralsWake();         // Wake LoRa from sleep mode
static void     peripheralsSleep();        // Sleep GNSS, LoRa, stop BLE

// Packet builders (construct TLV packets and transmit)
static void     sendTelemetry();           // Build + send PKT_TELEMETRY
static void     sendModeAck(uint32_t cmdMsgSeq);        // ACK a mode change command
static void     sendStatusResponse(uint32_t cmdMsgSeq); // Respond to status query
static void     sendLostModeAlert();       // Alert: lost mode 2hr timeout expired
static void     sendWakeCheckin();         // Home wake check-in (no GNSS, no routine LTE)
static void     transmitPacket(uint8_t *buf, uint8_t len, bool suppressLed = false);  // Raw TLV LoRa TX

// Command handling
static void     handleReceivedCommand(const uint8_t *buf, uint8_t len);
static void     applyProfile(bp_profile_t profile);  // Switch operating profile
static void     sendFindAck(uint32_t cmdMsgSeq);     // ACK a find command

// BLE Active Find beacon (collar advertises when in PROFILE_ACTIVE)
static void     bleFindBeaconStart();
static void     bleFindBeaconStop();

// GPS helpers
static void     gnssEnable();              // Start GNSS receiver via AT command
static void     gnssDisable();             // Stop GNSS receiver via AT command
static uint32_t gnssGetUnixTime();         // Return last parsed GNSS Unix timestamp

// LED helpers
static void     ledFlicker(uint8_t count, uint16_t onMs, uint16_t offMs);  // Blink N times
static void     ledBeacon();               // Single flash (used in lost mode continuous blink)

// Buzzer (passive piezo — frequency generated via PWM)
static void     buzzerInit();
static void     buzzerPlayPattern(bp_buzzer_pattern_t pattern);  // Play chirp/trill/siren/melody
static void     buzzerTone(uint16_t freqHz, uint16_t durationMs);
static void     buzzerOff();

// Cellular (Sequans Monarch 2 GM02SP via AT commands)
static void     cellularSendTlv(const uint8_t *pkt, uint8_t len);  // Full send sequence
static void     cellularConfigurePSM();    // Configure PSM + eDRX (first use only)
static bool     cellularSendAT(const char *cmd, const char *expect, uint16_t timeoutMs);

// ═══════════════════════════════════════════════
// BLE Scan Callback
//
// Called by the nRF52 BLE stack for every advertisement packet seen.
// We check if the advertised name matches our hub's beacon name.
// If found, we set bleHomeFound=true and stop scanning immediately
// (no need to keep scanning once we know the pet is home).
// ═══════════════════════════════════════════════
static void bleScanCallback(ble_gap_evt_adv_report_t *report) {
    uint8_t buf[32];
    // Extract the "Complete Local Name" from the advertisement data
    uint8_t len = Bluefruit.Scanner.parseReportByType(
        report, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, buf, sizeof(buf));

    if (len > 0) {
        buf[len] = '\0';  // Null-terminate the name string
        if (strcmp((const char *)buf, BLE_HOME_BEACON_NAME) == 0) {
            // Found the hub's beacon! Pet is home.
            bleHomeFound = true;
            Bluefruit.Scanner.stop();  // Stop scanning — we found what we need
            Serial.println("[BLE] Home beacon found!");
        }
    }

    // Resume scanning for more advertisements (if we haven't found the beacon yet)
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

    // ── LED (visual feedback for TX, errors, and lost mode beacon) ──
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    ledFlicker(3, 30, 30);  // 3 quick flashes = "I'm alive" boot indicator

    // ── Button (can be used for manual mode toggle — TODO) ──
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // ── Buzzer (passive piezo — needs PWM to generate tones) ──
    buzzerInit();

    // ── GNSS Init ──
    // GNSS is provided by the Sequans GM02SP's integrated receiver.
    // No separate module or UART — GPS data is fetched via AT+SQNGNSS
    // over the same Serial1 used for cellular AT commands.
    // GNSS starts disabled; enabled on-demand when a fix is needed.
    gpsAwake = false;
    Serial.println("[GNSS] Integrated (GM02SP) — disabled at boot");

    // ── LoRa Radio Init ──
    // All LoRa parameters must match the hub exactly, or packets won't be received.
    loraSPI.begin();
    Serial.println("[LORA] Initialising SX1262...");

    int state = lora.begin(LORA_FREQUENCY);  // Locked at 869.5 MHz
    if (state != RADIOLIB_ERR_NONE) {
        // Fatal error — can't operate without radio. Flash LED rapidly and halt.
        Serial.printf("[LORA] FATAL: init failed (%d)\n", state);
        ledFlicker(12, 80, 80);
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // Configure radio parameters (must match hub's settings in bp_config.h)
    lora.setSpreadingFactor(LORA_SPREADING);     // SF10 — good range
    lora.setBandwidth(LORA_BANDWIDTH);           // 125 kHz
    lora.setCodingRate(LORA_CODING_RATE);        // Locked at 4/6
    lora.setPreambleLength(LORA_PREAMBLE_LEN);   // Locked at 8 symbols
    lora.setSyncWord(LORA_SYNC_WORD);            // Private network sync word
    lora.setCRC(LORA_CRC_ENABLED);               // Hardware CRC
    lora.setOutputPower(currentConfig->tx_power_dBm);  // TX power from current profile
    lora.sleep();  // Start in sleep mode — only wake when we need to TX/RX

    Serial.printf("[LORA] Ready: %.1fMHz SF%d BW%.0fkHz %ddBm\n",
                  LORA_FREQUENCY, LORA_SPREADING, LORA_BANDWIDTH,
                  currentConfig->tx_power_dBm);
    Serial.printf("[LORA] AES-128: %s\n",
                  bp_aes_key_is_zero(aesKey) ? "OFF (key all zeros)" : "ENABLED");

    // ── BLE Init ──
    // We use BLE in "central" mode (scanner only) — NOT advertising.
    // The collar scans for the hub's BLE beacon to detect if pet is home.
    Bluefruit.begin(1, 1);  // 1 peripheral (find beacon) + 1 central (home scanner)
    Bluefruit.setName("BP_COLLAR");
    Bluefruit.Scanner.setRxCallback(bleScanCallback);  // Called for each advertisement seen
    Bluefruit.Scanner.restartOnDisconnect(false);       // Don't auto-restart after scan stops
    Bluefruit.Scanner.setInterval(160, 80);  // Scan interval 100ms, window 50ms (in 0.625ms units)
    Bluefruit.Scanner.useActiveScan(true);   // Active scan = request scan response for more data
    Serial.printf("[BLE] Ready, beacon: \"%s\"\n", BLE_HOME_BEACON_NAME);

    // ── Cellular Init (Sequans Monarch 2 GM02SP — UART only, modem off until needed) ──
    Serial1.begin(CELLULAR_BAUD_RATE);  // UART to GM02SP modem
    pinMode(PIN_CELL_PWR, OUTPUT);      // Power key — pulse to toggle modem on/off
    pinMode(PIN_CELL_RST, OUTPUT);      // Reset pin — LOW to reset
    digitalWrite(PIN_CELL_PWR, LOW);    // Keep modem off until we need it
    digitalWrite(PIN_CELL_RST, HIGH);   // Not in reset
    Serial.println("[CELL] GM02SP UART ready (modem off)");

    // ── Create FreeRTOS Tasks ──
    // nRF52840 is single-core, so tasks are time-sliced by priority.
    xTaskCreate(cycleTask,     "cycle", STACK_CYCLE, NULL, PRIO_CYCLE, &cycleTaskHandle);
    xTaskCreate(cellularTask,  "cell",  STACK_CELL,  NULL, PRIO_CELL,  &cellTaskHandle);

    Serial.println("[INIT] Tasks created. Entering first cycle.");
    Serial.println("──────────────────────────────────");
}

// Arduino loop() — not used. All work happens in FreeRTOS tasks.
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

            // Home wake check-in: BLE detection is the cause, but the packet
            // builder explicitly sets WAKE_CHECKIN and HOME_BEACON_SEEN.
            // This path skips GNSS and routine LTE, then listens for commands.
            uint8_t checkinRatio = currentConfig->wake_checkin_ratio;
            if (checkinRatio > 0 && (homeCycleCount % checkinRatio == 0)) {
                Serial.printf("[CYCLE] Home (x%d). WAKE_CHECKIN (ratio 1:%d).\n",
                              homeCycleCount, checkinRatio);
                sendWakeCheckin();
                listenForCommands();
            }

            peripheralsSleep();
            enterDeepSleep();
            continue;
        }

        homeCycleCount = 0;

        // ── Phase 2: GNSS acquisition via AT command ──
        bool haveFix = gnssAcquireFix();

        // ── Phase 3: Build TLV and transmit via LoRa ──
        sendTelemetry();

        // ── Phase 4: Listen for commands from hub ──
        listenForCommands();

        // ── Phase 5: Cellular (every Nth cycle per profile) ──
        // cellular_ratio defines how often we also send via NB-IoT:
        //   ratio=6  → every 6th cycle (normal)
        //   ratio=3  → every 3rd cycle (lost mode — more frequent)
        //   ratio=0  → disabled
        uint8_t cellRatio = currentConfig->cellular_ratio;
        if (cellRatio > 0 && (cycleCount % cellRatio == 0)) {
            Serial.printf("[CELL] Triggering (cycle %lu, ratio 1:%d)\n",
                          cycleCount, cellRatio);
            cellularPending = true;
            xTaskNotifyGive(cellTaskHandle);  // Wake the cellularTask
            // Cellular runs asynchronously — don't block the main cycle
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
    gnssEnable();  // GNSS stays on for entire lost mode

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
    // GNSS is enabled only if needed (not home) — managed in gnssAcquireFix()
    // Cellular modem stays off until triggered
}

// Power down all peripherals for deep sleep
static void peripheralsSleep() {
    // GNSS receiver disable
    gnssDisable();

    // LoRa into sleep mode (sub-uA)
    lora.sleep();

    // BLE scanner stop
    Bluefruit.Scanner.stop();

    // Cellular modem should already be in PSM after any transmission
    // No action needed here — GM02SP PSM handles its own sleep
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
// GNSS Acquisition via AT Command
//
// The Sequans Monarch 2 GM02SP has an integrated GNSS receiver.
// Instead of a separate GPS module with NMEA over UART, we issue
// AT commands to start the receiver and poll for a fix.
//
// Flow:
//   1. Enable GNSS receiver (AT+SQNGNSS=1,...)
//   2. Poll for fix (AT+SQNGNSS?) up to timeout
//   3. Parse response for lat/lon/time/sats/hdop
//   4. Disable GNSS when done (unless in continuous mode)
//
// Returns true if usable fix obtained.
// ═══════════════════════════════════════════════
static bool gnssAcquireFix() {
    gnssEnable();
    gpsFix = false;

    uint32_t ttffTimeoutS = gpsWarmStart ? GPS_TTFF_WARM_TIMEOUT_S : GPS_TTFF_COLD_TIMEOUT_S;
    Serial.printf("[GNSS] Acquiring fix (%s, %lus timeout)\n",
                  gpsWarmStart ? "warm" : "cold", ttffTimeoutS);

    uint32_t startMs = millis();
    uint32_t ttffTimeoutMs = ttffTimeoutS * 1000UL;

    while (millis() - startMs < ttffTimeoutMs) {
        // TODO: Replace with actual Sequans GNSS AT command sequence.
        // Expected flow:
        //   Send: AT+SQNGNSS?
        //   Response: +SQNGNSS: <fix_type>,<utc>,<lat>,<N/S>,<lon>,<E/W>,<hdop>,<alt>,<fix>,<sats>
        //   Parse the comma-separated fields.
        //
        // Placeholder: call cellularSendAT() to query GNSS status
        // and parse the response into gnssLat, gnssLon, etc.

        if (cellularSendAT("AT+SQNGNSS?", "+SQNGNSS:", 3000)) {
            // TODO: Parse the AT response string into:
            //   gnssLat, gnssLon, gnssHdop, gnssSats, gnssUnixTime
            // For now, assume a successful response means we have a fix.
            // The actual parsing will depend on the GM02SP response format.

            // Placeholder parse — replace with real NMEA/Sequans field parsing
            // gnssLat = parsedLat;
            // gnssLon = parsedLon;
            // gnssHdop = parsedHdop;
            // gnssSats = parsedSats;
            // gnssUnixTime = parsedUtc;

            gnssFixAgeMs = millis();
            gpsFix = true;
            gpsWarmStart = true;
            if (lastError == ERROR_GPS) lastError = ERROR_NONE;  // Clear GPS error on success
            Serial.printf("[GNSS] Fix acquired after %lums\n", millis() - startMs);
            Serial.printf("[GNSS] Position: %.6f, %.6f (sats: %d)\n",
                          gnssLat, gnssLon, gnssSats);
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));  // Poll every 2s
    }

    Serial.println("[GNSS] Timeout — no fix");
    lastError = ERROR_GPS;
    return false;
}

// ═══════════════════════════════════════════════
// Build and Send Telemetry (PKT_TELEMETRY)
// ═══════════════════════════════════════════════
static void sendTelemetry() {
    messageSeq++;

    bp_status_t status;
    uint8_t flags = 0;

    // Check if GNSS fix is valid and recent
    uint32_t fixAgeS = (millis() - gnssFixAgeMs) / 1000;
    bool locValid = gpsFix && (fixAgeS < GPS_STALE_THRESHOLD_S);

    if (locValid) {
        status = STATUS_OUT_AND_ABOUT;  // Pet is outside with valid position
        flags |= FLAG_GNSS_VALID | FLAG_FIX_3D;
    } else {
        status = STATUS_INVALID_GPS;    // No usable GPS fix this cycle
    }

    if (gpsWarmStart && !locValid) flags |= FLAG_STALE_FIX;
    if (lastError != ERROR_NONE) flags |= FLAG_ERROR_PRESENT;

    uint32_t unixTime = gnssGetUnixTime();  // Get timestamp from GNSS

    // ── Build the packet ──
    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, (uint16_t)(messageSeq & 0xFFFF), unixTime,
             status, currentProfile, flags, TX_TELEMETRY);

    if (flags & FLAG_HAS_GPS) {
        int32_t lat_e7 = (int32_t)(gnssLat * 1e7);
        int32_t lon_e7 = (int32_t)(gnssLon * 1e7);
        pkt_set_gps(buf, lat_e7, lon_e7);

        // GPS accuracy derived from HDOP (Horizontal Dilution of Precision)
        // Rough conversion: accuracy_meters ≈ HDOP × 5
        uint16_t acc_m = gnssHdop > 0 ? (uint16_t)((gnssHdop / 100.0) * 5) : 0;
        uint16_t batt_mV = 3700;  // TODO: Read actual battery voltage via ADC
        pkt_set_quality(buf, batt_mV, acc_m, (uint16_t)fixAgeS);
        pkt_set_sat_count(buf, gnssSats);
    } else {
        uint16_t batt_mV = 3700;  // TODO: Read actual battery voltage via ADC
        pkt_set_quality(buf, batt_mV, 0, 65535);
        pkt_set_sat_count(buf, 255);
    }

    // Keep embedded optional TLVs sparse until the production telemetry set is
    // finalized. Header fields carry status/profile/location/battery.
    pkt_add_tlv_u32(buf, TLV_UPTIME_S, millis() / 1000);
    if (lastError != ERROR_NONE) {
        pkt_add_tlv_u8(buf, TLV_RESET_REASON, lastError);
    }

    // Finalize: append v1.1 8-byte auth tag placeholder, return total length.
    uint8_t pktLen = pkt_finalize(buf);

    Serial.printf("[TX] TELEMETRY seq=%lu status=%s size=%dB\n",
                  messageSeq, bp_status_display(status), pktLen);
    pkt_print_hex(buf, pktLen);

    transmitPacket(buf, pktLen);

    // Save a copy for the cellular task to re-send via NB-IoT
    memcpy(lastTxPacket, buf, pktLen);
    lastTxPacketLen = pktLen;
}

// ═══════════════════════════════════════════════
// Send Home Wake Check-In (no GNSS, no routine LTE)
//
// Lightweight presence packet sent when the collar detects the BLE home beacon.
// The packet explicitly sets tx_reason=WAKE_CHECKIN and HOME_BEACON_SEEN; the
// packet builder does not infer those fields from each other.
// ═══════════════════════════════════════════════
static void sendWakeCheckin() {
    messageSeq++;

    uint8_t buf[BP_MAX_PACKET_SIZE];
    uint8_t flags = FLAG_HOME_BEACON_SEEN;
    if (lastError != ERROR_NONE) flags |= FLAG_ERROR_PRESENT;

    pkt_init(buf, MY_DEVICE_ID, (uint16_t)(messageSeq & 0xFFFF), 0,
             STATUS_HOME, currentProfile, flags, TX_WAKE_CHECKIN);

    uint16_t batt_mV = 3700;  // TODO: Read actual battery voltage via ADC
    pkt_set_quality(buf, batt_mV, 0, 65535);
    pkt_set_sat_count(buf, 255);

    pkt_add_tlv_u32(buf, TLV_UPTIME_S, millis() / 1000);
    if (lastError != ERROR_NONE) {
        pkt_add_tlv_u8(buf, TLV_RESET_REASON, lastError);
    }

    uint8_t pktLen = pkt_finalize(buf);

    Serial.printf("[TX] WAKE_CHECKIN seq=%lu homeCycles=%d size=%dB\n",
                  messageSeq, homeCycleCount, pktLen);
    pkt_print_hex(buf, pktLen);

    transmitPacket(buf, pktLen, /*suppressLed=*/true);

    // Save copy for diagnostics; routine cellular is intentionally skipped.
    memcpy(lastTxPacket, buf, pktLen);
    lastTxPacketLen = pktLen;
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
//
// Wakes the radio, transmits the raw TLV v1.1 packet, then provides visual
// feedback via LED flashes.
// LED pattern tells you the result: normal flashes = OK,
// 2 slow flashes = timeout, 6 rapid flashes = error.
// ═══════════════════════════════════════════════
static void transmitPacket(uint8_t *buf, uint8_t len, bool suppressLed) {
    lora.standby();                      // Wake radio from sleep → standby mode
    int state = lora.transmit(buf, len); // Blocking TX — returns when done or timeout

    if (state == RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] TX OK (%d bytes)\n", len);
        if (lastError == ERROR_RF) lastError = ERROR_NONE;  // Clear RF error on success
        if (!suppressLed) {
            ledFlicker(currentConfig->led_flashes, 50, 50);  // Success: profile-defined flash count
        }
    } else if (state == RADIOLIB_ERR_TX_TIMEOUT) {
        Serial.println("[LORA] TX timeout");
        lastError = ERROR_RF;
        ledFlicker(2, 200, 200);  // Slow double-flash = timeout (always show errors)
    } else {
        Serial.printf("[LORA] TX failed: %d\n", state);
        lastError = ERROR_RF;
        ledFlicker(6, 80, 80);    // Rapid 6-flash = error (always show errors)
    }
}

// ═══════════════════════════════════════════════
// Listen for Commands (10s RX window)
//
// After transmitting telemetry, the collar opens a short receive
// window so the hub can send commands (mode change, find, etc.).
// This is like a "half-duplex" protocol — the collar is mostly
// sleeping/transmitting, but briefly listens after each TX.
// The hub times its command TX to coincide with this window.
// ═══════════════════════════════════════════════
static void listenForCommands() {
    Serial.printf("[RX] Listening %dms...\n", CMD_LISTEN_WINDOW_MS);

    // Put radio into RX mode
    int rxState = lora.startReceive();
    if (rxState != RADIOLIB_ERR_NONE) {
        Serial.printf("[RX] startReceive failed: %d\n", rxState);
        return;
    }

    // Poll for incoming packet within the RX window
    uint32_t listenStart = millis();
    while (millis() - listenStart < CMD_LISTEN_WINDOW_MS) {
        // Check the radio's IRQ status register for RX_DONE flag
        uint16_t irq = lora.getIrqStatus();
        if (irq & RADIOLIB_SX126X_IRQ_RX_DONE) {
            uint8_t rxBuf[BP_MAX_PACKET_SIZE];
            int state = lora.readData(rxBuf, sizeof(rxBuf));
            if (state == RADIOLIB_ERR_NONE) {
                uint8_t rxLen = lora.getPacketLength();
                Serial.printf("[RX] Received %d bytes\n", rxLen);

                pkt_print_hex(rxBuf, rxLen);  // Debug: hex dump to serial

                // Basic validation before processing
                if (rxLen >= BP_MIN_PACKET_SIZE && rxBuf[0] == BP_PROTOCOL_VERSION) {
                    handleReceivedCommand(rxBuf, rxLen);
                }
            }
            break;  // Only process one command per RX window
        }
        vTaskDelay(pdMS_TO_TICKS(10));  // Poll every 10ms
    }

    lora.standby();  // Return radio to standby (will go to sleep later)
}

// ═══════════════════════════════════════════════
// Command Handler
//
// Processes incoming commands from the hub. Supported commands:
//   PKT_CMD_MODE   — Change operating profile (normal/powersave/active/lost)
//   PKT_CMD_STATUS — Request a status response packet
//   PKT_CMD_FIND   — Flash LED + play buzzer pattern ("Find My Pet")
// ═══════════════════════════════════════════════
static void handleReceivedCommand(const uint8_t *buf, uint8_t len) {
    // Validate TLV v1.1 structure. Authentication of cloud-bound uplinks is
    // performed by Supabase; local command authentication will be revised with
    // the downlink command protocol.
    if (!pkt_validate_crc(buf, len)) {
        Serial.println("[RX] TLV structure failed — dropping");
        return;
    }

    // Check if this command is addressed to us (or is a broadcast)
    uint16_t targetId = pkt_device_id(buf);
    if (targetId != MY_DEVICE_ID && targetId != DEVICE_ID_BROADCAST) {
        Serial.printf("[RX] Not for us (0x%04X)\n", targetId);
        return;
    }

    uint16_t pktType = pkt_pkt_type(buf);
    uint32_t cmdSeq  = pkt_msg_seq(buf);

    // ── Deduplication ──
    // The hub retries commands up to 3 times if it doesn't get an ACK.
    // We track the last processed sequence number to avoid executing
    // the same command multiple times (e.g. switching profile twice).
    if (cmdSeq != 0 && cmdSeq == lastProcessedCmdSeq) {
        Serial.printf("[RX] Duplicate cmd seq %lu — ignoring\n", cmdSeq);
        return;
    }
    lastProcessedCmdSeq = cmdSeq;

    switch (pktType) {
    case PKT_CMD_MODE: {
        // Hub is telling us to switch to a different operating profile.
        // Extract the target profile from TLV_PROFILE in the packet payload.
        uint8_t newProfile;
        if (pkt_tlv_get_u8(buf, TLV_PROFILE, &newProfile)) {
            Serial.printf("[RX] CMD_MODE → %s (seq %lu)\n",
                          bp_profile_name((bp_profile_t)newProfile), cmdSeq);
            applyProfile((bp_profile_t)newProfile);  // Apply new settings (TX power, sleep time, etc.)
            sendModeAck(cmdSeq);                     // ACK back to hub with new config
        } else {
            Serial.println("[RX] CMD_MODE missing TLV_PROFILE");
        }
        break;
    }
    case PKT_CMD_STATUS:
        // Hub wants a status report — send back current config details
        Serial.printf("[RX] CMD_STATUS (seq %lu)\n", cmdSeq);
        sendStatusResponse(cmdSeq);
        break;
    case PKT_CMD_FIND: {
        // "Find My Pet" — flash the LED and play a buzzer sound
        Serial.printf("[RX] CMD_FIND (seq %lu)\n", cmdSeq);

        // Extract LED flash count from TLV (default: 5)
        uint8_t flashCount = 5;
        pkt_tlv_get_u8(buf, TLV_LED_FLASH, &flashCount);
        if (flashCount > 0) {
            ledFlicker(flashCount, 80, 80);  // Visible blink to help locate the pet
        }

        // Extract buzzer pattern from TLV (default: chirp)
        uint8_t pattern = BUZZER_CHIRP;
        pkt_tlv_get_u8(buf, TLV_BUZZER_PATTERN, &pattern);
        if (pattern != BUZZER_OFF) {
            buzzerPlayPattern((bp_buzzer_pattern_t)pattern);  // Audible alert
        }

        sendFindAck(cmdSeq);  // ACK back to hub
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
// ═══════════════════════════════════════════════
// BLE Active Find Beacon
//
// When the collar enters PROFILE_ACTIVE (Active Find mode),
// it starts advertising a BLE beacon named "BP_FIND_XXXX"
// (where XXXX is the device ID in hex). The hub in Portable
// Mode scans for these beacons and shows RSSI-based proximity.
// nRF52840 supports simultaneous central + peripheral roles,
// so home beacon scanning continues to work in parallel.
// ═══════════════════════════════════════════════
static void bleFindBeaconStart() {
    if (bleAdvertising) return;

    char name[16];
    snprintf(name, sizeof(name), "%s%04X", BLE_FIND_BEACON_PREFIX, MY_DEVICE_ID);

    Bluefruit.Advertising.clearData();
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.setName(name);
    Bluefruit.Advertising.addName();
    Bluefruit.Advertising.setInterval(32, 48);  // 20-30ms fast advertising
    Bluefruit.Advertising.start(0);             // Advertise indefinitely
    bleAdvertising = true;
    Serial.printf("[BLE] Find beacon started: %s\n", name);
}

static void bleFindBeaconStop() {
    if (!bleAdvertising) return;
    Bluefruit.Advertising.stop();
    Bluefruit.setName("BP_COLLAR");
    bleAdvertising = false;
    Serial.println("[BLE] Find beacon stopped");
}

static void applyProfile(bp_profile_t profile) {
    Serial.printf("[MODE] %s → %s\n",
                  bp_profile_name(currentProfile), bp_profile_name(profile));

    currentProfile = profile;
    currentConfig  = bp_profile_config(profile);

    lora.setOutputPower(currentConfig->tx_power_dBm);

    // BLE find beacon: advertise when in Active Find, stop otherwise
    if (profile == PROFILE_ACTIVE) {
        bleFindBeaconStart();
    } else {
        bleFindBeaconStop();
    }

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
// Cellular Task
// Blocks on notification. Wakes GM02SP, configures
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
// Wakes GM02SP, configures PSM/eDRX on first use,
// POSTs the same TLV payload inside the HTTPS JSON wrapper, then returns
// (modem enters PSM).
// ═══════════════════════════════════════════════
static void cellularSendTlv(const uint8_t *pkt, uint8_t len) {
    // ── Power on GM02SP ──
    Serial.println("[CELL] Powering on GM02SP...");
    digitalWrite(PIN_CELL_PWR, HIGH);
    vTaskDelay(pdMS_TO_TICKS(600));
    digitalWrite(PIN_CELL_PWR, LOW);
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Wait for modem ready
    if (!cellularSendAT("AT", "OK", 5000)) {
        Serial.println("[CELL] Modem not responding — aborting");
        lastError = ERROR_CELLULAR;
        return;
    }
    if (lastError == ERROR_CELLULAR) lastError = ERROR_NONE;  // Modem responded OK

    // ── First-time PSM/eDRX configuration ──
    if (!cellularInitialised) {
        cellularConfigurePSM();
        cellularInitialised = true;
    }

    // ── POST the TLV payload ──
    // The collar-generated TLV must stay byte-for-byte identical to any copy
    // relayed by the hub. LTE adds transport metadata only in the HTTPS wrapper.
    // For now, this is a placeholder AT sequence.
    // The server endpoint and auth will be configured at provisioning.

    // TODO: Full Sequans HTTP POST sequence:
    //   AT+CEREG?                           → check registration
    //   AT+SQNHTTPCFG=0,"<server_host>",443,1  → configure HTTP client with TLS
    //   AT+SQNHTTPQRY=0,1,"/<endpoint>"     → POST request
    //   AT+SQNHTTPSND=0,1,<len>             → send JSON wrapper
    //   {"format":"tlv","ingest_path":"cellular_direct",...,"payload_b64":"..."}
    //   → +SQNHTTPRING: 0,200,...

    Serial.printf("[CELL] TODO: POST %d bytes TLV\n", len);

    // After POST, modem will enter PSM automatically
    // (configured via AT+CPSMS and AT+CEDRXS)
    Serial.println("[CELL] Modem entering PSM");
}

// ═══════════════════════════════════════════════
// Configure GM02SP PSM and eDRX
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
// GNSS Enable / Disable (via GM02SP AT commands)
//
// The Sequans Monarch 2 GM02SP has an integrated GNSS receiver
// controlled entirely through AT commands — no separate power pin.
// ═══════════════════════════════════════════════
static void gnssEnable() {
    if (!gpsAwake) {
        // TODO: Replace with actual Sequans GNSS enable command.
        // Expected AT command to start GNSS receiver:
        //   AT+SQNGNSS=1,<mode>,<constellation>,<nmea_mask>
        // Example:
        //   AT+SQNGNSS=1,1,3,0   → start single-shot, GPS+GLONASS
        //   AT+SQNGNSS=1,2,3,0   → start continuous, GPS+GLONASS
        cellularSendAT("AT+SQNGNSS=1", "OK", 3000);  // TODO: confirm exact syntax
        gpsAwake = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        Serial.println("[GNSS] Receiver enabled");
    }
}

static void gnssDisable() {
    if (gpsAwake && !currentConfig->gps_continuous) {
        // TODO: Replace with actual Sequans GNSS disable command.
        // Expected: AT+SQNGNSS=0
        cellularSendAT("AT+SQNGNSS=0", "OK", 2000);  // TODO: confirm exact syntax
        gpsAwake = false;
        Serial.println("[GNSS] Receiver disabled");
    }
}

// Return last parsed GNSS Unix timestamp.
// Returns 0 if no valid time has been obtained from GNSS.
// The collar doesn't have an RTC, so GNSS is our only time source.
static uint32_t gnssGetUnixTime() {
    // gnssUnixTime is populated by gnssAcquireFix() when parsing the
    // AT+SQNGNSS? response. Returns 0 if no fix has been obtained yet.
    return gnssUnixTime;
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
