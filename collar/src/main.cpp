/*
  ┌──────────────────────────────────────────────────────────┐
  │  BLUEPAWS V4 — COLLAR FIRMWARE                           │
  │  RAK4630 (nRF52840 + SX1262) + Sequans GM02SP           │
  │  FreeRTOS task-based architecture                        │
  └──────────────────────────────────────────────────────────┘

  NORMAL / POWERSAVE / ACTIVE cycle:
    1. Wake from deep sleep (only RTC running)
    2. BLE scan for home beacon (10s)
    3. If home → send a profile-cadenced WAKE_CHECKIN raw TLV over LoRa,
       open the command RX window, then usually go back to sleep.
       GNSS sanity refreshes and LTE heartbeats run only on their profile cadence.
    4. If NOT home → GPS two-phase acquisition:
       a) Phase 1: TTFF — wait up to 20s for initial fix
       b) Phase 2: Stabilisation — wait 10s for accuracy
    5. Build TLV packet, transmit via LoRa
    6. Listen for commands from hub (10s RX window)
    7. Cellular direct-to-cloud is secondary/fallback and profile-cadenced
    8. Power down all peripherals, deep sleep until next cycle

  LOST MODE (emergency):
    - No sleep — stays awake for up to 2 hours
    - GPS continuous (kept on between transmissions)
    - LoRa at full power (22 dBm), TX every 30s
    - Cellular fallback/heartbeat is frequent compared with normal
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
#include <math.h>
#include <RadioLib.h>        // SX1262 LoRa radio driver
#include <bluefruit.h>       // Adafruit nRF52 BLE library (scanner for home beacon)
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

// ── BluePaws shared protocol library ──
#include <bp_protocol.h>     // Binary TLV packet format, builder & parser
#include <bp_config.h>       // LoRa params, profiles, AES key, timing constants
#include <bp_crypto.h>       // Legacy AES helper retained for downlink experiments
#include <bp_hmac_sha256.h>  // Tiny embedded HMAC-SHA256 helper for TLV auth
#include "collar_hardware_profile.h"  // Testbed/production guardrails
#include "collar_pins.h"     // GPIO pin assignments for this board
#if __has_include("collar_secrets.h")
#include "collar_secrets.h"  // Local-only per-collar HMAC key; ignored by Git
#endif
#include "collar_routing.h"  // Explicitly provisioned affiliated Home Hub

#ifndef BLUEPAWS_BUILD_UNIX_TIME
#error "Build through PlatformIO with tools/firmware_build_time.py enabled"
#endif

// FreeRTOS (built into Adafruit nRF52 BSP — no separate install needed)
#include <FreeRTOS.h>
#include <task.h>            // xTaskCreate, vTaskDelay
#include <semphr.h>          // xSemaphoreCreateMutex, Take/Give

using namespace Adafruit_LittleFS_Namespace;

// ═══════════════════════════════════════════════
// Device Identity — set per collar at provisioning
// Each collar gets a unique ID (e.g. 0x0001, 0x0002).
// Override via build flag: -DMY_DEVICE_ID=0x0002
// ═══════════════════════════════════════════════
#ifndef MY_DEVICE_ID
#define MY_DEVICE_ID  0x0001
#endif

#ifndef BLUEPAWS_FW_MAJOR
#define BLUEPAWS_FW_MAJOR 1
#endif

#ifndef BLUEPAWS_FW_MINOR
#define BLUEPAWS_FW_MINOR 1
#endif

#define BLUEPAWS_BOOT_GNSS_TIMEOUT_S       60UL
#define BLUEPAWS_STATE_SEQ_CHECKPOINT_EVERY 16UL
#define BLUEPAWS_BUTTON_DEBOUNCE_MS        40UL
#define BLUEPAWS_BUTTON_LONG_PRESS_MS      3000UL
#define BLUEPAWS_COLLAR_STATE_PATH         "/bp_collar_state.bin"
#define BLUEPAWS_COLLAR_STATE_MAGIC        0x42505634UL
#define BLUEPAWS_COLLAR_STATE_VERSION      1U

#if defined(COLLAR_HMAC_KEY_BYTES)
static const uint8_t collarHmacKey[32] = COLLAR_HMAC_KEY_BYTES;
#define BLUEPAWS_COLLAR_HMAC_CONFIGURED 1
#else
#define BLUEPAWS_COLLAR_HMAC_CONFIGURED 0
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
static uint32_t lastLteHeartbeatMs = 0; // Last time a home LTE heartbeat was queued
static volatile bool bootReportPending = true;        // First runtime action is a BOOT report
static volatile bool forceUserReportRequested = false; // Physical button / debug forced TX
static uint8_t bootResetReason = 0;                   // Raw nRF reset-reason low byte

#if defined(BLUEPAWS_TESTBED_BUILD) && BLUEPAWS_TESTBED_BUILD
// USB Serial debug console for RAK4631/WisMesh testbed work only.
// Non-persistent: reset/power-cycle returns to firmware defaults.
static volatile bool     debugCadenceEnabled = false;
static volatile uint16_t debugSleepIntervalS = 60;
static volatile bool     forceImmediateCycle = false;
#endif

// ── GPS State ──
static volatile bool gpsAwake     = false;   // true = GPS module is powered on
static volatile bool gpsWarmStart = false;   // true = we had a fix before (ephemeris cached)
static volatile bool gpsFix       = false;   // true = valid fix obtained this cycle
static SemaphoreHandle_t gpsMutex = NULL;    // Protects GNSS state shared across tasks

// ── BLE State ──
static volatile bool bleHomeFound = false;   // Set by BLE scan callback when home beacon found
static bool bleAdvertising = false;          // true = BLE find beacon is advertising

// ── Error State ──
// Tracks the most recent subsystem fault. Auto-clears on success.
static volatile bp_error_t lastError = BP_ERROR_NONE;

// ── Command Deduplication ──
// The hub retries commands up to 3 times if no ACK received.
// We store the last processed command sequence number and ignore
// any duplicates, so the collar doesn't execute the same command twice.
static uint32_t lastProcessedCmdSeq = 0;
static uint16_t lastCommandSourceId = BP_DEFAULT_HUB_ID;

// ── Lost Mode Tracking ──
static volatile bool     inLostMode      = false;  // true = currently in emergency lost mode
static volatile uint32_t lostModeStartMs = 0;      // millis() when lost mode started

// ── Conservative non-volatile collar state ──
// This is intentionally small and versioned. Write immediately for human config
// changes; checkpoint high-churn counters to reduce internal flash wear.
struct collar_persisted_state_t {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t checksum;
    uint8_t  profile;
    uint8_t  lost_active;
    uint16_t config_revision;
    uint32_t message_seq_checkpoint;
    uint32_t boot_counter;
    uint32_t runtime_counter_checkpoint;
    uint8_t  home_cycle_checkpoint;
    uint8_t  reserved0[3];
    int32_t  last_lat_e7;
    int32_t  last_lon_e7;
    uint32_t last_fix_unix;
    uint16_t last_fix_acc_m;
    uint8_t  last_fix_sats;
    uint8_t  has_last_fix;
    char     apn[32];
};

static collar_persisted_state_t persistedState;
static bool collarStateReady = false;

// ── Legacy AES-128 key ──
// TLV v1.2 uplink packets are sent as raw authenticated TLV over private LoRa.
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
static bool     gnssAcquireFixWithTimeout(uint32_t timeoutS); // Bounded GNSS request
static void     listenForCommands();       // Open 10s LoRa RX window for hub commands
static void     enterDeepSleep();          // Power down, sleep until next cycle
static void     runLostMode();             // Emergency continuous operation loop

// Peripheral power management
static void     peripheralsWake();         // Wake LoRa from sleep mode
static void     peripheralsSleep();        // Sleep GNSS, LoRa, stop BLE

// Packet builders (construct TLV packets and transmit)
static void     sendTelemetry();           // Build + send PKT_TELEMETRY
static void     sendTelemetryWithReason(uint8_t txReason);    // TELEMETRY/INTERRUPT helper
static void     sendBootReport(bool atHome, bool haveFix);    // Cold/reboot boot report
static void     sendModeAck(uint32_t cmdMsgSeq, uint16_t destinationId);        // ACK a mode change command
static void     sendStatusResponse(uint32_t cmdMsgSeq, uint16_t destinationId); // Respond to status query
static void     sendLostModeAlert();       // Alert: lost mode 2hr timeout expired
static void     sendWakeCheckin();         // Home wake check-in (no GNSS, no routine LTE)
static void     transmitPacket(uint8_t *buf, uint8_t len, bool suppressLed = false);  // Raw TLV LoRa TX
static uint8_t  finalizeAuthenticatedPacket(uint8_t *buf); // Append/sign TLV v1.2 HMAC tag

// Command handling
static void     handleReceivedCommand(const uint8_t *buf, uint8_t len);
static void     applyProfile(bp_profile_t profile);  // Switch operating profile
static void     sendFindAck(uint32_t cmdMsgSeq, uint16_t destinationId);     // ACK a find command

// BLE Lost/Find beacon (collar advertises only when in PROFILE_LOST)
static void     bleFindBeaconStart();
static void     bleFindBeaconStop();
static void     buttonPoll();              // Short press = forced TX; long press = Lost Alert

// GPS helpers
static void     gnssEnable();              // Start GNSS receiver via AT command
static void     gnssDisable();             // Stop GNSS receiver via AT command
static uint32_t gnssGetUnixTime();         // Return last parsed GNSS Unix timestamp
static bool     gnssSpoofAcquireFix();     // Testbed-only spoof GNSS fix
static uint32_t compileTimeUnix();         // Approximate UTC from PlatformIO build epoch

// Persistence helpers
static void     collarStateDefaults();
static bool     collarStateLoad();
static void     collarStateSave(const char *reason, bool noisy = true);
static void     collarStateCheckpoint(const char *reason);
static void     collarStateRememberFix();
static uint32_t collarStateChecksum(collar_persisted_state_t state);
static uint8_t  captureResetReason();

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
static uint16_t effectiveSleepIntervalS();

#if defined(BLUEPAWS_TESTBED_BUILD) && BLUEPAWS_TESTBED_BUILD
static void     debugConsolePoll();
static void     debugConsoleHandleLine(String line);
static void     debugConsolePrintHelp();
static void     debugConsolePrintStatus();
#endif

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
    bootResetReason = captureResetReason();

    Serial.println("══════════════════════════════════");
    Serial.println("  Bluepaws V4 — Collar");
    Serial.printf("  Device: %s (0x%04X)\n", bp_device_name(MY_DEVICE_ID), MY_DEVICE_ID);
    Serial.printf("  Affiliated Home Hub: %04X (%u)\n",
                  (unsigned)MY_HOME_HUB_ID, (unsigned)MY_HOME_HUB_ID);
    Serial.printf("  Protocol v%d | Max %dB packet\n", BP_PROTOCOL_VERSION, BP_MAX_PACKET_SIZE);
    Serial.printf("  Hardware profile: %s\n", BLUEPAWS_COLLAR_HARDWARE_PROFILE);
#if BLUEPAWS_TESTBED_BUILD
    Serial.println("  BUILD: TESTBED — diagnostic/spoof features may be enabled");
#else
    Serial.println("  BUILD: PRODUCTION — diagnostic/spoof features disabled");
#endif
#if BLUEPAWS_GNSS_SPOOF_ENABLED
    Serial.println("  GNSS: SPOOF ENABLED — emits synthetic drift for bench testing");
#else
    Serial.println("  GNSS: real GM02SP only");
#endif
    Serial.println("══════════════════════════════════");

    // ── Mutex ──
    gpsMutex = xSemaphoreCreateMutex();

    if (InternalFS.begin()) {
        collarStateReady = true;
        if (collarStateLoad()) {
            currentProfile = (bp_profile_t)persistedState.profile;
            currentConfig = bp_profile_config(currentProfile);
            inLostMode = persistedState.lost_active != 0;
            messageSeq = persistedState.message_seq_checkpoint;
            cycleCount = persistedState.runtime_counter_checkpoint;
            homeCycleCount = persistedState.home_cycle_checkpoint;
            if (persistedState.has_last_fix) {
                gnssLat = persistedState.last_lat_e7 / 1e7;
                gnssLon = persistedState.last_lon_e7 / 1e7;
                gnssUnixTime = persistedState.last_fix_unix;
                gnssSats = persistedState.last_fix_sats;
                gpsWarmStart = true;
            }
            persistedState.boot_counter++;
            Serial.printf("[STATE] Loaded flash state: profile=%s seq=%lu boots=%lu lost=%s\n",
                          bp_profile_name(currentProfile),
                          messageSeq,
                          persistedState.boot_counter,
                          inLostMode ? "yes" : "no");
        } else {
            collarStateDefaults();
            persistedState.boot_counter = 1;
            Serial.println("[STATE] No valid flash state; using factory defaults.");
        }
        collarStateSave("boot", true);
    } else {
        Serial.println("[STATE] InternalFS unavailable; running with volatile state only.");
    }

    // ── LED (visual feedback for TX, errors, and lost mode beacon) ──
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    ledFlicker(3, 30, 30);  // 3 quick flashes = "I'm alive" boot indicator

    // ── Button ──
    // Short press: force a user-requested report. Long press: toggle Lost Alert.
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
    if (inLostMode) {
        bleFindBeaconStart();
        lostModeStartMs = millis();
        Serial.println("[MODE] Persisted Lost Alert restored after reboot.");
    }

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

// Arduino loop() — production work happens in FreeRTOS tasks.
void loop() {
    buttonPoll();
#if defined(BLUEPAWS_TESTBED_BUILD) && BLUEPAWS_TESTBED_BUILD
    debugConsolePoll();
    vTaskDelay(pdMS_TO_TICKS(25));
#else
    vTaskDelay(pdMS_TO_TICKS(25));
#endif
}

// ═══════════════════════════════════════════════════════════
// MAIN CYCLE TASK
// Orchestrates the collar's wake/sense/transmit/sleep loop.
// ═══════════════════════════════════════════════════════════
static void cycleTask(void *param) {
    (void)param;
    vTaskDelay(pdMS_TO_TICKS(500));

    for (;;) {
        if (bootReportPending) {
            bootReportPending = false;
            cycleCount++;

            Serial.printf("\n[BOOT] Boot report cycle | reset=0x%02X | profile=%s\n",
                          bootResetReason, bp_profile_name(currentProfile));
            peripheralsWake();

            bool atHome = bleScanForHome();
            if (atHome) {
                homeCycleCount++;
            } else {
                homeCycleCount = 0;
            }

            Serial.printf("[BOOT] GNSS acquisition ceiling: %lus\n", BLUEPAWS_BOOT_GNSS_TIMEOUT_S);
            bool haveBootFix = gnssAcquireFixWithTimeout(BLUEPAWS_BOOT_GNSS_TIMEOUT_S);
            sendBootReport(atHome, haveBootFix);
            listenForCommands();

            Serial.println("[CELL] Boot safety POST requested for same TLV.");
            cellularPending = true;
            xTaskNotifyGive(cellTaskHandle);

            peripheralsSleep();
            enterDeepSleep();
            continue;
        }

        if (forceUserReportRequested) {
            forceUserReportRequested = false;
            cycleCount++;

            Serial.printf("\n[USER] Forced report cycle | profile=%s\n", bp_profile_name(currentProfile));
            peripheralsWake();

            bool atHome = bleScanForHome();
            if (atHome) {
                homeCycleCount++;
            } else {
                homeCycleCount = 0;
            }

            (void)gnssAcquireFixWithTimeout(BLUEPAWS_BOOT_GNSS_TIMEOUT_S);
            sendTelemetryWithReason(TX_INTERRUPT);
            listenForCommands();

            peripheralsSleep();
            enterDeepSleep();
            continue;
        }

        // ── Lost mode runs its own continuous loop ──
        if (inLostMode) {
            runLostMode();
            continue;  // after lost mode ends, resume normal cycle
        }

        cycleCount++;
        Serial.printf("\n[CYCLE %lu] %s | interval %ds%s\n",
                      cycleCount, bp_profile_name(currentProfile),
                      effectiveSleepIntervalS(),
#if defined(BLUEPAWS_TESTBED_BUILD) && BLUEPAWS_TESTBED_BUILD
                      debugCadenceEnabled ? " | debug cadence" : ""
#else
                      ""
#endif
        );

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

            uint8_t homeGnssRatio = currentConfig->home_gnss_refresh_ratio;
            if (homeGnssRatio > 0 && (homeCycleCount % homeGnssRatio == 0)) {
                Serial.printf("[CYCLE] Home (x%d). GNSS sanity refresh due (ratio 1:%d).\n",
                              homeCycleCount, homeGnssRatio);
                bool haveHomeFix = gnssAcquireFix();
                if (haveHomeFix) {
                    Serial.println("[CYCLE] Home GNSS sanity refresh has a fix; sending HOME telemetry.");
                    sendTelemetry();
                    listenForCommands();
                } else {
                    Serial.println("[CYCLE] Home GNSS failed; keeping HOME status and last known location downstream.");
                }
            }

            uint32_t lteHeartbeatS = currentConfig->lte_heartbeat_interval_s;
            if (lteHeartbeatS > 0 &&
                (lastLteHeartbeatMs == 0 ||
                 millis() - lastLteHeartbeatMs >= lteHeartbeatS * 1000UL)) {
                Serial.printf("[CELL] Home LTE heartbeat due (interval %lus)\n", lteHeartbeatS);
                cellularPending = true;
                lastLteHeartbeatMs = millis();
                xTaskNotifyGive(cellTaskHandle);
            }

            peripheralsSleep();
            enterDeepSleep();
            continue;
        }

        homeCycleCount = 0;

        // ── Phase 2: GNSS acquisition via AT command ──
        bool haveFix = gnssAcquireFix();
        Serial.printf("[GNSS] Away acquisition result: %s\n", haveFix ? "fix" : "no fix");

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
    uint32_t ttffTimeoutS = gpsWarmStart ? GPS_TTFF_WARM_TIMEOUT_S : GPS_TTFF_COLD_TIMEOUT_S;
    return gnssAcquireFixWithTimeout(ttffTimeoutS);
}

static bool gnssAcquireFixWithTimeout(uint32_t timeoutS) {
#if BLUEPAWS_GNSS_SPOOF_ENABLED
    return gnssSpoofAcquireFix();
#endif

    gnssEnable();
    gpsFix = false;

    Serial.printf("[GNSS] Acquiring fix (%s, %lus timeout)\n",
                  gpsWarmStart ? "warm" : "cold", timeoutS);

    uint32_t startMs = millis();
    uint32_t ttffTimeoutMs = timeoutS * 1000UL;

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
            if (lastError == BP_ERROR_GPS) lastError = BP_ERROR_NONE;  // Clear GPS error on success
            collarStateRememberFix();
            Serial.printf("[GNSS] Fix acquired after %lums\n", millis() - startMs);
            Serial.printf("[GNSS] Position: %.6f, %.6f (sats: %d)\n",
                          gnssLat, gnssLon, gnssSats);
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));  // Poll every 2s
    }

    Serial.println("[GNSS] Timeout — no fix");
    lastError = BP_ERROR_GPS;
    return false;
}

// ═══════════════════════════════════════════════
// Testbed GNSS Spoof
//
// Used only while the RAK4631/WisMesh board is standing in for the final collar
// PCB and no real GNSS path is fitted. It produces realistic-looking movement
// around the agreed bench coordinate, but does not alter the TLV contract.
// Disable with -DBLUEPAWS_GNSS_SPOOF_ENABLED=0 for production hardware.
// ═══════════════════════════════════════════════
static bool gnssSpoofAcquireFix() {
#if !BLUEPAWS_GNSS_SPOOF_ENABLED
    return false;
#else
    if (gpsMutex) {
        xSemaphoreTake(gpsMutex, portMAX_DELAY);
    }

    const float driftMaxM = BLUEPAWS_SPOOF_DRIFT_METRES_DEFAULT;
    const float phaseA = (float)((cycleCount % 360) * 0.104719755f);   // 6 degrees/cycle
    const float phaseB = (float)((messageSeq % 360) * 0.06981317f);    // 4 degrees/packet
    const float driftNorthM = sinf(phaseA) * driftMaxM;
    const float driftEastM = cosf(phaseB) * driftMaxM;
    const float metresPerDegLat = 111320.0f;
    const float metresPerDegLon = 111320.0f * cosf((float)BLUEPAWS_SPOOF_HOME_LAT * 0.01745329252f);

    gnssLat = BLUEPAWS_SPOOF_HOME_LAT + (driftNorthM / metresPerDegLat);
    gnssLon = BLUEPAWS_SPOOF_HOME_LON + (driftEastM / metresPerDegLon);
    gnssHdop = 120;      // HDOP 1.20, intentionally plausible not perfect
    gnssSats = 9;
    gnssUnixTime = compileTimeUnix() + (millis() / 1000UL);
    gnssFixAgeMs = millis();
    gpsFix = true;
    gpsWarmStart = true;
    if (lastError == BP_ERROR_GPS) lastError = BP_ERROR_NONE;
    collarStateRememberFix();

    if (gpsMutex) {
        xSemaphoreGive(gpsMutex);
    }

    Serial.printf("[GNSS-SPOOF] TESTBED fix %.6f, %.6f drift<=%.0fm sats=%u time=%lu\n",
                  gnssLat, gnssLon, driftMaxM, gnssSats, gnssUnixTime);
    return true;
#endif
}

static uint32_t compileTimeUnix() {
    // UTC epoch avoids the old whole-date/month lookup bug (always January)
    // and the compiler's local-time/DST offset. Only a bench/fallback anchor;
    // a reboot long after building still requires an authoritative time source.
    return (uint32_t)BLUEPAWS_BUILD_UNIX_TIME;
}

// ═══════════════════════════════════════════════
// Conservative Flash Persistence
// ═══════════════════════════════════════════════
static uint8_t captureResetReason() {
#if defined(NRF_POWER)
    uint32_t reason = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = reason;  // nRF52 reset reason bits are cleared by writing them back.
    return (uint8_t)(reason & 0xFF);
#else
    return 0;
#endif
}

static void collarStateDefaults() {
    memset(&persistedState, 0, sizeof(persistedState));
    persistedState.magic = BLUEPAWS_COLLAR_STATE_MAGIC;
    persistedState.version = BLUEPAWS_COLLAR_STATE_VERSION;
    persistedState.size = sizeof(persistedState);
    persistedState.profile = PROFILE_NORMAL;
    persistedState.config_revision = 1;
    strncpy(persistedState.apn, "iot.1nce.net", sizeof(persistedState.apn) - 1);
    currentProfile = PROFILE_NORMAL;
    currentConfig = bp_profile_config(PROFILE_NORMAL);
    inLostMode = false;
}

static uint32_t collarStateChecksum(collar_persisted_state_t state) {
    state.checksum = 0;
    const uint8_t *raw = (const uint8_t *)&state;
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < sizeof(state); i++) {
        hash ^= raw[i];
        hash *= 16777619UL;
    }
    return hash;
}

static bool collarStateLoad() {
    collarStateDefaults();
    if (!collarStateReady || !InternalFS.exists(BLUEPAWS_COLLAR_STATE_PATH)) {
        return false;
    }

    File file = InternalFS.open(BLUEPAWS_COLLAR_STATE_PATH, FILE_O_READ);
    if (!file) {
        return false;
    }

    collar_persisted_state_t loaded;
    int readBytes = file.read(&loaded, sizeof(loaded));
    file.close();

    if (readBytes != (int)sizeof(loaded) ||
        loaded.magic != BLUEPAWS_COLLAR_STATE_MAGIC ||
        loaded.version != BLUEPAWS_COLLAR_STATE_VERSION ||
        loaded.size != sizeof(loaded) ||
        loaded.checksum != collarStateChecksum(loaded) ||
        loaded.profile > BP_MAX_POWER_PROFILE) {
        Serial.println("[STATE] Stored state failed validation.");
        return false;
    }

    persistedState = loaded;
    return true;
}

static void collarStateSave(const char *reason, bool noisy) {
    if (!collarStateReady) return;

    persistedState.magic = BLUEPAWS_COLLAR_STATE_MAGIC;
    persistedState.version = BLUEPAWS_COLLAR_STATE_VERSION;
    persistedState.size = sizeof(persistedState);
    persistedState.profile = currentProfile;
    persistedState.lost_active = inLostMode ? 1 : 0;
    persistedState.message_seq_checkpoint = messageSeq;
    persistedState.runtime_counter_checkpoint = cycleCount;
    persistedState.home_cycle_checkpoint = homeCycleCount;
    persistedState.checksum = collarStateChecksum(persistedState);

    InternalFS.remove(BLUEPAWS_COLLAR_STATE_PATH);
    File file = InternalFS.open(BLUEPAWS_COLLAR_STATE_PATH, FILE_O_WRITE);
    if (!file) {
        Serial.println("[STATE] Save failed: could not open state file.");
        return;
    }

    size_t written = file.write((const uint8_t *)&persistedState, sizeof(persistedState));
    file.close();

    if (written != sizeof(persistedState)) {
        Serial.println("[STATE] Save failed: short write.");
        return;
    }

    if (noisy) {
        Serial.printf("[STATE] Saved (%s): profile=%s seq=%lu boots=%lu lost=%s\n",
                      reason,
                      bp_profile_name(currentProfile),
                      messageSeq,
                      persistedState.boot_counter,
                      inLostMode ? "yes" : "no");
    }
}

static void collarStateCheckpoint(const char *reason) {
    if ((messageSeq % BLUEPAWS_STATE_SEQ_CHECKPOINT_EVERY) == 0) {
        collarStateSave(reason, false);
    }
}

static void collarStateRememberFix() {
    if (!gpsFix) return;

    uint32_t fixAgeS = (millis() - gnssFixAgeMs) / 1000;
    if (fixAgeS >= GPS_STALE_THRESHOLD_S) return;

    persistedState.last_lat_e7 = (int32_t)(gnssLat * 1e7);
    persistedState.last_lon_e7 = (int32_t)(gnssLon * 1e7);
    persistedState.last_fix_unix = gnssGetUnixTime();
    persistedState.last_fix_acc_m = gnssHdop > 0 ? (uint16_t)((gnssHdop / 100.0) * 5) : 0;
    persistedState.last_fix_sats = gnssSats;
    persistedState.has_last_fix = 1;

    if (messageSeq == 0 || (messageSeq % BLUEPAWS_STATE_SEQ_CHECKPOINT_EVERY) == 0) {
        collarStateSave("gnss-fix", false);
    }
}

// ═══════════════════════════════════════════════
// User Button
// ═══════════════════════════════════════════════
static void buttonPoll() {
    static bool stablePressed = false;
    static bool lastRawPressed = false;
    static bool longHandled = false;
    static uint32_t lastChangeMs = 0;
    static uint32_t pressStartMs = 0;

    bool rawPressed = digitalRead(PIN_BUTTON) == LOW;
    uint32_t now = millis();

    if (rawPressed != lastRawPressed) {
        lastRawPressed = rawPressed;
        lastChangeMs = now;
    }

    if (now - lastChangeMs < BLUEPAWS_BUTTON_DEBOUNCE_MS) {
        return;
    }

    if (rawPressed != stablePressed) {
        stablePressed = rawPressed;
        if (stablePressed) {
            pressStartMs = now;
            longHandled = false;
        } else if (!longHandled) {
            forceUserReportRequested = true;
#if defined(BLUEPAWS_TESTBED_BUILD) && BLUEPAWS_TESTBED_BUILD
            forceImmediateCycle = true;
#endif
            if (cycleTaskHandle != NULL) {
                xTaskNotifyGive(cycleTaskHandle);
            }
            Serial.println("[BTN] Short press: user-requested report queued.");
            ledFlicker(1, 80, 40);
        }
    }

    if (stablePressed && !longHandled &&
        now - pressStartMs >= BLUEPAWS_BUTTON_LONG_PRESS_MS) {
        longHandled = true;
        bp_profile_t next = inLostMode ? LOST_MODE_FALLBACK : PROFILE_LOST;
        Serial.printf("[BTN] Long press: toggling %s.\n", bp_profile_name(next));
        applyProfile(next);
        forceUserReportRequested = true;
#if defined(BLUEPAWS_TESTBED_BUILD) && BLUEPAWS_TESTBED_BUILD
        forceImmediateCycle = true;
#endif
        if (cycleTaskHandle != NULL) {
            xTaskNotifyGive(cycleTaskHandle);
        }
        ledFlicker(inLostMode ? 4 : 2, 120, 80);
    }
}

// ═══════════════════════════════════════════════
// Finalize + Authenticate TLV v1.2 Packet
//
// pkt_finalize() appends the correctly-sized auth-tag area. If this collar has
// been locally provisioned with a 32-byte HMAC key, replace that placeholder
// with the first 8 bytes of HMAC-SHA256(key, header + TLVs).
// ═══════════════════════════════════════════════
static uint8_t finalizeAuthenticatedPacket(uint8_t *buf) {
    uint8_t pktLen = pkt_finalize(buf);

#if BLUEPAWS_COLLAR_HMAC_CONFIGURED
    const uint8_t authenticatedLen = pktLen - BP_AUTH_TAG_SIZE;
    bp_hmac_sha256_truncated8(collarHmacKey,
                              sizeof(collarHmacKey),
                              buf,
                              authenticatedLen,
                              &buf[authenticatedLen]);
#else
    static bool warned = false;
    if (!warned) {
        Serial.println("[AUTH] No collar HMAC key configured; using zero placeholder tag");
        warned = true;
    }
#endif

    return pktLen;
}

// ═══════════════════════════════════════════════
// Build and Send Telemetry (PKT_TELEMETRY)
// ═══════════════════════════════════════════════
static void sendTelemetry() {
    sendTelemetryWithReason(TX_TELEMETRY);
}

static void sendTelemetryWithReason(uint8_t txReason) {
    messageSeq++;

    bp_status_t status;
    uint8_t flags = 0;

    // Check if GNSS fix is valid and recent
    uint32_t fixAgeS = (millis() - gnssFixAgeMs) / 1000;
    bool locValid = gpsFix && (fixAgeS < GPS_STALE_THRESHOLD_S);

    if (bleHomeFound) {
        // A home GNSS sanity refresh is still a HOME observation. GNSS may
        // improve last-known accuracy, but seeing the trusted home beacon is
        // the stronger state signal for the customer-facing status.
        status = STATUS_HOME;
        flags |= FLAG_HOME_BEACON_SEEN;
        if (locValid) flags |= FLAG_GNSS_VALID | FLAG_FIX_3D;
    } else if (locValid) {
        status = STATUS_OUT_AND_ABOUT;  // Pet is outside with valid position
        flags |= FLAG_GNSS_VALID | FLAG_FIX_3D;
    } else {
        status = STATUS_INVALID_GPS;    // No usable GPS fix this cycle
    }

    if (gpsWarmStart && !locValid) flags |= FLAG_STALE_FIX;
    if (lastError != BP_ERROR_NONE) flags |= FLAG_ERROR_PRESENT;

    uint32_t unixTime = gnssGetUnixTime();  // Get timestamp from GNSS

    // ── Build the packet ──
    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, MY_HOME_HUB_ID, (uint16_t)(messageSeq & 0xFFFF), unixTime,
             status, currentProfile, flags, txReason);

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
    // ERROR_PRESENT reports a fault. TLV 0x06 describes a reset, not a fault.

    uint8_t pktLen = finalizeAuthenticatedPacket(buf);

    Serial.printf("[TX] %s seq=%lu status=%s size=%dB\n",
                  bp_tx_reason_display(txReason), messageSeq, bp_status_display(status), pktLen);
    pkt_print_hex(buf, pktLen);

    transmitPacket(buf, pktLen);

    // Save a copy for the cellular task to re-send via NB-IoT
    memcpy(lastTxPacket, buf, pktLen);
    lastTxPacketLen = pktLen;
    if (locValid) {
        collarStateRememberFix();
    }
    collarStateCheckpoint("tx");
}

// ═══════════════════════════════════════════════
// Send Boot Report
//
// Cold/reboot path deliberately attempts BLE Home and GNSS before reporting.
// If GNSS fails, the packet remains valid and presence-only; Supabase keeps the
// previous map position and updates last-seen/diagnostic fields.
// ═══════════════════════════════════════════════
static void sendBootReport(bool atHome, bool haveFix) {
    messageSeq++;

    uint8_t flags = 0;
    bp_status_t status = STATUS_INVALID_GPS;
    uint32_t fixAgeS = (millis() - gnssFixAgeMs) / 1000;
    bool locValid = haveFix && gpsFix && (fixAgeS < GPS_STALE_THRESHOLD_S);

    if (atHome) {
        status = STATUS_HOME;
        flags |= FLAG_HOME_BEACON_SEEN;
    } else if (locValid) {
        status = STATUS_OUT_AND_ABOUT;
    }

    if (locValid) {
        flags |= FLAG_GNSS_VALID | FLAG_FIX_3D;
    } else {
        flags |= FLAG_STALE_FIX | FLAG_ERROR_PRESENT;
    }
    if (lastError != BP_ERROR_NONE) flags |= FLAG_ERROR_PRESENT;

    uint32_t unixTime = locValid ? gnssGetUnixTime() : (compileTimeUnix() + millis() / 1000UL);

    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, MY_HOME_HUB_ID, (uint16_t)(messageSeq & 0xFFFF), unixTime,
             status, currentProfile, flags, TX_BOOT);

    uint16_t batt_mV = 3700;  // TODO: Read actual battery voltage via ADC
    if (locValid) {
        int32_t lat_e7 = (int32_t)(gnssLat * 1e7);
        int32_t lon_e7 = (int32_t)(gnssLon * 1e7);
        pkt_set_gps(buf, lat_e7, lon_e7);
        uint16_t acc_m = gnssHdop > 0 ? (uint16_t)((gnssHdop / 100.0) * 5) : 0;
        pkt_set_quality(buf, batt_mV, acc_m, (uint16_t)fixAgeS);
        pkt_set_sat_count(buf, gnssSats);
    } else {
        pkt_set_quality(buf, batt_mV, 0, 65535);
        pkt_set_sat_count(buf, 255);
    }

    uint16_t fw = (uint16_t)(((BLUEPAWS_FW_MAJOR & 0xFF) << 8) | (BLUEPAWS_FW_MINOR & 0xFF));
    pkt_add_tlv_u16(buf, TLV_FW_VER, fw);
    pkt_add_tlv_u8(buf, TLV_RESET_REASON, bootResetReason);
    pkt_add_tlv_u32(buf, TLV_UPTIME_S, millis() / 1000);

    uint8_t pktLen = finalizeAuthenticatedPacket(buf);

    Serial.printf("[TX] BOOT seq=%lu home=%s gnss=%s reset=0x%02X size=%dB\n",
                  messageSeq, atHome ? "yes" : "no", locValid ? "fix" : "no-fix",
                  bootResetReason, pktLen);
    pkt_print_hex(buf, pktLen);

    transmitPacket(buf, pktLen);

    memcpy(lastTxPacket, buf, pktLen);
    lastTxPacketLen = pktLen;
    if (locValid) {
        collarStateRememberFix();
    }
    collarStateCheckpoint("boot-report");
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
    if (lastError != BP_ERROR_NONE) flags |= FLAG_ERROR_PRESENT;

    pkt_init(buf, MY_DEVICE_ID, MY_HOME_HUB_ID, (uint16_t)(messageSeq & 0xFFFF), 0,
             STATUS_HOME, currentProfile, flags, TX_WAKE_CHECKIN);

    uint16_t batt_mV = 3700;  // TODO: Read actual battery voltage via ADC
    pkt_set_quality(buf, batt_mV, 0, 65535);
    pkt_set_sat_count(buf, 255);

    pkt_add_tlv_u32(buf, TLV_UPTIME_S, millis() / 1000);
    // Do not encode a runtime fault as a reset-reason TLV.

    uint8_t pktLen = finalizeAuthenticatedPacket(buf);

    Serial.printf("[TX] WAKE_CHECKIN seq=%lu homeCycles=%d size=%dB\n",
                  messageSeq, homeCycleCount, pktLen);
    pkt_print_hex(buf, pktLen);

    transmitPacket(buf, pktLen, /*suppressLed=*/true);

    // Save copy for diagnostics; routine cellular is intentionally skipped.
    memcpy(lastTxPacket, buf, pktLen);
    lastTxPacketLen = pktLen;
    collarStateCheckpoint("wake-checkin");
}

// ═══════════════════════════════════════════════
// Send Mode ACK (PKT_MODE_ACK)
// ═══════════════════════════════════════════════
static void sendModeAck(uint32_t cmdMsgSeq, uint16_t destinationId) {
    messageSeq++;
    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, destinationId, (uint16_t)(messageSeq & 0xFFFF), 0,
             STATUS_HOME, currentProfile, 0, TX_ACK);

    pkt_add_tlv_u8(buf,  TLV_PROFILE,       currentProfile);
    pkt_add_tlv_i8(buf,  TLV_TX_POWER,      currentConfig->tx_power_dBm);
    pkt_add_tlv_u16(buf, TLV_SLEEP_INTERVAL, currentConfig->sleep_interval_s);
    pkt_add_tlv_u16(buf, TLV_CMD_MSG_ID,    (uint16_t)(cmdMsgSeq & 0xFFFF));

    uint8_t pktLen = finalizeAuthenticatedPacket(buf);
    Serial.printf("[TX] MODE_ACK for cmd seq %lu\n", cmdMsgSeq);
    transmitPacket(buf, pktLen);
}

// ═══════════════════════════════════════════════
// Send Status Response (PKT_STATUS_RESP)
// ═══════════════════════════════════════════════
static void sendStatusResponse(uint32_t cmdMsgSeq, uint16_t destinationId) {
    messageSeq++;
    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, destinationId, (uint16_t)(messageSeq & 0xFFFF), 0,
             STATUS_HOME, currentProfile, 0, TX_ACK);

    pkt_add_tlv_u8(buf,  TLV_PROFILE,        currentProfile);
    pkt_add_tlv_i8(buf,  TLV_TX_POWER,       currentConfig->tx_power_dBm);
    pkt_add_tlv_u16(buf, TLV_SLEEP_INTERVAL,  currentConfig->sleep_interval_s);
    pkt_add_tlv_u8(buf,  TLV_GPS_WARM,        gpsWarmStart ? 1 : 0);
    pkt_add_tlv_u8(buf,  TLV_HOME_CYCLES,     homeCycleCount);
    pkt_add_tlv_u16(buf, TLV_CMD_MSG_ID,     (uint16_t)(cmdMsgSeq & 0xFFFF));

    uint8_t pktLen = finalizeAuthenticatedPacket(buf);
    Serial.printf("[TX] STATUS_RESP for cmd seq %lu\n", cmdMsgSeq);
    transmitPacket(buf, pktLen);
}

// ═══════════════════════════════════════════════
// Send Find ACK (PKT_FIND_ACK)
// ═══════════════════════════════════════════════
static void sendFindAck(uint32_t cmdMsgSeq, uint16_t destinationId) {
    messageSeq++;
    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, destinationId, (uint16_t)(messageSeq & 0xFFFF), 0,
             STATUS_HOME, currentProfile, 0, TX_ACK);

    pkt_add_tlv_u16(buf, TLV_CMD_MSG_ID, (uint16_t)(cmdMsgSeq & 0xFFFF));
    pkt_add_tlv_u8(buf,  TLV_PROFILE,    currentProfile);

    uint8_t pktLen = finalizeAuthenticatedPacket(buf);
    Serial.printf("[TX] FIND_ACK for cmd seq %lu\n", cmdMsgSeq);
    transmitPacket(buf, pktLen);
}

// ═══════════════════════════════════════════════
// Send Lost Mode Timeout Alert (PKT_ALERT)
// ═══════════════════════════════════════════════
static void sendLostModeAlert() {
    messageSeq++;
    uint8_t buf[BP_MAX_PACKET_SIZE];
    pkt_init(buf, MY_DEVICE_ID, MY_HOME_HUB_ID, (uint16_t)(messageSeq & 0xFFFF), 0,
             STATUS_LOST_TIMEOUT, currentProfile, 0, PKT_ALERT);

    uint32_t duration = (millis() - lostModeStartMs) / 1000;
    pkt_add_tlv_u32(buf, TLV_DURATION_S, duration);
    pkt_add_tlv_u8(buf,  TLV_NEW_MODE,  (uint8_t)LOST_MODE_FALLBACK);

    uint8_t pktLen = finalizeAuthenticatedPacket(buf);
    Serial.printf("[TX] ALERT: lost mode timeout after %lus\n", duration);
    transmitPacket(buf, pktLen);
}

// ═══════════════════════════════════════════════
// LoRa Transmit
//
// Wakes the radio, transmits the raw TLV v1.2 packet, then provides visual
// feedback via LED flashes.
// LED pattern tells you the result: normal flashes = OK,
// 2 slow flashes = timeout, 6 rapid flashes = error.
// ═══════════════════════════════════════════════
static void transmitPacket(uint8_t *buf, uint8_t len, bool suppressLed) {
    lora.standby();                      // Wake radio from sleep → standby mode
    int state = lora.transmit(buf, len); // Blocking TX — returns when done or timeout

    if (state == RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] TX OK (%d bytes)\n", len);
        if (lastError == BP_ERROR_RF) lastError = BP_ERROR_NONE;  // Clear RF error on success
        if (!suppressLed) {
            ledFlicker(currentConfig->led_flashes, 50, 50);  // Success: profile-defined flash count
        }
    } else if (state == RADIOLIB_ERR_TX_TIMEOUT) {
        Serial.println("[LORA] TX timeout");
        lastError = BP_ERROR_RF;
        ledFlicker(2, 200, 200);  // Slow double-flash = timeout (always show errors)
    } else {
        Serial.printf("[LORA] TX failed: %d\n", state);
        lastError = BP_ERROR_RF;
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

    uint8_t rxBuf[BP_MAX_PACKET_SIZE];
    int state = lora.receive(rxBuf, sizeof(rxBuf), CMD_LISTEN_WINDOW_MS);

    if (state == RADIOLIB_ERR_NONE) {
        size_t rxLen = lora.getPacketLength(false);
        if (rxLen == 0 || rxLen > sizeof(rxBuf)) {
            rxLen = sizeof(rxBuf);
        }

        Serial.printf("[RX] Received %d bytes\n", (int)rxLen);
        pkt_print_hex(rxBuf, (uint8_t)rxLen);  // Debug: hex dump to serial

        // Basic validation before processing
        if (rxLen >= BP_MIN_PACKET_SIZE && rxBuf[0] == BP_PROTOCOL_VERSION) {
            handleReceivedCommand(rxBuf, (uint8_t)rxLen);
        }
    } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
        Serial.println("[RX] No command received");
    } else {
        Serial.printf("[RX] receive failed: %d\n", state);
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
    // Validate TLV v1.2 structure only. Downlink command authentication is
    // deliberately deferred to the later command-protocol milestone; these
    // ACKed commands are for prototype/bench testing and are not production-secure.
    if (!pkt_validate_crc(buf, len)) {
        Serial.println("[RX] TLV structure failed — dropping");
        return;
    }

    // v1.2 explicitly distinguishes the command originator from its target.
    uint16_t sourceId = pkt_source_id(buf);
    uint16_t targetId = pkt_destination_id(buf);
    if (targetId != MY_DEVICE_ID && targetId != DEVICE_ID_BROADCAST) {
        Serial.printf("[RX] Not for us (0x%04X)\n", targetId);
        return;
    }
    if (!bp_is_hub_id(sourceId)) {
        Serial.printf("[RX] Command source is not a hub (0x%04X)\n", sourceId);
        return;
    }

    uint16_t pktType = pkt_pkt_type(buf);
    uint32_t cmdSeq  = pkt_msg_seq(buf);

    // ── Deduplication ──
    // The hub retries commands up to 3 times if it doesn't get an ACK.
    // We track the last processed sequence number to avoid executing
    // the same command multiple times (e.g. switching profile twice).
    if (cmdSeq != 0 && cmdSeq == lastProcessedCmdSeq) {
        // The original ACK may have been lost. Do not apply the command twice,
        // but repeat the correct ACK so hub/cloud delivery can converge.
        Serial.printf("[RX] Duplicate cmd seq %lu — re-ACKing\n", cmdSeq);
        if (pktType == PKT_CMD_MODE) sendModeAck(cmdSeq, lastCommandSourceId);
        else if (pktType == PKT_CMD_STATUS) sendStatusResponse(cmdSeq, lastCommandSourceId);
        else if (pktType == PKT_CMD_FIND) sendFindAck(cmdSeq, lastCommandSourceId);
        return;
    }
    lastProcessedCmdSeq = cmdSeq;
    lastCommandSourceId = sourceId;

    switch (pktType) {
    case PKT_CMD_MODE: {
        // Hub is telling us to switch to a different operating profile.
        // Extract the target profile from TLV_PROFILE in the packet payload.
        uint8_t newProfile;
        if (pkt_tlv_get_u8(buf, TLV_PROFILE, &newProfile)) {
            Serial.printf("[RX] CMD_MODE → %s (seq %lu)\n",
                          bp_profile_name((bp_profile_t)newProfile), cmdSeq);
            applyProfile((bp_profile_t)newProfile);  // Apply new settings (TX power, sleep time, etc.)
            sendModeAck(cmdSeq, sourceId);           // ACK back to the originating hub
        } else {
            Serial.println("[RX] CMD_MODE missing TLV_PROFILE");
        }
        break;
    }
    case PKT_CMD_STATUS:
        // Hub wants a status report — send back current config details
        Serial.printf("[RX] CMD_STATUS (seq %lu)\n", cmdSeq);
        sendStatusResponse(cmdSeq, sourceId);
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

        sendFindAck(cmdSeq, sourceId);  // ACK back to the originating hub
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
// BLE Lost/Find Beacon
//
// When the collar enters PROFILE_LOST (Lost Alert mode),
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

    // BLE find beacon: Lost Alert only. Active is higher-frequency monitoring,
    // not emergency close-range search.
    if (profile == PROFILE_LOST) {
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
    collarStateSave("profile", true);
}

static uint16_t effectiveSleepIntervalS() {
#if defined(BLUEPAWS_TESTBED_BUILD) && BLUEPAWS_TESTBED_BUILD
    if (debugCadenceEnabled && currentProfile != PROFILE_LOST) {
        return debugSleepIntervalS;
    }
#endif
    return currentConfig->sleep_interval_s;
}

#if defined(BLUEPAWS_TESTBED_BUILD) && BLUEPAWS_TESTBED_BUILD
static void debugConsolePoll() {
    static String line;

    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            debugConsoleHandleLine(line);
            line = "";
            continue;
        }

        if (line.length() < 96) {
            line += c;
        } else {
            line = "";
            Serial.println("[DBG] Command too long; discarded. Type 'help'.");
        }
    }
}

static void debugConsoleHandleLine(String line) {
    line.trim();
    if (line.length() == 0) return;

    String lower = line;
    lower.toLowerCase();

    if (lower == "help" || lower == "?") {
        debugConsolePrintHelp();
        return;
    }

    if (lower == "status") {
        debugConsolePrintStatus();
        return;
    }

    if (lower == "tx" || lower == "send" || lower == "wake") {
        forceUserReportRequested = true;
        forceImmediateCycle = true;
        if (cycleTaskHandle != NULL) {
            xTaskNotifyGive(cycleTaskHandle);
        }
        Serial.println("[DBG] Forced user-requested report queued.");
        return;
    }

    if (lower == "debug on") {
        debugCadenceEnabled = true;
        forceImmediateCycle = true;
        if (cycleTaskHandle != NULL) {
            xTaskNotifyGive(cycleTaskHandle);
        }
        Serial.printf("[DBG] Debug cadence ON: %us sleep interval.\n", debugSleepIntervalS);
        return;
    }

    if (lower == "debug off") {
        debugCadenceEnabled = false;
        Serial.println("[DBG] Debug cadence OFF: profile cadence restored.");
        debugConsolePrintStatus();
        return;
    }

    if (lower.startsWith("interval ")) {
        String value = lower.substring(9);
        value.trim();
        long seconds = value.toInt();
        if (seconds < 5 || seconds > 3600) {
            Serial.println("[DBG] interval must be 5..3600 seconds.");
            return;
        }
        debugSleepIntervalS = (uint16_t)seconds;
        debugCadenceEnabled = true;
        forceImmediateCycle = true;
        if (cycleTaskHandle != NULL) {
            xTaskNotifyGive(cycleTaskHandle);
        }
        Serial.printf("[DBG] Debug cadence ON: %us sleep interval.\n", debugSleepIntervalS);
        return;
    }

    if (lower.startsWith("profile ")) {
        String value = lower.substring(8);
        value.trim();

        bp_profile_t next = PROFILE_NORMAL;
        if (value == "normal") {
            next = PROFILE_NORMAL;
        } else if (value == "powersave" || value == "power_save" || value == "power-save") {
            next = PROFILE_POWERSAVE;
        } else if (value == "active") {
            next = PROFILE_ACTIVE;
        } else if (value == "lost" || value == "lost_alert" || value == "lost-alert") {
            next = PROFILE_LOST;
        } else if (value == "debug" || value == "dev" || value == "dev_debug" || value == "dev-debug") {
            next = PROFILE_DEBUG;
        } else {
            Serial.println("[DBG] Unknown profile. Use: normal, powersave, active, lost, debug.");
            return;
        }

        applyProfile(next);
        forceImmediateCycle = true;
        if (cycleTaskHandle != NULL) {
            xTaskNotifyGive(cycleTaskHandle);
        }
        debugConsolePrintStatus();
        return;
    }

    Serial.printf("[DBG] Unknown command: %s\n", line.c_str());
    Serial.println("[DBG] Type 'help' for commands.");
}

static void debugConsolePrintHelp() {
    Serial.println();
    Serial.println("[DBG] Bluepaws collar testbed commands");
    Serial.println("      help                 Show this help");
    Serial.println("      status               Print profile/cadence/counters");
    Serial.println("      profile normal       Set Normal profile");
    Serial.println("      profile powersave    Set Power Save profile");
    Serial.println("      profile active       Set Active profile");
    Serial.println("      profile lost         Set Lost Alert profile");
    Serial.println("      profile debug        Set development Debug profile");
    Serial.println("      debug on             Override sleep interval to 60s");
    Serial.println("      debug off            Restore profile sleep interval");
    Serial.println("      interval <seconds>   Override sleep interval, 5..3600s");
    Serial.println("      tx                   Request an immediate next cycle");
    Serial.println();
}

static void debugConsolePrintStatus() {
    Serial.printf("[DBG] device=%u profile=%s debug=%s interval=%us seq=%lu cycles=%lu home_wakes=%u lost=%s\n",
                  (unsigned)MY_DEVICE_ID,
                  bp_profile_name(currentProfile),
                  debugCadenceEnabled ? "on" : "off",
                  effectiveSleepIntervalS(),
                  messageSeq,
                  cycleCount,
                  homeCycleCount,
                  inLostMode ? "yes" : "no");
}
#endif

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
        lastError = BP_ERROR_CELLULAR;
        return;
    }
    if (lastError == BP_ERROR_CELLULAR) lastError = BP_ERROR_NONE;  // Modem responded OK

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
    uint16_t sleepIntervalS = effectiveSleepIntervalS();
    uint32_t sleepMs = sleepIntervalS * 1000UL;

    Serial.printf("[SLEEP] %us (all peripherals off, RTC only%s)\n",
                  sleepIntervalS,
#if defined(BLUEPAWS_TESTBED_BUILD) && BLUEPAWS_TESTBED_BUILD
                  debugCadenceEnabled ? ", debug cadence" : ""
#else
                  ""
#endif
    );
    Serial.flush();

    // FreeRTOS tickless idle will put the nRF52840 into
    // system-on sleep mode. Only the RTC and ULP remain active.
    // All GPIOs retain state (GPS sleep pin stays LOW, etc).
#if defined(BLUEPAWS_TESTBED_BUILD) && BLUEPAWS_TESTBED_BUILD
    if (forceImmediateCycle) {
        forceImmediateCycle = false;
        Serial.println("[DBG] Sleep skipped for forced cycle.");
    } else {
        uint32_t sleptMs = 0;
        while (sleptMs < sleepMs) {
            uint32_t remainingMs = sleepMs - sleptMs;
            uint32_t chunkMs = remainingMs > 1000UL ? 1000UL : remainingMs;
            uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(chunkMs));
            if (notified > 0 || forceImmediateCycle) {
                forceImmediateCycle = false;
                Serial.println("[DBG] Sleep interrupted for debug command.");
                break;
            }
            sleptMs += chunkMs;
        }
    }
#else
    vTaskDelay(pdMS_TO_TICKS(sleepMs));
#endif

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
