/*
  ┌─────────────────────────────────────────────────────────────┐
  │  BLUEPAWS V4 — HOME HUB FIRMWARE                            │
  │  Hardware: Heltec Wireless Tracker V2 ESP32-S3 + SX1262     │
  │                                                             │
  │  The hub is the "base station" that sits at home. It:       │
  │   - Receives telemetry packets from collar(s) over LoRa     │
  │   - Sends commands to collar(s) (mode change, find, etc.)   │
  │   - Hosts a web GUI for viewing device locations on a map   │
  │   - Advertises a BLE beacon so collars know when pet is home│
  │   - Relays telemetry to a cloud endpoint (Supabase etc.)    │
  │                                                             │
  │  FreeRTOS Tasks (pinned to cores):                          │
  │    loraTask  (core 1, prio 3) — RX/TX LoRa packets         │
  │    webTask   (core 1, prio 2) — HTTP server + SSE push      │
  │    network   (core 0, prio 2) — Wi-Fi policy + captive DNS  │
  │    bleTask   (core 0, prio 1) — BLE home beacon advertising │
  │    cloudTask (core 0, prio 1) — REST POST relay to cloud    │
  │  Main loop() yields to scheduler (does nothing).            │
  └─────────────────────────────────────────────────────────────┘
*/

// ── Arduino / ESP32 core ──
#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>        // SX1262 LoRa radio driver
#include <WiFi.h>            // WiFi AP+STA dual mode
#include <DNSServer.h>       // Captive portal wildcard DNS in Off-Grid mode
#include <WebServer.h>       // Lightweight HTTP server (port 80)
#include <LittleFS.h>        // On-chip flash filesystem (stores web files + logs)
#include <ESPmDNS.h>         // mDNS so you can browse to http://bluepaws.local
#include <BLEDevice.h>       // BLE for home beacon advertising
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <HTTPClient.h>      // HTTP client for cloud POST relay
#include <esp_system.h>
#include <esp_wifi.h>
#include <atomic>
#include <ArduinoJson.h>     // Parse pending commands returned by the Edge Function
#include <time.h>

// ── FreeRTOS primitives ──
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>    // xTaskCreatePinnedToCore, vTaskDelay
#include <freertos/queue.h>   // xQueueCreate, xQueueSend/Receive
#include <freertos/semphr.h>  // xSemaphoreCreateMutex, Take/Give

// ── BluePaws shared protocol library ──
#include <bp_protocol.h>     // Binary TLV packet format, builder & parser
#include <bp_config.h>       // LoRa params, profiles, AES key, timing constants
#include <bp_crypto.h>       // Legacy AES helper retained for downlink experiments
#include "hub_config.h"      // Bench-safe defaults + local-only secret overrides
#include "hub_pins.h"        // GPIO pin assignments for this board
#include "offline_journal.h" // Crash-tolerant per-collar offline history
#include "offline_access.h"  // RAM-only optional command PIN and sessions
#include "wifi_failover.h"

// ═══════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════

// Naming note: older notes and logs may say "LoRa hub", "base station",
// "receiving base station", or "Home Hub LoRa relay". In Bluepaws V4 these all
// mean this Home Hub: it receives raw collar TLV over LoRa and optionally relays
// the unchanged packet to Supabase in an HTTPS JSON wrapper.

// mDNS hostname — after connecting, browse to http://bluepaws.local
#define MDNS_HOSTNAME    "bluepaws"

// Network ports
#define WS_PORT          81   // Reserved for future WebSocket use
#define HTTP_PORT        80   // Web GUI + API endpoints

// On-chip storage paths (LittleFS flash filesystem)
#define CONFIG_FILE_PATH "/config.json"   // Saved WiFi/cloud credentials
#define DEVICE_META_PATH "/devices.meta"  // Hub-local name/emoji/colour overrides

// ═══════════════════════════════════════════════
// FreeRTOS Task Configuration
// Stack sizes are in bytes. Priority: higher number = higher priority.
// ═══════════════════════════════════════════════
#define STACK_LORA   4096   // LoRa RX/TX — moderate stack (radio + crypto)
#define STACK_WEB    8192   // Web server — large stack (JSON building, string ops)
#define STACK_BLE    4096   // BLE beacon + scanning in portable mode
#define STACK_CLOUD  16384  // Cloud relay — HTTPS/TLS + HTTPClient need generous stack
#define STACK_STORAGE 6144  // Asynchronous LittleFS journal writes
#define STACK_CONSOLE 4096  // USB serial bench console
#define STACK_NETWORK 6144  // Sole Wi-Fi/DNS owner; no TLS, flash or web handlers

#define PRIO_LORA    3      // Highest — LoRa packets are time-sensitive
#define PRIO_WEB     2      // Medium — serves the GUI
#define PRIO_CLOUD   1      // TLS/replay must yield to connectivity and radio work
#define PRIO_STORAGE 2      // Medium — never block LoRa reception on flash
#define PRIO_BLE     1      // Lowest — BLE beacon just needs to stay alive
#define PRIO_CONSOLE 1      // Low — bench/debug command parser
#define PRIO_NETWORK 2     // Well below ESP-IDF Wi-Fi/system task priorities

#define LORA_RX_POLL_TIMEOUT_MS 250  // Short timed RX window; avoids relying solely on DIO1 ISR

// ═══════════════════════════════════════════════
// Globals
// ═══════════════════════════════════════════════

// ── LoRa Radio ──
// SX1262 connected via HSPI bus. loraMutex protects the SPI bus so
// only one task accesses the radio at a time (RX vs TX).
static SPIClass loraSPI(HSPI);
static SX1262 lora = new Module(PIN_LORA_NSS, PIN_LORA_DIO1,
                                 PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);
static volatile bool loraPacketReceived = false;  // Set by DIO1 ISR when packet arrives
static SemaphoreHandle_t loraMutex = NULL;        // Protects SPI bus access

// ── Web Server ──
static WebServer httpServer(HTTP_PORT);
static DNSServer captiveDns;
static constexpr uint8_t CAPTIVE_DNS_PORT = 53;
static OfflineAccess offlineAccess;

// ── SSE (Server-Sent Events) ──
// Instead of WebSocket, we use SSE for real-time push to the browser.
// SSE is simpler (no extra library), uni-directional (server→client),
// and works in all modern browsers. The client opens GET /events and
// the server pushes "event: <type>\ndata: <json>\n\n" lines.
// We support up to 8 simultaneous SSE clients; further browsers poll slowly.
static constexpr uint8_t MAX_SSE_CLIENTS = 8;
static int snapshotLastHttpCode = 0;
static WiFiClient sseClients[MAX_SSE_CLIENTS];
static uint8_t sseClientCount = 0;        // How many SSE clients are connected
static SemaphoreHandle_t sseMutex = NULL;  // Protects the sseClients array

// ── Command TX Queue (hub → collar) ──
// When the web GUI sends a command (mode change, find, etc.), it gets
// queued here. The loraTask pulls from this queue and transmits via LoRa.
#define CMD_QUEUE_SIZE   8
static QueueHandle_t cmdQueue = NULL;

struct cmd_entry_t {
    uint8_t  buf[BP_MAX_PACKET_SIZE];  // Pre-built packet (before encryption)
    uint8_t  len;                      // Packet length in bytes
    uint16_t targetId;                 // Target collar
    uint16_t cmdSeq;                   // Downlink command sequence for ACK matching
    bp_pkt_type_t type;                // Command class
};

// ── Cloud Relay Queue ──
// Every received telemetry packet gets queued here for the cloudTask
// to POST to the cloud endpoint. Queue drops silently if full.
#define CLOUD_QUEUE_SIZE 16
static QueueHandle_t cloudQueue = NULL;

struct cloud_entry_t {
    uint8_t  buf[BP_MAX_PACKET_SIZE];  // Raw packet data
    uint8_t  len;                      // Packet length
    int16_t  rssi;                     // Signal strength when received
    float    snr;                      // Signal-to-noise ratio when received
    uint32_t gateway_rx_time_unix;      // Hub receive time used in cloud wrapper
    uint32_t local_id;                  // Journal record updated after cloud result
    uint16_t source_id;
};

#define STORAGE_QUEUE_SIZE 24
static QueueHandle_t storageQueue = NULL;
static OfflineJournal offlineJournal;

// ── Legacy AES-128 key ──
// TLV v1.2 uplink packets are raw authenticated TLV over private LoRa. Keep
// this only until the downlink command protocol is revised.
static const uint8_t aesKey[16] = LORA_AES_KEY;

// ── Packet Statistics (displayed in web GUI status) ──
static uint32_t rxCount = 0;         // Total valid packets received
static uint32_t crcFailCount = 0;    // Packets that failed TLV structure/auth checks
static uint32_t txCount = 0;         // Total commands transmitted
static std::atomic<uint32_t> cmdSeqCounter{0}; // Web, console and cloud tasks

// ── Pending Command ACK Tracking ──
// When we send a command to a collar, we track it here and wait for
// an ACK packet back. If no ACK within CMD_ACK_TIMEOUT_MS, we retry
// up to CMD_MAX_RETRIES times, then wait for another RX opportunity until expiry.
struct pending_cmd_t {
    uint32_t cmdSeq;        // msg_seq we assigned to this command
    uint16_t targetId;      // device_id of the target collar
    bp_pkt_type_t type;     // command type (PKT_CMD_MODE, PKT_CMD_FIND, etc.)
    uint32_t sentAtMs;      // millis() timestamp when last sent; 0 = queued but not TX-confirmed yet
    uint32_t createdAtMs;   // local delivery expires ten minutes after creation
    const char *state;     // RAM-only feedback, retained up to fifteen minutes
    uint8_t  retries;       // how many retransmissions so far
    uint8_t  buf[BP_MAX_PACKET_SIZE];  // original packet (for retransmission)
    uint8_t  len;           // packet length
    bool     active;        // true = this slot is tracking a pending command
    bool     waitingOpportunity; // blind retries exhausted; wait for next collar uplink
};

#define MAX_PENDING_CMDS 16                         // One useful pending command per collar
#define LOCAL_COMMAND_TTL_MS 600000UL
#define COMMAND_FEEDBACK_TTL_MS 900000UL
static pending_cmd_t pendingCmds[MAX_PENDING_CMDS];
static SemaphoreHandle_t pendingMutex = NULL;       // Protects pendingCmds array
static volatile TickType_t commandRxOpportunityUntil = 0;

// ── WiFi / Cloud State ──
static std::atomic<bool> staConnected{false};
static String staSSID = WIFI_STA_SSID;        // Current STA SSID (may be loaded from config file)
static String staPass = WIFI_STA_PASS;        // Current STA password
static String secondarySSID = WIFI_SECONDARY_SSID;
static String secondaryPass = WIFI_SECONDARY_PASS;
static String cloudEndpoint = CLOUD_ENDPOINT; // Cloud POST URL
static String cloudToken = CLOUD_BEARER_TOKEN; // Gateway bearer token for Supabase Edge Function
static bool hubProvisioningMode = HUB_PROVISIONING_MODE_DEFAULT;
static std::atomic<bool> hubApEnabled{false};
static std::atomic<bool> homeBeaconAllowed{false};
static std::atomic<bool> hubBeaconEnabled{true}; // User preference; cannot bypass Home Wi-Fi gate.
static std::atomic<bool> hubBeaconAdvertising{false}; // Actual BLE task state.
static void initHubPresence();
static void handleHubPresence();
static void handleHubPreferences();
static void postHubPresence();
static void pollHubSettings();
static std::atomic<bool> clearOfflineSessions{false};
static std::atomic<bool> knownWifiAvailable{false};
static std::atomic<bool> modeChangePending{false};
static std::atomic<bool> networkStackReady{false};
static std::atomic<WifiFailover::Phase> wifiPhase{WifiFailover::Phase::Idle};
static std::atomic<uint32_t> wifiRecoveryRemainingMs{0};
static std::atomic<uint32_t> apStartFailures{0};
static bool captiveDnsRunning = false; // network task only
static bool hubTimeSynced = false;
static uint32_t lastNtpSyncMs = 0;

// ── Hub Communications Profile ──
// Connectivity selects Home/Portable, then latches Off-Grid after 30 seconds
// without either configured uplink. Only a confirmed request leaves Off-Grid.
enum hub_comm_profile_t : uint8_t {
    HUB_COMM_HOME = 0,       // Fixed at home, normal household WiFi/cloud path.
    HUB_COMM_PORTABLE = 1,   // User deliberately took the hub roaming; cloud is best-effort.
    HUB_COMM_OFF_GRID = 2,   // Local-only roaming/search mode; cloud relay intentionally disabled.
};

static std::atomic<hub_comm_profile_t> hubCommProfile{HUB_COMM_HOME};
struct network_request_t { hub_comm_profile_t profile; bool confirmed; uint32_t requestedAt; };
static QueueHandle_t networkQueue = nullptr;

struct hub_connectivity_t {
    std::atomic<bool> wifi_connected{false};
    std::atomic<bool> internet_reachable{false}; // promoted by a successful cloud response
    std::atomic<bool> cloud_reachable{false};
    std::atomic<bool> lora_rx_active{false};
    std::atomic<uint32_t> last_cloud_success_ms{0};
    std::atomic<uint8_t> cloud_failures{0};
};

static hub_connectivity_t hubConnectivity;

// ── BLE Scan Results (Portable Mode) ──
#define MAX_BLE_DEVICES 8
struct ble_scan_result_t {
    uint16_t device_id;
    int      rssi;
    uint32_t last_seen_ms;
};
static ble_scan_result_t bleScanResults[MAX_BLE_DEVICES];
static uint8_t bleScanCount = 0;
static SemaphoreHandle_t bleMutex = NULL;

#include <BLEScan.h>
static BLEScan *pBLEScan = nullptr;

// ── Storage ──
// ── FreeRTOS Task Handles ──
static TaskHandle_t loraTaskHandle  = NULL;
static TaskHandle_t webTaskHandle   = NULL;
static TaskHandle_t bleTaskHandle   = NULL;
static TaskHandle_t cloudTaskHandle = NULL;
static TaskHandle_t storageTaskHandle = NULL;
static TaskHandle_t consoleTaskHandle = NULL;
static TaskHandle_t networkTaskHandle = NULL;

// ── Device State Table ──
// Stores the latest telemetry from each collar so the web GUI can
// display it immediately when a new browser connects. Up to 16 collars.
#define MAX_DEVICES 16
struct device_state_t {
    uint16_t device_id;      // Collar's unique ID (e.g. 0x0001)
    uint32_t last_seq;       // Last message sequence number received
    uint32_t last_time;      // Unix timestamp from the collar's GPS
    int32_t  lat_e7;         // Latitude × 10^7 (integer encoding, ~1cm precision)
    int32_t  lon_e7;         // Longitude × 10^7
    uint16_t batt_mV;        // Battery voltage in millivolts
    uint16_t acc_m;          // GPS accuracy in meters (derived from HDOP)
    uint16_t fix_age_s;      // How old the GPS fix is, in seconds
    uint8_t  status;         // bp_status_t — OK, OUT_AND_ABOUT, LOST, etc.
    uint8_t  profile;        // bp_profile_t — NORMAL, POWERSAVE, ACTIVE, LOST
    bool     error_present; // Header ERROR_PRESENT, never inferred from reset_reason
    uint8_t  flags;         // Latest packet flags, independent of retained map position
    uint8_t  tx_reason;
    uint8_t  reset_reason;
    bool     reset_reason_present;
    bool     heard_this_boot; // Journal restoration is NOT a live RX opportunity
    int16_t  rssi;           // LoRa RSSI when hub received the packet (dBm)
    float    snr;            // LoRa SNR when hub received the packet (dB)
    uint32_t local_millis;   // millis() on the hub when this packet arrived
    uint32_t gateway_rx_time_unix;
    uint32_t local_id;
    uint8_t sync_state;
    bool     has_gps;        // true if collar had a valid GPS fix
};

static device_state_t devices[MAX_DEVICES];    // Device state table
static uint8_t deviceCount = 0;                // How many unique collars we've seen
static SemaphoreHandle_t deviceMutex = NULL;   // Protects devices[] array

struct device_meta_t {
    uint16_t device_id;
    char name[33];
    char emoji[17];
    char colour[8];
    bool local_override;
};

static device_meta_t deviceMeta[MAX_DEVICES];
static uint8_t deviceMetaCount = 0;

// ═══════════════════════════════════════════════
// Forward Declarations
// ═══════════════════════════════════════════════

// FreeRTOS task entry points
static void loraTask(void *param);
static void webTask(void *param);
static void bleTask(void *param);
static void cloudTask(void *param);
static void storageTask(void *param);
static void consoleTask(void *param);
static void networkTask(void *param);
static bool requestHubMode(hub_comm_profile_t profile, bool confirmed);

// Hardware initialisation
static void initLoRa();
static void femInit();
static void femSetRx();
static void femSetTx();
static void initBLE();
static void initStorage();
static void initWebServer();

// Hub communications profile / connectivity health
static const char *hubCommProfileName(hub_comm_profile_t profile);
static hub_comm_profile_t hubCommProfileFromString(const String &value);
static bool hubProfileUsesBleScanning();
static bool hubProfileAllowsCloudRelay();
static bool hubProfileNeedsLocalAp();
static void setHubCommunicationsProfile(hub_comm_profile_t profile);
static void applyBleRoleForCurrentProfile();
static void applyWifiRoleForCurrentProfile();
static void updateConnectivityState();
static void noteCloudPostResult(int httpCode);
static void syncHubClock(bool force);

// Packet handling pipeline
static void handlePacket(const uint8_t *buf, uint8_t len, int16_t rssi, float snr);
static void buildDeviceJson(const uint8_t *buf, int16_t rssi, float snr,
                             uint32_t localId, uint32_t gatewayRxTime,
                             uint8_t syncState, char *out, size_t outLen);
static void updateDeviceStateFromRecord(const bp_journal_record_t &record, bool live = false);
static void sseBroadcast(const char *event, const char *data);
static String base64Encode(const uint8_t *data, uint8_t len);
static String buildCloudWrapperJson(const cloud_entry_t &entry);
static void replayPendingJournal();
static void syncCloudSnapshotMetadata();
static device_state_t *findDevice(uint16_t id);
static const char *journalSyncName(uint8_t state);
static uint32_t deviceAgeSeconds(const device_state_t &device);
static void loadDeviceMetadata();
static bool saveDeviceMetadata();
static device_meta_t *findDeviceMetadata(uint16_t id, bool create = false);
static const char *deviceDisplayName(uint16_t id);
static const char *deviceEmoji(uint16_t id);
static const char *deviceColour(uint16_t id);

// Command building & ACK tracking
static uint16_t sendCommand(uint16_t target_id, bp_pkt_type_t type, bp_profile_t mode);
static uint16_t sendCommandFind(uint16_t target_id, bp_pkt_type_t type,
                              bp_profile_t mode, uint8_t ledFlash,
                              bp_buzzer_pattern_t buzzerPattern,
                              uint16_t sequenceOverride = 0, uint32_t initialAgeMs = 0);
static bool queueCloudCommandResponse(const String &response, uint16_t expectedDeviceId);
static uint16_t sendStatusCommand(uint16_t target_id);
static void broadcastCommand(const pending_cmd_t &cmd);
static void handleApiCommands();
static bool commandStillPending(const cmd_entry_t &cmd);
static uint32_t deviceRxWindowMs(const device_state_t &device);
static void checkPendingAcks();
static void handleAck(const uint8_t *buf);
static void noteCommandSent(const cmd_entry_t &cmd);
static void queuePendingCommandForDevice(uint16_t targetId);
static void handleSerialCommand(String line);
static void printSerialHelp();
static void printHubSerialStatus();
static bool saveHubConfigToFlash(bool resetMode = false);
static bool extractSerialArg(const String &line, const char *key, String &out);
static uint16_t parseSerialDeviceId(String value);

// Portable mode (BLE scanning for collar find beacons)
static void handleApiBle();
static void handleApiHubMode();
static void handleApiSecurityStatus();
static void handleApiSecurityPin();
static void handleApiSecurityUnlock();
static bool requireCommandAccess();
static void handleApiHistory();
static void handleApiHistoryCsv();

// ── DIO1 Interrupt Service Routine ──
// The SX1262 fires DIO1 when a packet is fully received.
// This ISR just sets a flag — actual processing happens in loraTask.
// IRAM_ATTR keeps this function in fast RAM for minimal latency.
static void IRAM_ATTR onLoRaDio1() {
    loraPacketReceived = true;
}

// ═══════════════════════════════════════════════
// Setup
// ═══════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }  // Wait up to 3s for USB serial

    Serial.println("=================================");
    Serial.println("  Bluepaws V4 — Home Hub");
    Serial.printf("  Protocol v%d | Max %d bytes\n",
                  BP_PROTOCOL_VERSION, BP_MAX_PACKET_SIZE);
    Serial.println("=================================");

    // Create FreeRTOS synchronization primitives before any task uses them
    loraMutex    = xSemaphoreCreateMutex();   // Guards SPI radio access
    sseMutex     = xSemaphoreCreateMutex();   // Guards SSE client list
    deviceMutex  = xSemaphoreCreateMutex();   // Guards device state table
    pendingMutex = xSemaphoreCreateMutex();   // Guards pending command slots
    bleMutex     = xSemaphoreCreateMutex();   // Guards BLE scan results (portable mode)
    cmdQueue     = xQueueCreate(CMD_QUEUE_SIZE,   sizeof(cmd_entry_t));   // Web→LoRa command pipe
    cloudQueue   = xQueueCreate(CLOUD_QUEUE_SIZE, sizeof(cloud_entry_t)); // LoRa→Cloud relay pipe
    storageQueue = xQueueCreate(STORAGE_QUEUE_SIZE, sizeof(bp_journal_record_t));
    networkQueue = xQueueCreate(1, sizeof(network_request_t));
    if (!networkQueue) {
        Serial.println("[FATAL] Cannot allocate network control queue");
        delay(1000);
        ESP.restart();
    }

    // Zero out the pending command tracking slots
    memset(pendingCmds, 0, sizeof(pendingCmds));

    // Initialise hardware subsystems (order matters: storage first to load config)
    initStorage();  // Mount LittleFS, load saved WiFi/cloud config
    initLoRa();     // SPI + SX1262 radio setup, start listening
    initHubPresence(); // Own GNSS task and persisted hub preferences, never collar-derived.
    initBLE();      // BLE home beacon advertising

    // Create FreeRTOS tasks, each pinned to a specific core.
    // ESP32-S3 has 2 cores: core 0 handles WiFi/BLE, core 1 is free for LoRa.
    // Pinning LoRa to core 1 avoids WiFi interrupt contention.
    xTaskCreatePinnedToCore(loraTask,  "lora",  STACK_LORA,  NULL, PRIO_LORA,  &loraTaskHandle,  1);  // Core 1
    xTaskCreatePinnedToCore(webTask,   "web",   STACK_WEB,   NULL, PRIO_WEB,   &webTaskHandle,   1);  // Below LoRa on core 1
    if (xTaskCreatePinnedToCore(networkTask, "network", STACK_NETWORK, NULL,
                               PRIO_NETWORK, &networkTaskHandle, 0) != pdPASS) {
        Serial.println("[FATAL] Cannot start Wi-Fi/captive DNS task");
        delay(1000);
        ESP.restart();
    }
    xTaskCreatePinnedToCore(bleTask,   "ble",   STACK_BLE,   NULL, PRIO_BLE,   &bleTaskHandle,   0);  // Core 0
    xTaskCreatePinnedToCore(cloudTask, "cloud", STACK_CLOUD, NULL, PRIO_CLOUD, &cloudTaskHandle, 0);  // Core 0
    xTaskCreatePinnedToCore(storageTask, "storage", STACK_STORAGE, NULL, PRIO_STORAGE, &storageTaskHandle, 0);
    xTaskCreatePinnedToCore(consoleTask, "console", STACK_CONSOLE, NULL, PRIO_CONSOLE, &consoleTaskHandle, 0); // Core 0

    Serial.println("[INIT] All tasks started");
    printSerialHelp();
}

// Arduino loop() — does nothing. All work happens in FreeRTOS tasks.
// We just sleep forever so the scheduler can run the real tasks.
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// ═══════════════════════════════════════════════
// Hub Communications Profile / Connectivity Health
//
// Loss of STA starts a bounded recovery, then automatic Off-Grid entry.
// Leaving Off-Grid always requires confirmation. Cloud failure alone is not
// Wi-Fi loss. Only networkTask changes radio roles; BLE has its own owner.
// ═══════════════════════════════════════════════

static const char *hubCommProfileName(hub_comm_profile_t profile) {
    switch (profile) {
    case HUB_COMM_HOME: return "home";
    case HUB_COMM_PORTABLE: return "portable";
    case HUB_COMM_OFF_GRID: return "off_grid";
    default: return "home";
    }
}

static hub_comm_profile_t hubCommProfileFromString(const String &value) {
    if (value == "portable") return HUB_COMM_PORTABLE;
    if (value == "off_grid" || value == "off-grid" || value == "offline") {
        return HUB_COMM_OFF_GRID;
    }
    return HUB_COMM_HOME;
}

static bool hubProfileUsesBleScanning() {
    // Portable and Off-Grid are both search/roaming profiles. BLE scanning is
    // useful there for local Active Find proximity. Home mode advertises the
    // trusted home beacon instead.
    return hubCommProfile == HUB_COMM_PORTABLE || hubCommProfile == HUB_COMM_OFF_GRID;
}

static bool hubProfileAllowsCloudRelay() {
    // Off-Grid is an explicit local-only choice. Home and Portable may relay
    // when their STA/internet/cloud path is healthy.
    return hubCommProfile != HUB_COMM_OFF_GRID;
}

static bool hubProfileNeedsLocalAp() {
    // The Home Hub should not advertise its own AP during normal connected
    // Home operation. AP is explicit for Off-Grid/local-only and provisioning.
    // If there is no STA SSID configured at all, keep AP up as a recovery path.
    return hubCommProfile == HUB_COMM_OFF_GRID
        || hubProvisioningMode
        || staSSID.length() == 0;
}

static void setHubCommunicationsProfile(hub_comm_profile_t profile) {
    if (hubCommProfile == profile) return;

    hub_comm_profile_t previous = hubCommProfile;
    hubCommProfile = profile;
    if (previous == HUB_COMM_OFF_GRID && profile != HUB_COMM_OFF_GRID) {
        clearOfflineSessions = true; // web task owns OfflineAccess
    }
    Serial.printf("[HUB] Communications profile → %s\n", hubCommProfileName(hubCommProfile));
}

static void applyWifiRoleForCurrentProfile() {
    bool shouldEnableAp = hubProfileNeedsLocalAp();
    wifi_mode_t desiredMode = shouldEnableAp ? WIFI_AP_STA : WIFI_STA;

    if (WiFi.getMode() != desiredMode) {
        WiFi.mode(desiredMode);
    }

    if (shouldEnableAp && !hubApEnabled) {
        // Eight AP associations as well as eight SSE slots. Always-on hub:
        // retain Bluetooth-compatible modem sleep and never restart a healthy AP.
        hubApEnabled = WiFi.softAP(WIFI_AP_SSID, nullptr, WIFI_AP_CHANNEL, false, MAX_SSE_CLIENTS);
        WiFi.setSleep(true); // WIFI_PS_MIN_MODEM: required for Wi-Fi/BLE coexistence
        if (hubApEnabled) {
            Serial.printf("[WIFI] AP enabled: %s @ %s (channel %u, max clients %u)\n",
                          WIFI_AP_SSID, WiFi.softAPIP().toString().c_str(),
                          WIFI_AP_CHANNEL, MAX_SSE_CLIENTS);
        } else {
            ++apStartFailures;
            Serial.println("[WIFI] AP start failed; retry in 5 seconds");
        }
    } else if (!shouldEnableAp && hubApEnabled) {
        captiveDns.stop();
        captiveDnsRunning = false;
        WiFi.softAPdisconnect(true);
        hubApEnabled = false;
        WiFi.mode(WIFI_STA);
        Serial.println("[WIFI] AP disabled for normal Home/Portable STA mode");
    }
    if (hubApEnabled && !captiveDnsRunning) {
        captiveDnsRunning = captiveDns.start(CAPTIVE_DNS_PORT, "*", WiFi.softAPIP());
        if (!captiveDnsRunning) Serial.println("[WIFI] Captive DNS start failed; will retry");
    }
}

static void updateConnectivityState() {
    bool connected = WiFi.status() == WL_CONNECTED;
    staConnected = connected;
    hubConnectivity.wifi_connected = connected;
    // These are intentionally conservative until an active internet probe and
    // cloud health endpoint are added. A successful cloud POST promotes both.
    if (!connected) {
        hubConnectivity.internet_reachable = false;
        hubConnectivity.cloud_reachable = false;
    }
}

static void noteCloudPostResult(int httpCode) {
    if (httpCode >= 200 && httpCode < 300) {
        hubConnectivity.internet_reachable = true;
        hubConnectivity.cloud_reachable = true;
        hubConnectivity.last_cloud_success_ms = millis();
        hubConnectivity.cloud_failures = 0;
        return;
    }

    if (hubConnectivity.cloud_failures < 255) {
        hubConnectivity.cloud_failures++;
    }
    // Simple hysteresis placeholder: require repeated failed POSTs before the
    // UI should show cloud as down. Replace with an explicit probe later.
    if (hubConnectivity.cloud_failures >= 3) {
        hubConnectivity.cloud_reachable = false;
    }
}

static bool unixTimeLooksValid(time_t value) {
    // Reject unset/Unix-epoch-ish values and obviously bad future values.
    // This project is active in 2026; 2025-01-01 is a safe low watermark.
    return value >= 1735689600 && value <= 0xFFFFFFFF;
}

static void syncHubClock(bool force) {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    uint32_t nowMs = millis();
    if (!force && hubTimeSynced && (nowMs - lastNtpSyncMs) < 3600000UL) {
        return;
    }

    Serial.println("[TIME] Syncing Home Hub clock via NTP...");
    configTime(0, 0, NTP_PRIMARY, NTP_SECONDARY, NTP_TERTIARY);

    struct tm timeInfo;
    for (uint8_t attempt = 0; attempt < 20; attempt++) {
        if (getLocalTime(&timeInfo, 250)) {
            time_t synced = time(nullptr);
            if (unixTimeLooksValid(synced)) {
                hubTimeSynced = true;
                lastNtpSyncMs = millis();
                Serial.printf("[TIME] NTP synced: %lu\n", (uint32_t)synced);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    Serial.println("[TIME] NTP not ready yet; gateway wrapper will temporarily fall back to collar TLV time.");
}

// ═══════════════════════════════════════════════
// Initialisation
// ═══════════════════════════════════════════════

static void initLoRa() {
    // Start SPI bus to the SX1262 module
    loraSPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

    // Heltec Tracker V2.x routes the SX1262 through an external RF
    // front-end. Power/enable it before lora.begin(), then leave it in RX.
    femInit();

    Serial.print("[LORA] Initialising SX1262... ");
    int state = lora.begin(LORA_FREQUENCY);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("FAILED (err %d)\n", state);
        return;
    }

    // Configure radio parameters explicitly, matching the collar and the
    // proven T190 sniffer initialisation style.
    lora.setSpreadingFactor(LORA_SPREADING);
    lora.setBandwidth(LORA_BANDWIDTH);
    lora.setCodingRate(LORA_CODING_RATE);
    lora.setPreambleLength(LORA_PREAMBLE_LEN);
    lora.setSyncWord(LORA_SYNC_WORD);
    lora.setOutputPower(10);         // Hub command TX is low power for now
    lora.setCRC(LORA_CRC_ENABLED);   // Enable hardware CRC on the radio
    lora.setDio1Action(onLoRaDio1);   // Register our ISR for packet-received interrupt
    lora.startReceive();              // Put radio into continuous RX mode
    hubConnectivity.lora_rx_active = true;

    Serial.println("OK");
    Serial.println("[LORA] TLV v1.2 raw binary uplink enabled");
}

static void femInit() {
    pinMode(PIN_FEM_VCTRL, OUTPUT);
    pinMode(PIN_FEM_CSD, OUTPUT);
    pinMode(PIN_FEM_CTX, OUTPUT);

    digitalWrite(PIN_FEM_VCTRL, HIGH);  // Enable FEM LDO/power rail
    digitalWrite(PIN_FEM_CSD, HIGH);    // Enable FEM chip
    femSetRx();                         // Default to receive path
    delay(20);

    Serial.println("[FEM] KCT8103L powered + enabled; CTX=RX");
}

static void femSetRx() {
    digitalWrite(PIN_FEM_CTX, LOW);
}

static void femSetTx() {
    digitalWrite(PIN_FEM_CTX, HIGH);
}

static bool requestHubMode(hub_comm_profile_t profile, bool confirmed) {
    if (modeChangePending.exchange(true)) return false;
    network_request_t request{profile, confirmed, millis()};
    if (xQueueSend(networkQueue, &request, 0) != pdTRUE) {
        modeChangePending = false;
        return false;
    }
    return true;
}

// All application Wi-Fi mutations and captive DNS belong to this task.
// Keep HTTP handlers, NTP waits, TLS, filesystem writes and BLE scans elsewhere.
static void networkTask(void *param) {
    (void)param;
    // Config changes save and reboot; private copies avoid String races while saving.
    const String primaryName = staSSID, primaryPassword = staPass;
    const String secondaryName = secondarySSID, secondaryPassword = secondaryPass;
    const bool hasSecondary = secondaryName.length() && secondaryName != primaryName;
    WifiFailover policy(primaryName.length() > 0, hasSecondary);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false); // the policy owns retries and their deadline
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true); // minimum modem sleep is required while Bluetooth is enabled
    networkStackReady = true; // lwIP must exist before webTask binds its socket
    bool scanActive = false;
    uint32_t lastScan = millis(), lastMaintenance = millis();
    bool mdnsStarted = false;
    network_request_t pending{};
    bool hasRequest = false;

    auto perform = [&](WifiFailover::Action action) {
        using A = WifiFailover::Action;
        if (action == A::None) return;
        if (action == A::ConnectPrimary || action == A::ConnectSecondary || action == A::StartOffGrid) {
            homeBeaconAllowed = false;
            if (scanActive) {
                esp_wifi_scan_stop();
                scanActive = false;
            }
            WiFi.scanDelete();
            WiFi.disconnect(false, false); // never erase saved config or shut down AP
            knownWifiAvailable = false;
        }
        if (action == A::StartOffGrid) {
            setHubCommunicationsProfile(HUB_COMM_OFF_GRID);
            applyWifiRoleForCurrentProfile();
            lastScan = millis(); // let the hotspot settle before any idle discovery
            Serial.println("[WIFI] Off-Grid: local hotspot active; automatic reconnect stopped");
        } else if (action == A::ConnectPrimary || action == A::ConnectSecondary) {
            applyWifiRoleForCurrentProfile();
            const bool primary = action == A::ConnectPrimary;
            Serial.printf("[WIFI] Searching %s uplink (30-second total recovery budget)\n",
                          primary ? "primary" : "secondary");
            WiFi.begin(primary ? primaryName.c_str() : secondaryName.c_str(),
                       primary ? primaryPassword.c_str() : secondaryPassword.c_str());
        } else {
            const bool primary = action == A::ConnectedPrimary;
            setHubCommunicationsProfile(primary ? HUB_COMM_HOME : HUB_COMM_PORTABLE);
            homeBeaconAllowed = primary;
            applyWifiRoleForCurrentProfile();
            Serial.printf("[WIFI] %s connected at %s\n", primary ? "Primary/Home" : "Secondary/Portable",
                          WiFi.localIP().toString().c_str());
        }
    };
    perform(hubCommProfile == HUB_COMM_OFF_GRID ? policy.offGrid()
        : policy.begin(millis(), hubCommProfile == HUB_COMM_PORTABLE));

    for (;;) {
        const uint32_t now = millis();
        if (!hasRequest && xQueueReceive(networkQueue, &pending, 0) == pdTRUE) hasRequest = true;
        // Let the HTTP response leave before intentionally disconnecting its AP.
        if (hasRequest && now - pending.requestedAt >= 500) {
            if (hubCommProfile != HUB_COMM_OFF_GRID || pending.profile == HUB_COMM_OFF_GRID || pending.confirmed) {
                setHubCommunicationsProfile(pending.profile);
                perform(pending.profile == HUB_COMM_OFF_GRID ? policy.offGrid()
                    : policy.begin(now, pending.profile == HUB_COMM_PORTABLE));
            } else {
                Serial.println("[WIFI] Rejected unconfirmed Off-Grid exit");
            }
            hasRequest = false;
            modeChangePending = false;
        }
        perform(policy.tick(now, WiFi.status() == WL_CONNECTED));
        wifiPhase = policy.phase();
        wifiRecoveryRemainingMs = policy.remaining(now);
        updateConnectivityState();
        if (!staConnected) homeBeaconAllowed = false;

        // AP+STA shares one radio. Never associate or scan while local clients
        // are connected: channel hops disrupt captive portal, HTTP and SSE.
        // Idle-only discovery provides a hint, NEVER an automatic mode change.
        if (policy.phase() == WifiFailover::Phase::OffGrid && hubApEnabled) {
            const bool busy = WiFi.softAPgetStationNum() > 0;
            if (scanActive && busy) esp_wifi_scan_stop();
            if (scanActive) {
                int count = WiFi.scanComplete();
                if (count != WIFI_SCAN_RUNNING) {
                    if (!busy) {
                        bool found = false;
                        for (int i = 0; i < count; ++i) {
                            const String name = WiFi.SSID(i);
                            if (name == primaryName || (hasSecondary && name == secondaryName)) found = true;
                        }
                        knownWifiAvailable = found;
                    }
                    WiFi.scanDelete();
                    scanActive = false;
                }
            } else if (!busy && now - lastScan >= 60000 && (primaryName.length() || hasSecondary)) {
                lastScan = now;
                scanActive = WiFi.scanNetworks(true, false, false, 120) == WIFI_SCAN_RUNNING;
            }
        }
        if (hubApEnabled && captiveDnsRunning) captiveDns.processNextRequest();
        if (now - lastMaintenance >= 5000) {
            lastMaintenance = now;
            // Retry only failed AP/DNS starts. Healthy interfaces are left alone.
            if (hubProfileNeedsLocalAp() && (!hubApEnabled || !captiveDnsRunning)) applyWifiRoleForCurrentProfile();
            if (!mdnsStarted && (hubApEnabled || staConnected)) {
                mdnsStarted = MDNS.begin(MDNS_HOSTNAME);
                if (mdnsStarted) MDNS.addService("http", "tcp", HTTP_PORT);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void initBLE() {
    // The hub advertises a BLE beacon with a known name (e.g. "BluePaws_Home").
    // The collar scans for this name before each cycle — if it sees the beacon,
    // it knows the pet is home and skips GPS/LoRa to save battery.
    BLEDevice::init(BLE_HOME_BEACON_NAME);
    BLEServer *pServer = BLEDevice::createServer();
    BLEAdvertising *pAdv = BLEDevice::getAdvertising();

    BLEAdvertisementData advData;
    advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    advData.setName(BLE_HOME_BEACON_NAME);  // This is what the collar scans for
    pAdv->setAdvertisementData(advData);

    // bleTask starts/stops advertising only after primary Wi-Fi is confirmed.
    // A roaming hub must not convince collars that they are still at home.
}

static device_meta_t *findDeviceMetadata(uint16_t id, bool create) {
    for (uint8_t i = 0; i < deviceMetaCount; ++i) {
        if (deviceMeta[i].device_id == id) return &deviceMeta[i];
    }
    if (!create || id == 0 || deviceMetaCount >= MAX_DEVICES) return nullptr;
    device_meta_t *meta = &deviceMeta[deviceMetaCount++];
    memset(meta, 0, sizeof(*meta));
    meta->device_id = id;
    strlcpy(meta->name, bp_device_name(id), sizeof(meta->name));
    strlcpy(meta->emoji, "🐾", sizeof(meta->emoji));
    strlcpy(meta->colour, "#1d9bf0", sizeof(meta->colour));
    meta->local_override = false;
    return meta;
}

static const char *deviceDisplayName(uint16_t id) {
    device_meta_t *meta = findDeviceMetadata(id);
    return meta && meta->name[0] ? meta->name : bp_device_name(id);
}

static const char *deviceEmoji(uint16_t id) {
    device_meta_t *meta = findDeviceMetadata(id);
    return meta && meta->emoji[0] ? meta->emoji : "";
}

static const char *deviceColour(uint16_t id) {
    device_meta_t *meta = findDeviceMetadata(id);
    return meta && meta->colour[0] ? meta->colour : "";
}

static void loadDeviceMetadata() {
    if (!LittleFS.exists(DEVICE_META_PATH)) return;
    File file = LittleFS.open(DEVICE_META_PATH, "r");
    if (!file) return;

    JsonDocument doc;
    if (deserializeJson(doc, file) == DeserializationError::Ok) {
        for (JsonObject item : doc.as<JsonArray>()) {
            uint16_t id = item["device_id"] | 0;
            device_meta_t *meta = findDeviceMetadata(id, true);
            if (!meta) continue;
            strlcpy(meta->name, item["name"] | bp_device_name(id), sizeof(meta->name));
            strlcpy(meta->emoji, item["emoji"] | "🐾", sizeof(meta->emoji));
            strlcpy(meta->colour, item["colour"] | "#1d9bf0", sizeof(meta->colour));
            meta->local_override = item["local_override"] | false;
        }
        Serial.printf("[FS] Loaded %u local collar appearances\n", deviceMetaCount);
    } else {
        Serial.println("[FS] Ignoring invalid devices.meta cache");
    }
    file.close();
}

static bool saveDeviceMetadata() {
    JsonDocument doc;
    JsonArray items = doc.to<JsonArray>();
    for (uint8_t i = 0; i < deviceMetaCount; ++i) {
        JsonObject item = items.add<JsonObject>();
        item["device_id"] = deviceMeta[i].device_id;
        item["name"] = deviceMeta[i].name;
        item["emoji"] = deviceMeta[i].emoji;
        item["colour"] = deviceMeta[i].colour;
        item["local_override"] = deviceMeta[i].local_override;
    }
    File file = LittleFS.open(DEVICE_META_PATH, "w");
    if (!file) return false;
    bool ok = serializeJson(doc, file) > 0;
    file.close();
    return ok;
}

static void initStorage() {
    // Mount LittleFS (on-chip flash filesystem). The 'true' parameter
    // auto-formats the partition if it's not already formatted.
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed!");
        return;
    }
    Serial.println("[FS] LittleFS mounted");
    loadDeviceMetadata();

    if (!offlineJournal.begin(LittleFS)) {
        Serial.println("[FS] Offline journal unavailable");
    } else {
        Serial.printf("[FS] Offline journal: %lu valid, %u pending\n",
                      (unsigned long)offlineJournal.totalValidRecords(),
                      offlineJournal.pendingCount());

        uint16_t ids[BP_JOURNAL_MAX_DEVICES] = {};
        uint16_t idCount = offlineJournal.collectIds(ids, BP_JOURNAL_MAX_DEVICES);
        for (uint16_t i = 0; i < idCount; ++i) {
            bp_journal_record_t latest{};
            if (offlineJournal.latest(ids[i], latest)) {
                updateDeviceStateFromRecord(latest);
            }
        }
    }

    // Load saved WiFi/cloud config from a simple key=value text file.
    // This file gets written by the Settings modal in the web GUI.
    if (LittleFS.exists(CONFIG_FILE_PATH)) {
        File f = LittleFS.open(CONFIG_FILE_PATH, "r");
        if (f) {
            String line;
            while (f.available()) {
                line = f.readStringUntil('\n');
                line.trim();
                int eq = line.indexOf('=');
                if (eq < 0) continue;
                String key = line.substring(0, eq);
                String val = line.substring(eq + 1);
                if (key == "sta_ssid")  staSSID = val;        // Home WiFi name
                if (key == "sta_pass")  staPass = val;        // Home WiFi password
                if (key == "secondary_ssid") secondarySSID = val;
                if (key == "secondary_pass") secondaryPass = val;
                if (key == "cloud_url")   cloudEndpoint = val;  // Cloud POST endpoint
                if (key == "cloud_token") cloudToken = val;     // Gateway bearer token
                if (key == "hub_mode")    hubCommProfile = hubCommProfileFromString(val);
                if (key == "provisioning") hubProvisioningMode = (val == "1" || val == "true");
            }
            f.close();
            Serial.println("[FS] Config loaded");
        }
    }
}

// ═══════════════════════════════════════════════
// LoRa Task — RX & TX (runs on core 1)
//
// This is the most important task. It runs in a tight loop:
//   1. Listen for a raw TLV packet using a short timed receive window.
//      This deliberately avoids depending solely on DIO1 interrupts during
//      hardware bring-up, because a wrong DIO pin can otherwise look like
//      "RX ready but no packets".
//   2. Dispatch received raw TLV packets to handlePacket()
//   3. Check the command queue for outgoing commands from the web GUI
//   4. Check for timed-out pending commands that need retrying
// ═══════════════════════════════════════════════

static void loraTask(void *param) {
    (void)param;
    uint8_t rxBuf[BP_MAX_PACKET_SIZE];
    TickType_t lastCmdTx = 0;  // Timestamp of last command TX (for rate-limiting)

    for (;;) {
        // ── Step 1: Listen for received LoRa packet ──
        // Use the same continuous-RX/DIO/readData pattern as the proven T190
        // sniffer. Timed receive polling can leave SX126x state transitions in
        // awkward places on this board.
        if (loraPacketReceived && xSemaphoreTake(loraMutex, pdMS_TO_TICKS(100))) {
            loraPacketReceived = false;
            size_t len = lora.getPacketLength(false);
            int state = RADIOLIB_ERR_PACKET_TOO_LONG;
            if (len > 0 && len <= BP_MAX_PACKET_SIZE) {
                state = lora.readData(rxBuf, len);
            }
            int16_t rssi = lora.getRSSI();  // Signal strength
            float snr = lora.getSNR();      // Signal-to-noise ratio
            lora.startReceive();            // Always re-arm continuous RX
            xSemaphoreGive(loraMutex);

            if (state == RADIOLIB_ERR_NONE && len > 0 && len <= BP_MAX_PACKET_SIZE) {
                handlePacket(rxBuf, (uint8_t)len, rssi, snr);
            } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
                Serial.println("[LORA] RX CRC mismatch");
            } else {
                Serial.printf("[LORA] RX read error: state=%d len=%u\n", state, (unsigned)len);
            }
        }

        // ── Step 2: Process outgoing commands from the web GUI ──
        // Commands are rate-limited (CMD_QUEUE_INTERVAL_MS between transmissions)
        // to avoid flooding the LoRa channel.
        cmd_entry_t cmd;
        TickType_t now = xTaskGetTickCount();
        bool collarRxOpportunity = commandRxOpportunityUntil != 0 &&
                                   (int32_t)(commandRxOpportunityUntil - now) > 0;
        if ((now - lastCmdTx) >= pdMS_TO_TICKS(CMD_QUEUE_INTERVAL_MS) || collarRxOpportunity) {
            if (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
                // Superseded/ACKed/expired queue entries must never transmit later.
                if (!commandStillPending(cmd)) continue;
                // Take SPI mutex, transmit, then go back to RX mode
                if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(200))) {
                    femSetTx();
                    int state = lora.transmit(cmd.buf, cmd.len);
                    femSetRx();
                    lora.startReceive();  // Always return to RX after TX
                    xSemaphoreGive(loraMutex);

                    if (state == RADIOLIB_ERR_NONE) {
                        txCount++;
                        noteCommandSent(cmd);
                        Serial.printf("[LORA] CMD TX %d bytes seq=%u -> %s\n",
                                      cmd.len, cmd.cmdSeq, bp_device_name(cmd.targetId));
                    } else {
                        Serial.printf("[LORA] TX failed: %d\n", state);
                    }
                    lastCmdTx = now;
                    if (collarRxOpportunity) {
                        commandRxOpportunityUntil = 0;
                    }
                }
            }
        }

        // ── Step 3: Check for timed-out pending commands ──
        // If a command hasn't been ACK'd within timeout, retransmit or expire it.
        checkPendingAcks();

        // Sleep 10ms to yield CPU — this loop runs ~100 times/second
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ═══════════════════════════════════════════════
// Command ACK Tracking
//
// When the hub sends a command (mode change, find, etc.), it stores
// the command in pendingCmds[] and waits for the collar to ACK it.
// The collar includes TLV_CMD_MSG_ID in its ACK packet so we can
// match it back to the original command.
// ═══════════════════════════════════════════════

// Called when we receive an ACK packet from a collar.
// Looks up the original command by sequence number and marks it done.
static void handleAck(const uint8_t *buf) {
    // The collar puts the original command's msg_seq in TLV_CMD_MSG_ID
    uint32_t ackedSeq = 0;
    uint16_t ackedSeq16 = 0;
    if (pkt_tlv_get_u16(buf, TLV_CMD_MSG_ID, &ackedSeq16)) {
        ackedSeq = ackedSeq16;
    } else if (!pkt_tlv_get_u32(buf, TLV_CMD_MSG_ID, &ackedSeq)) {
        return;
    }

    if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(50))) {
        for (int i = 0; i < MAX_PENDING_CMDS; i++) {
            if (pendingCmds[i].active && pendingCmds[i].cmdSeq == ackedSeq
                && pendingCmds[i].targetId == pkt_device_id(buf)
                && millis() - pendingCmds[i].createdAtMs < LOCAL_COMMAND_TTL_MS) {
                // Found the matching command — calculate round-trip time
                uint32_t rtt = millis() - pendingCmds[i].sentAtMs;
                Serial.printf("[ACK] Cmd seq %lu ACK'd by %s (RTT %lums)\n",
                              ackedSeq, bp_device_name(pkt_device_id(buf)), rtt);
                pendingCmds[i].active = false;  // Retain bounded feedback, stop delivery
                pendingCmds[i].state = "acked";
                broadcastCommand(pendingCmds[i]);
                break;
            }
        }
        xSemaphoreGive(pendingMutex);
    }
}

static void noteCommandSent(const cmd_entry_t &cmd) {
    if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(20))) {
        for (int i = 0; i < MAX_PENDING_CMDS; i++) {
            if (pendingCmds[i].active && pendingCmds[i].cmdSeq == cmd.cmdSeq
                && pendingCmds[i].targetId == cmd.targetId) {
                pendingCmds[i].sentAtMs = millis();
                pendingCmds[i].state = "transmitted";
                broadcastCommand(pendingCmds[i]);
                break;
            }
        }
        xSemaphoreGive(pendingMutex);
    }
}

// All readers/writers of command feedback take pendingMutex. Do not expose
// packet bytes or credentials. Age is monotonic, independent of NTP changes.
static void commandJson(const pending_cmd_t &cmd, char *out, size_t size) {
    uint8_t profile = PROFILE_UNKNOWN;
    pkt_tlv_get_u8(cmd.buf, TLV_PROFILE, &profile);
    uint32_t age = millis() - cmd.createdAtMs;
    const char *state = cmd.active && age >= LOCAL_COMMAND_TTL_MS ? "expired" : cmd.state;
    snprintf(out, size,
        "{\"device\":%u,\"cmdSeq\":%lu,\"status\":\"%s\",\"type\":\"%s\","
        "\"profile\":\"%s\",\"age_ms\":%lu,\"expires_in_seconds\":%lu}",
        cmd.targetId, (unsigned long)cmd.cmdSeq, state ? state : "queued",
        cmd.type == PKT_CMD_MODE ? "profile" : cmd.type == PKT_CMD_FIND ? "find" : "status",
        bp_profile_name((bp_profile_t)profile), (unsigned long)age,
        (unsigned long)(age < LOCAL_COMMAND_TTL_MS ? (LOCAL_COMMAND_TTL_MS - age) / 1000 : 0));
}

static void broadcastCommand(const pending_cmd_t &cmd) {
    char json[256];
    commandJson(cmd, json, sizeof(json));
    sseBroadcast("cmd_ack", json);
}

static bool commandStillPending(const cmd_entry_t &cmd) {
    bool valid = false;
    if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(20))) {
        for (const auto &p : pendingCmds) {
            if (p.active && p.cmdSeq == cmd.cmdSeq && p.targetId == cmd.targetId
                && millis() - p.createdAtMs < LOCAL_COMMAND_TTL_MS) valid = true;
        }
        xSemaphoreGive(pendingMutex);
    }
    return valid;
}

static void handleApiCommands() {
    if (!xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(50))) {
        httpServer.send(503, "application/json", "{\"error\":\"busy\"}");
        return;
    }
    String json = "[";
    for (const auto &cmd : pendingCmds) {
        if (!cmd.state || millis() - cmd.createdAtMs >= COMMAND_FEEDBACK_TTL_MS) continue;
        char item[256];
        commandJson(cmd, item, sizeof(item));
        if (json.length() > 1) json += ',';
        json += item;
    }
    xSemaphoreGive(pendingMutex);
    json += ']';
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send(200, "application/json", json);
}

static void queuePendingCommandForDevice(uint16_t targetId) {
    if (targetId == DEVICE_ID_HUB || targetId == DEVICE_ID_BROADCAST) return;

    if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(20))) {
        for (int i = 0; i < MAX_PENDING_CMDS; i++) {
            if (!pendingCmds[i].active || pendingCmds[i].targetId != targetId) continue;
            if (millis() - pendingCmds[i].createdAtMs >= LOCAL_COMMAND_TTL_MS) continue;

            pendingCmds[i].retries = 0;
            pendingCmds[i].sentAtMs = 0;
            pendingCmds[i].waitingOpportunity = false;

            cmd_entry_t cmd;
            memcpy(cmd.buf, pendingCmds[i].buf, pendingCmds[i].len);
            cmd.len = pendingCmds[i].len;
            cmd.targetId = pendingCmds[i].targetId;
            cmd.cmdSeq = (uint16_t)(pendingCmds[i].cmdSeq & 0xFFFF);
            cmd.type = pendingCmds[i].type;

            commandRxOpportunityUntil = xTaskGetTickCount() + pdMS_TO_TICKS(8000);
            if (xQueueSendToFront(cmdQueue, &cmd, 0) == pdTRUE) {
                Serial.printf("[CMD] RX window opportunity: queued seq %u -> %s\n",
                              cmd.cmdSeq, bp_device_name(targetId));
            } else {
                Serial.printf("[CMD] RX window opportunity but command queue full for %s\n",
                              bp_device_name(targetId));
            }
            break;
        }
        xSemaphoreGive(pendingMutex);
    }
}

// Called every loop iteration by loraTask.
// Checks if any pending commands have timed out waiting for ACK.
// Retry up to CMD_MAX_RETRIES times, then wait for the next RX opportunity.
static void checkPendingAcks() {
    if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(20))) {
        uint32_t now = millis();
        for (int i = 0; i < MAX_PENDING_CMDS; i++) {
            if (!pendingCmds[i].active) continue;                      // Skip unused slots
            if (now - pendingCmds[i].createdAtMs >= LOCAL_COMMAND_TTL_MS) {
                pendingCmds[i].active = false;
                pendingCmds[i].state = "expired";
                broadcastCommand(pendingCmds[i]);
                continue;
            }
            if (pendingCmds[i].waitingOpportunity) continue;
            if (pendingCmds[i].sentAtMs == 0) continue;                 // Queued but not TX-confirmed yet
            if (now - pendingCmds[i].sentAtMs < CMD_ACK_TIMEOUT_MS) continue;  // Not timed out yet

            if (pendingCmds[i].retries < CMD_MAX_RETRIES) {
                // Haven't exhausted retries — re-queue the packet for TX
                pendingCmds[i].retries++;
                pendingCmds[i].sentAtMs = 0;  // Timeout restarts when TX is confirmed

                cmd_entry_t cmd;
                memcpy(cmd.buf, pendingCmds[i].buf, pendingCmds[i].len);
                cmd.len = pendingCmds[i].len;
                cmd.targetId = pendingCmds[i].targetId;
                cmd.cmdSeq = (uint16_t)(pendingCmds[i].cmdSeq & 0xFFFF);
                cmd.type = pendingCmds[i].type;
                if (xQueueSend(cmdQueue, &cmd, 0) != pdTRUE) {
                    pendingCmds[i].waitingOpportunity = true;
                    pendingCmds[i].state = "waiting";
                    broadcastCommand(pendingCmds[i]);
                    continue;
                }

                Serial.printf("[ACK] Retry %d/%d for seq %lu → %s\n",
                              pendingCmds[i].retries, CMD_MAX_RETRIES,
                              pendingCmds[i].cmdSeq,
                              bp_device_name(pendingCmds[i].targetId));
            } else {
                // Blind retries exhausted. Retain until the collar's next RX
                // opportunity or the ten-minute local command TTL.
                Serial.printf("[ACK] Waiting for next RX opportunity seq %lu → %s\n",
                              pendingCmds[i].cmdSeq,
                              bp_device_name(pendingCmds[i].targetId));

                pendingCmds[i].state = "waiting";
                broadcastCommand(pendingCmds[i]);
                pendingCmds[i].waitingOpportunity = true;
            }
        }
        xSemaphoreGive(pendingMutex);
    }
}

// ═══════════════════════════════════════════════
// Packet Handler — the central dispatch for every received packet
//
// Pipeline: structure check → version check → ACK matching → update state
//           → log to flash → queue for cloud → broadcast to web GUI
// ═══════════════════════════════════════════════

static void handlePacket(const uint8_t *buf, uint8_t len, int16_t rssi, float snr) {
    // Step 1: Validate TLV v1.2 structure. Supabase validates HMAC.
    if (!pkt_validate_crc(buf, len)) {
        crcFailCount++;
        Serial.printf("[LORA] TLV structure fail #%u (len=%u)\n", crcFailCount, len);
        return;
    }

    // Step 2: Reject packets from incompatible protocol versions
    if (pkt_version(buf) != BP_PROTOCOL_VERSION) {
        Serial.printf("[LORA] Version mismatch: %d\n", pkt_version(buf));
        return;
    }

    rxCount++;
    uint16_t devId = pkt_source_id(buf);
    uint16_t destinationId = pkt_destination_id(buf);
    uint8_t pktType = pkt_pkt_type(buf);

    if (!bp_is_collar_id(devId)) {
        Serial.printf("[LORA] Ignoring non-collar source 0x%04X\n", devId);
        return;
    }
    if (destinationId != BP_DEST_CLOUD
        && destinationId != (uint16_t)GATEWAY_GUID16
        && destinationId != BP_ID_BROADCAST) {
        Serial.printf("[LORA] Packet for another destination 0x%04X\n", destinationId);
        return;
    }

    Serial.printf("[LORA] RX #%u source=%04X destination=%04X | type=0x%02X rssi=%d snr=%.1f\n",
                  rxCount, devId, destinationId, pktType, rssi, snr);

    // Step 3: If this is an ACK/response to a command we sent, match it up
    if ((pktType == PKT_MODE_ACK || pktType == PKT_FIND_ACK || pktType == PKT_STATUS_RESP)
        && destinationId == (uint16_t)GATEWAY_GUID16) {
        handleAck(buf);
    }

    // Step 4: Assign a monotonic local journal identity and update RAM now.
    // Flash writes happen on storageTask, never on the time-sensitive LoRa task.
    time_t rxTime = time(nullptr);
    uint32_t gatewayRxTime = unixTimeLooksValid(rxTime)
        ? (uint32_t)rxTime
        : pkt_time_unix(buf);
    bp_journal_record_t record{};
    record.local_id = offlineJournal.nextLocalId();
    record.source_id = devId;
    record.gateway_rx_time_unix = gatewayRxTime;
    record.rssi_dbm = rssi;
    record.snr_x10 = (int16_t)roundf(snr * 10.0f);
    record.packet_len = len;
    record.sync_state = BP_JOURNAL_PENDING;
    memcpy(record.packet, buf, len);
    OfflineJournal::seal(record);
    updateDeviceStateFromRecord(record, true);

    if (xQueueSend(storageQueue, &record, 0) != pdTRUE) {
        Serial.printf("[FS] Journal queue full; local packet %lu remains RAM-only\n",
                      (unsigned long)record.local_id);
    }

    // Step 7: Push the telemetry as JSON to all connected web browsers via SSE
    char jsonBuf[768];
    buildDeviceJson(buf, rssi, snr, record.local_id, record.gateway_rx_time_unix,
                    record.sync_state, jsonBuf, sizeof(jsonBuf));
    sseBroadcast("telemetry", jsonBuf);

    // Step 8: A valid uplink means the collar should now be in its short RX
    // command window. If a bench command is pending for this collar, push it to
    // the front of the LoRa TX queue immediately rather than waiting for blind
    // retry timing.
    queuePendingCommandForDevice(devId);
}

// Build a JSON string from a raw packet for the web GUI.
// This JSON gets pushed via SSE ("telemetry" event) and also used
// in the /api/devices endpoint. The browser parses it to update
// the device card and map marker.
static void buildDeviceJson(const uint8_t *buf, int16_t rssi, float snr,
                             uint32_t localId, uint32_t gatewayRxTime,
                             uint8_t syncState, char *out, size_t outLen) {
    uint16_t devId    = pkt_device_id(buf);
    uint16_t flags    = pkt_flags(buf);
    bool hasGps       = (flags & FLAG_HAS_GPS) != 0;
    double lat        = hasGps ? pkt_lat_e7(buf) / 1e7 : 0.0;  // Convert from integer×10^7 to degrees
    double lon        = hasGps ? pkt_lon_e7(buf) / 1e7 : 0.0;
    uint8_t profile   = pkt_power_profile(buf);
    uint8_t resetReason = 0;
    bool resetReasonPresent = pkt_tlv_get_u8(buf, TLV_RESET_REASON, &resetReason);

    snprintf(out, outLen,
        "{\"id\":%u,\"name\":\"%s\",\"emoji\":\"%s\",\"colour\":\"%s\",\"seq\":%u,\"time\":%u,"
        "\"status\":\"%s\",\"profile\":\"%s\",\"errorPresent\":%s,\"resetReason\":%u,\"rxWindowMs\":10000,"
        "\"flags\":%u,\"txReasonCode\":%u,\"resetReasonPresent\":%s,"
        "\"lat\":%.7f,\"lon\":%.7f,\"hasGps\":%s,"
        "\"batt\":%u,\"acc\":%u,\"fixAge\":%u,"
        "\"rssi\":%d,\"snr\":%.1f,"
        "\"bleHome\":%s,\"cellular\":false,\"txReason\":\"%s\","
        "\"localId\":%lu,\"gatewayRxTime\":%lu,\"verification\":\"%s\"}",
        devId, deviceDisplayName(devId), deviceEmoji(devId), deviceColour(devId),
        pkt_msg_seq(buf), pkt_time_unix(buf),
        bp_status_display((bp_status_t)pkt_status(buf)),
        bp_profile_name((bp_profile_t)profile),
        (flags & FLAG_ERROR_PRESENT) ? "true" : "false", resetReason,
        flags, pkt_tx_reason(buf), resetReasonPresent ? "true" : "false",
        lat, lon, hasGps ? "true" : "false",
        pkt_batt_mV(buf), pkt_acc_m(buf), pkt_fix_age_s(buf),
        rssi, snr,
        (flags & FLAG_HOME_BEACON_SEEN) ? "true" : "false",
        bp_tx_reason_display(pkt_tx_reason(buf)),
        (unsigned long)localId, (unsigned long)gatewayRxTime,
        journalSyncName(syncState)
    );
}

// Update our in-memory device state table with the latest packet data.
// This is what the web GUI reads when a new browser connects.
static void updateDeviceStateFromRecord(const bp_journal_record_t &record, bool live) {
    const uint8_t *buf = record.packet;
    uint16_t devId = pkt_device_id(buf);
    uint16_t flags = pkt_flags(buf);

    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(50))) {
        // Look up existing device, or allocate a new slot
        device_state_t *dev = findDevice(devId);
        if (!dev && deviceCount < MAX_DEVICES) {
            dev = &devices[deviceCount++];
            dev->device_id = devId;
        }
        if (dev) {
            // Overwrite with latest values from this packet
            dev->last_seq    = pkt_msg_seq(buf);
            dev->last_time   = pkt_time_unix(buf);
            dev->status      = pkt_status(buf);
            dev->batt_mV     = pkt_batt_mV(buf);
            dev->acc_m       = pkt_acc_m(buf);
            dev->fix_age_s   = pkt_fix_age_s(buf);
            dev->rssi        = record.rssi_dbm;
            dev->snr         = record.snr_x10 / 10.0f;
            dev->local_millis = millis();  // Record when WE received it (for "last seen" age)
            dev->heard_this_boot = live;
            dev->gateway_rx_time_unix = record.gateway_rx_time_unix;
            dev->local_id = record.local_id;
            dev->sync_state = record.sync_state;
            dev->has_gps     = (flags & FLAG_HAS_GPS) != 0;
            if (dev->has_gps) {
                dev->lat_e7 = pkt_lat_e7(buf);
                dev->lon_e7 = pkt_lon_e7(buf);
            }
            dev->profile = pkt_power_profile(buf);
            dev->error_present = (flags & FLAG_ERROR_PRESENT) != 0;
            dev->flags = flags;
            dev->tx_reason = pkt_tx_reason(buf);
            dev->reset_reason = 0;
            dev->reset_reason_present = pkt_tlv_get_u8(buf, TLV_RESET_REASON, &dev->reset_reason);
        }
        xSemaphoreGive(deviceMutex);
    }
}

// Linear search through device table by ID. Returns NULL if not found.
static device_state_t *findDevice(uint16_t id) {
    for (uint8_t i = 0; i < deviceCount; i++) {
        if (devices[i].device_id == id) return &devices[i];
    }
    return NULL;
}

// Storage task owns LittleFS journal writes. Once durable, a packet may enter
// the live cloud queue; Off-Grid packets remain pending for batch replay.
static void storageTask(void *param) {
    (void)param;
    bp_journal_record_t record{};
    for (;;) {
        if (xQueueReceive(storageQueue, &record, portMAX_DELAY) != pdTRUE) continue;
        if (!offlineJournal.append(record)) {
            Serial.printf("[FS] Failed journal append local_id=%lu\n",
                          (unsigned long)record.local_id);
            continue;
        }

        if (hubProfileAllowsCloudRelay()) {
            cloud_entry_t ce{};
            memcpy(ce.buf, record.packet, record.packet_len);
            ce.len = record.packet_len;
            ce.rssi = record.rssi_dbm;
            ce.snr = record.snr_x10 / 10.0f;
            ce.gateway_rx_time_unix = record.gateway_rx_time_unix;
            ce.local_id = record.local_id;
            ce.source_id = record.source_id;
            xQueueSend(cloudQueue, &ce, 0);
        }
    }
}

// ═══════════════════════════════════════════════
// SSE (Server-Sent Events) Push
//
// SSE is a simple HTTP-based protocol for server→client push.
// The browser opens GET /events and receives a persistent stream.
// Each message is formatted as:
//   event: <type>\n
//   data: <json>\n
//   \n
// This function sends a message to ALL connected SSE clients.
// Disconnected clients are automatically cleaned up.
// ═══════════════════════════════════════════════

static void sseBroadcast(const char *event, const char *data) {
    if (xSemaphoreTake(sseMutex, pdMS_TO_TICKS(50))) {
        // Iterate backwards so we can safely remove disconnected clients
        for (int i = sseClientCount - 1; i >= 0; i--) {
            // A reset socket can still report connected until a write fails.
            // Drop failed/partial streams rather than retrying the same dead fd.
            const size_t expected = strlen(event) + strlen(data) + 16;
            const bool delivered = sseClients[i].connected()
                && sseClients[i].printf("event: %s\ndata: %s\n\n", event, data) == expected;
            if (!delivered) {
                sseClients[i].stop();
                // Client disconnected — shift remaining clients down to fill the gap
                for (int j = i; j < sseClientCount - 1; j++) {
                    sseClients[j] = sseClients[j + 1];
                }
                sseClientCount--;
            }
        }
        xSemaphoreGive(sseMutex);
    }
}

// ═══════════════════════════════════════════════
// Web Task — HTTP + SSE
//
// The hub serves a single-page web app from LittleFS (flash storage).
// Files: index.html, style.css, app.js (uploaded via PlatformIO).
// API endpoints handle device queries and commands.
// SSE endpoint pushes real-time telemetry to the browser.
// ═══════════════════════════════════════════════

// Only AP-side HTTP requests belong to the captive portal. Never redirect
// ordinary LAN API traffic or reflect an arbitrary Host header into Location.
static bool isCaptivePortalClient() {
    return hubApEnabled && httpServer.client().localIP() == WiFi.softAPIP();
}

static bool hasForeignPortalHost() {
    String host = httpServer.hostHeader();
    host.toLowerCase();
    if (host.endsWith(":80")) host.remove(host.length() - 3);
    if (host.endsWith(".")) host.remove(host.length() - 1);
    return host.length() && host != WiFi.softAPIP().toString()
        && host != String(MDNS_HOSTNAME) + ".local";
}

static void handleCaptiveProbe() {
    if (!isCaptivePortalClient()) {
        httpServer.send(404, "text/plain", "Not found");
        return;
    }
    String target = "http://" + WiFi.softAPIP().toString() + "/welcome";
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.sendHeader("Location", target, true);
    httpServer.send(302, "text/html", "<a href=\"" + target + "\">Welcome to Bluepaws Home Hub</a>");
}

// A small entry page for OS sign-in windows; the full dashboard stays at /.
// Do not fake Internet validation or attempt to force another application open.
static void handleWelcome() {
    if (hasForeignPortalHost()) { handleCaptiveProbe(); return; }
    httpServer.sendHeader("Cache-Control", "no-store");
    File file = LittleFS.open("/welcome.html", "r");
    if (!file) {
        httpServer.send(200, "text/html", "<h1>Welcome to Bluepaws</h1>"
            "<p>Internet is not required. Stay connected to the hub Wi-Fi.</p>"
            "<p><a href=\"/\">Open tracking dashboard</a></p>");
        return;
    }
    httpServer.streamFile(file, "text/html; charset=utf-8");
    file.close();
}

// Small read-only snapshot: no location, credentials, names, commands or SSE.
static void handleApiWelcome() {
    httpServer.sendHeader("Cache-Control", "no-store");
    if (!isCaptivePortalClient()) {
        httpServer.send(404, "application/json", "{\"error\":\"not_found\"}");
        return;
    }
    if (!xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(50))) {
        httpServer.send(503, "application/json", "{\"error\":\"busy\"}");
        return;
    }
    uint8_t recent = 0;
    uint32_t lastAge = UINT32_MAX;
    for (uint8_t i = 0; i < deviceCount; ++i) {
        const uint32_t age = deviceAgeSeconds(devices[i]);
        if (age < 600) ++recent;
        if (age < lastAge) lastAge = age;
    }
    const uint8_t known = deviceCount;
    xSemaphoreGive(deviceMutex);
    char gateway[5];
    snprintf(gateway, sizeof(gateway), "%04X", (uint16_t)GATEWAY_GUID16);
    JsonDocument doc;
    doc["hub_id"] = gateway;
    doc["recent_collars"] = recent;
    doc["known_collars"] = known;
    if (lastAge != UINT32_MAX) doc["last_report_age_s"] = lastAge;
    else doc["last_report_age_s"] = nullptr;
    doc["time_synced"] = hubTimeSynced;
    String json;
    serializeJson(doc, json);
    httpServer.send(200, "application/json", json);
}

static void handleFavicon() {
    File f = LittleFS.open("/favicon.svg", "r");
    if (!f) { httpServer.send(204, "image/svg+xml", ""); return; }
    httpServer.streamFile(f, "image/svg+xml");
    f.close();
}

// Serve the main HTML page from flash
static void handleRoot() {
    if (isCaptivePortalClient() && hasForeignPortalHost()) {
        handleCaptiveProbe();
        return;
    }
    httpServer.sendHeader("Cache-Control", "no-store");
    File f = LittleFS.open("/index.html", "r");
    if (f) {
        httpServer.streamFile(f, "text/html");
        f.close();
    } else {
        // Fallback if web files haven't been uploaded yet
        httpServer.send(200, "text/html",
            "<h1>Bluepaws V4 Hub</h1><p>Upload web files to LittleFS.</p>");
    }
}

// Serve CSS stylesheet from flash
static void handleCSS() {
    httpServer.sendHeader("Cache-Control", "no-store");
    File f = LittleFS.open("/style.css", "r");
    if (f) {
        httpServer.streamFile(f, "text/css");
        f.close();
    } else {
        httpServer.send(404, "text/plain", "Not found");
    }
}

// Serve JavaScript app from flash
static void handleJS() {
    httpServer.sendHeader("Cache-Control", "no-store");
    File f = LittleFS.open("/app.js", "r");
    if (f) {
        httpServer.streamFile(f, "application/javascript");
        f.close();
    } else {
        httpServer.send(404, "text/plain", "Not found");
    }
}

// ── SSE Endpoint: GET /events ──
// The browser connects here and keeps the connection open.
// We send HTTP headers for SSE, then immediately push the current
// state of all known devices as an initial snapshot. After that,
// sseBroadcast() pushes live updates as they arrive from LoRa.
static void handleEvents() {
    WiFiClient client = httpServer.client();

    // Register this client; additional browsers use slow polling.
    if (xSemaphoreTake(sseMutex, pdMS_TO_TICKS(100))) {
        if (sseClientCount < MAX_SSE_CLIENTS) {
            sseClients[sseClientCount++] = client;
            xSemaphoreGive(sseMutex);
        } else {
            xSemaphoreGive(sseMutex);
            httpServer.send(503, "text/plain", "Too many clients");
            return;
        }
    }

    // Send SSE response headers (this is NOT a normal HTTP response —
    // the connection stays open and we keep writing events to it)
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/event-stream");
    client.println("Cache-Control: no-cache");
    client.println("Connection: keep-alive");
    client.println("Access-Control-Allow-Origin: *");
    client.println();
    client.flush();

    // Send a snapshot of all currently-known devices so the browser
    // can populate the map immediately without waiting for the next
    // LoRa packet to arrive.
    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(100))) {
        for (uint8_t i = 0; i < deviceCount; i++) {
            device_state_t *d = &devices[i];
            char json[768];
            snprintf(json, sizeof(json),
                "{\"id\":%u,\"name\":\"%s\",\"emoji\":\"%s\",\"colour\":\"%s\",\"seq\":%u,\"time\":%u,"
                "\"status\":\"%s\",\"profile\":\"%s\",\"errorPresent\":%s,\"resetReason\":%u,\"rxWindowMs\":%lu,"
                "\"flags\":%u,\"txReasonCode\":%u,\"resetReasonPresent\":%s,"
                "\"lat\":%.7f,\"lon\":%.7f,\"hasGps\":%s,"
                "\"batt\":%u,\"acc\":%u,\"fixAge\":%u,"
                "\"rssi\":%d,\"snr\":%.1f,\"bleHome\":false,\"cellular\":false,"
                "\"age\":%lu,\"stale\":%s,\"localId\":%lu,"
                "\"gatewayRxTime\":%lu,\"verification\":\"%s\"}",
                d->device_id, deviceDisplayName(d->device_id),
                deviceEmoji(d->device_id), deviceColour(d->device_id),
                d->last_seq, d->last_time,
                bp_status_display((bp_status_t)d->status),
                bp_profile_name((bp_profile_t)d->profile),
                d->error_present ? "true" : "false", d->reset_reason,
                (unsigned long)deviceRxWindowMs(*d),
                d->flags, d->tx_reason, d->reset_reason_present ? "true" : "false",
                d->has_gps ? d->lat_e7 / 1e7 : 0.0,
                d->has_gps ? d->lon_e7 / 1e7 : 0.0,
                d->has_gps ? "true" : "false",
                d->batt_mV, d->acc_m, d->fix_age_s,
                d->rssi, d->snr,
                (unsigned long)deviceAgeSeconds(*d),
                deviceAgeSeconds(*d) >= 600 ? "true" : "false",
                (unsigned long)d->local_id,
                (unsigned long)d->gateway_rx_time_unix,
                journalSyncName(d->sync_state)
            );
            client.printf("event: telemetry\ndata: %s\n\n", json);
        }
        xSemaphoreGive(deviceMutex);
    }
}

// ── API: GET /api/devices ──
// Returns a JSON array of all known devices and their latest telemetry.
// Called by the browser on initial page load (before SSE catches up).
static void handleApiDevices() {
    String json = "[";
    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(100))) {
        for (uint8_t i = 0; i < deviceCount; i++) {
            device_state_t *d = &devices[i];
            if (i > 0) json += ",";
            char buf[768];
            snprintf(buf, sizeof(buf),
                "{\"id\":%u,\"name\":\"%s\",\"emoji\":\"%s\",\"colour\":\"%s\",\"seq\":%u,\"time\":%u,"
                "\"status\":\"%s\",\"profile\":\"%s\",\"errorPresent\":%s,\"resetReason\":%u,\"rxWindowMs\":%lu,"
                "\"flags\":%u,\"txReasonCode\":%u,\"resetReasonPresent\":%s,"
                "\"lat\":%.7f,\"lon\":%.7f,\"hasGps\":%s,"
                "\"batt\":%u,\"acc\":%u,\"fixAge\":%u,"
                "\"rssi\":%d,\"snr\":%.1f,\"age\":%lu,\"stale\":%s,"
                "\"localId\":%lu,\"gatewayRxTime\":%lu,\"verification\":\"%s\"}",
                d->device_id, deviceDisplayName(d->device_id),
                deviceEmoji(d->device_id), deviceColour(d->device_id),
                d->last_seq, d->last_time,
                bp_status_display((bp_status_t)d->status),
                bp_profile_name((bp_profile_t)d->profile),
                d->error_present ? "true" : "false", d->reset_reason,
                (unsigned long)deviceRxWindowMs(*d),
                d->flags, d->tx_reason, d->reset_reason_present ? "true" : "false",
                d->has_gps ? d->lat_e7 / 1e7 : 0.0,
                d->has_gps ? d->lon_e7 / 1e7 : 0.0,
                d->has_gps ? "true" : "false",
                d->batt_mV, d->acc_m, d->fix_age_s,
                d->rssi, d->snr,
                (unsigned long)deviceAgeSeconds(*d),
                deviceAgeSeconds(*d) >= 600 ? "true" : "false",
                (unsigned long)d->local_id,
                (unsigned long)d->gateway_rx_time_unix,
                journalSyncName(d->sync_state)
            );
            json += buf;
        }
        xSemaphoreGive(deviceMutex);
    }
    json += "]";
    httpServer.send(200, "application/json", json);
}

static const char *journalSyncName(uint8_t state) {
    if (state == BP_JOURNAL_VALIDATED) return "validated";
    if (state == BP_JOURNAL_REJECTED) return "rejected";
    return "pending";
}

static uint32_t deviceAgeSeconds(const device_state_t &device) {
    time_t now = time(nullptr);
    if (unixTimeLooksValid(now) && unixTimeLooksValid(device.gateway_rx_time_unix)
        && (uint32_t)now >= device.gateway_rx_time_unix) {
        return (uint32_t)now - device.gateway_rx_time_unix;
    }
    return (millis() - device.local_millis) / 1000;
}

static uint32_t deviceRxWindowMs(const device_state_t &device) {
    uint32_t age = millis() - device.local_millis;
    return device.heard_this_boot && age < 10000 ? 10000 - age : 0;
}

static void appendHistoryJson(String &json, const bp_journal_record_t &record) {
    const uint8_t *packet = record.packet;
    uint16_t flags = pkt_flags(packet);
    bool hasGps = (flags & FLAG_HAS_GPS) != 0;
    char item[640];
    snprintf(item, sizeof(item),
        "{\"local_id\":%lu,\"device_id\":%u,\"sequence\":%u,"
        "\"gateway_rx_time_unix\":%lu,\"collar_time_unix\":%u,"
        "\"status\":\"%s\",\"profile\":\"%s\",\"tx_reason\":\"%s\","
        "\"has_gps\":%s,\"latitude\":%.7f,\"longitude\":%.7f,"
        "\"battery_mv\":%u,\"accuracy_m\":%u,\"fix_age_s\":%u,"
        "\"rssi_dbm\":%d,\"snr_db\":%.1f,\"verification\":\"%s\","
        "\"payload_b64\":\"%s\"}",
        (unsigned long)record.local_id, record.source_id, pkt_msg_seq(packet),
        (unsigned long)record.gateway_rx_time_unix, pkt_time_unix(packet),
        bp_status_display((bp_status_t)pkt_status(packet)),
        bp_profile_name((bp_profile_t)pkt_power_profile(packet)),
        bp_tx_reason_display(pkt_tx_reason(packet)),
        hasGps ? "true" : "false",
        hasGps ? pkt_lat_e7(packet) / 1e7 : 0.0,
        hasGps ? pkt_lon_e7(packet) / 1e7 : 0.0,
        pkt_batt_mV(packet), pkt_acc_m(packet), pkt_fix_age_s(packet),
        record.rssi_dbm, record.snr_x10 / 10.0f,
        journalSyncName(record.sync_state),
        base64Encode(packet, record.packet_len).c_str());
    json += item;
}

static void handleApiHistory() {
    uint16_t sourceId = parseSerialDeviceId(httpServer.arg("device"));
    uint16_t limit = (uint16_t)constrain(httpServer.arg("limit").toInt(), 1, 100);
    if (httpServer.arg("limit").length() == 0) limit = 100;
    if (sourceId == 0) {
        httpServer.send(400, "application/json", "{\"error\":\"device_required\"}");
        return;
    }

    uint32_t ids[BP_JOURNAL_PER_DEVICE] = {};
    uint16_t count = offlineJournal.collectLocalIds(sourceId, ids, BP_JOURNAL_PER_DEVICE);
    uint16_t start = count > limit ? count - limit : 0;
    String json;
    json.reserve((count - start) * 420 + 64);
    json = "{\"device_id\":" + String(sourceId) + ",\"items\":[";
    bool first = true;
    for (uint16_t i = start; i < count; ++i) {
        bp_journal_record_t record{};
        if (!offlineJournal.find(ids[i], sourceId, record)) continue;
        if (!first) json += ',';
        appendHistoryJson(json, record);
        first = false;
    }
    json += "]}";
    httpServer.send(200, "application/json", json);
}

static void handleApiHistoryCsv() {
    uint16_t sourceId = parseSerialDeviceId(httpServer.arg("device"));
    if (sourceId == 0) {
        httpServer.send(400, "application/json", "{\"error\":\"device_required\"}");
        return;
    }
    uint32_t ids[BP_JOURNAL_PER_DEVICE] = {};
    uint16_t count = offlineJournal.collectLocalIds(sourceId, ids, BP_JOURNAL_PER_DEVICE);
    String csv;
    csv.reserve(count * 150 + 180);
    csv = "local_id,gateway_rx_time_unix,collar_time_unix,device_id,sequence,status,profile,tx_reason,latitude,longitude,battery_mv,accuracy_m,fix_age_s,rssi_dbm,snr_db,verification\r\n";
    for (uint16_t i = 0; i < count; ++i) {
        bp_journal_record_t record{};
        if (!offlineJournal.find(ids[i], sourceId, record)) continue;
        const uint8_t *packet = record.packet;
        bool hasGps = (pkt_flags(packet) & FLAG_HAS_GPS) != 0;
        char row[320];
        snprintf(row, sizeof(row),
            "%lu,%lu,%u,%u,%u,%s,%s,%s,%.7f,%.7f,%u,%u,%u,%d,%.1f,%s\r\n",
            (unsigned long)record.local_id, (unsigned long)record.gateway_rx_time_unix,
            pkt_time_unix(packet), record.source_id, pkt_msg_seq(packet),
            bp_status_display((bp_status_t)pkt_status(packet)),
            bp_profile_name((bp_profile_t)pkt_power_profile(packet)),
            bp_tx_reason_display(pkt_tx_reason(packet)),
            hasGps ? pkt_lat_e7(packet) / 1e7 : 0.0,
            hasGps ? pkt_lon_e7(packet) / 1e7 : 0.0,
            pkt_batt_mV(packet), pkt_acc_m(packet), pkt_fix_age_s(packet),
            record.rssi_dbm, record.snr_x10 / 10.0f,
            journalSyncName(record.sync_state));
        csv += row;
    }
    httpServer.sendHeader("Content-Disposition",
                          "attachment; filename=bluepaws-device-" + String(sourceId) + ".csv");
    httpServer.send(200, "text/csv", csv);
}

// ── API: GET /api/status ──
// Returns hub diagnostic info: uptime, packet counts, memory, WiFi state.
// Displayed in the Settings modal in the web GUI.
static void handleApiStatus() {
    updateConnectivityState();

    uint8_t connectedSse = 0;
    if (xSemaphoreTake(sseMutex, pdMS_TO_TICKS(25))) {
        connectedSse = sseClientCount;
        xSemaphoreGive(sseMutex);
    }
    size_t fsTotal = LittleFS.totalBytes();
    size_t fsUsed = LittleFS.usedBytes();

    char buf[1536];
    snprintf(buf, sizeof(buf),
        "{\"uptime\":%u,\"rxCount\":%u,\"txCount\":%u,"
        "\"crcFails\":%u,\"devices\":%u,\"logEntries\":%u,"
        "\"staConnected\":%s,\"staIP\":\"%s\",\"apEnabled\":%s,\"apIP\":\"%s\","
        "\"freeHeap\":%u,\"hubMode\":\"%s\","
        "\"provisioning_mode\":%s,\"time_synced\":%s,\"hub_time_unix\":%u,"
        "\"mode\":\"%s\",\"wifi_connected\":%s,"
        "\"internet_reachable\":%s,\"cloud_reachable\":%s,"
        "\"last_cloud_success_ms\":%u,\"lora_rx_active\":%s,"
        "\"littlefs_total_bytes\":%u,\"littlefs_used_bytes\":%u,"
        "\"littlefs_free_bytes\":%u,\"sse_clients\":%u,"
        "\"sse_capacity\":%u,\"replay_backlog\":%u,"
        "\"reset_reason\":%u,\"snapshot_http\":%d,\"cached_appearances\":%u,"
        "\"network_phase\":\"%s\",\"recovery_remaining_ms\":%u,\"known_wifi_available\":%s,"
        "\"ap_clients\":%u,\"ap_channel\":%u,\"ap_start_failures\":%u,"
        "\"network_stack_free\":%u,\"web_stack_free\":%u,\"mode_change_pending\":%s}",
        millis() / 1000, rxCount, txCount,
        crcFailCount, deviceCount, (unsigned)offlineJournal.totalValidRecords(),
        staConnected ? "true" : "false",
        staConnected ? WiFi.localIP().toString().c_str() : "",
        hubApEnabled ? "true" : "false",
        WiFi.softAPIP().toString().c_str(),
        ESP.getFreeHeap(),
        hubCommProfileName(hubCommProfile),
        hubProvisioningMode ? "true" : "false",
        hubTimeSynced ? "true" : "false",
        (uint32_t)time(nullptr),
        hubCommProfileName(hubCommProfile),
        hubConnectivity.wifi_connected ? "true" : "false",
        hubConnectivity.internet_reachable ? "true" : "false",
        hubConnectivity.cloud_reachable ? "true" : "false",
        hubConnectivity.last_cloud_success_ms.load(),
        hubConnectivity.lora_rx_active ? "true" : "false",
        (unsigned)fsTotal, (unsigned)fsUsed, (unsigned)(fsTotal - fsUsed),
        connectedSse, MAX_SSE_CLIENTS, offlineJournal.pendingCount(),
        (unsigned)esp_reset_reason(), snapshotLastHttpCode, deviceMetaCount,
        WifiFailover::name(wifiPhase.load()), (unsigned)wifiRecoveryRemainingMs.load(),
        knownWifiAvailable ? "true" : "false", (unsigned)WiFi.softAPgetStationNum(),
        hubApEnabled ? (unsigned)WiFi.channel() : 0, (unsigned)apStartFailures.load(),
        networkTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(networkTaskHandle) : 0,
        webTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(webTaskHandle) : 0,
        modeChangePending ? "true" : "false"
    );
    httpServer.send(200, "application/json", buf);
}

static String getPostField(const String &body, const char *name) {
    if (httpServer.hasArg(name)) {
        return httpServer.arg(name);
    }

    String marker = String(name) + "=";
    int idx = body.indexOf(marker);
    if (idx < 0) {
        return "";
    }

    String value = body.substring(idx + marker.length());
    int ampIdx = value.indexOf('&');
    if (ampIdx >= 0) {
        value = value.substring(0, ampIdx);
    }
    return value;
}

static bool validLocalName(const String &value) {
    if (value.length() == 0 || value.length() > 32) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        uint8_t c = (uint8_t)value[i];
        if (c < 0x20 || c == '"' || c == '\\') return false;
    }
    return true;
}

static bool validMarkerColour(const String &value) {
    if (value.length() != 7 || value[0] != '#') return false;
    for (size_t i = 1; i < value.length(); ++i) {
        if (!isxdigit((unsigned char)value[i])) return false;
    }
    return true;
}

static bool validLocalEmoji(const String &value) {
    if (value.length() == 0 || value.length() > 16) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        uint8_t c = (uint8_t)value[i];
        if (c < 0x20 || c == '<' || c == '>' || c == '&' || c == '"'
            || c == '\'' || c == '\\') return false;
    }
    return true;
}

// Hub-local appearance overrides never alter Supabase. They are deliberately
// small and stored beside the journal so the Off-Grid UI stays recognisable.
static void handleApiDeviceMetadata() {
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "text/plain", "POST only");
        return;
    }
    if (!requireCommandAccess()) return;

    String body = httpServer.arg("plain");
    uint16_t id = parseSerialDeviceId(getPostField(body, "device"));
    String name = getPostField(body, "name");
    String emoji = getPostField(body, "emoji");
    String colour = getPostField(body, "colour");
    if (id == 0 || !validLocalName(name) || !validLocalEmoji(emoji)
        || !validMarkerColour(colour)) {
        httpServer.send(400, "application/json", "{\"error\":\"invalid_appearance\"}");
        return;
    }

    device_meta_t *meta = findDeviceMetadata(id, true);
    if (!meta) {
        httpServer.send(409, "application/json", "{\"error\":\"device_limit\"}");
        return;
    }
    strlcpy(meta->name, name.c_str(), sizeof(meta->name));
    strlcpy(meta->emoji, emoji.c_str(), sizeof(meta->emoji));
    strlcpy(meta->colour, colour.c_str(), sizeof(meta->colour));
    meta->local_override = true;
    if (!saveDeviceMetadata()) {
        httpServer.send(500, "application/json", "{\"error\":\"storage_failed\"}");
        return;
    }

    JsonDocument response;
    response["ok"] = true;
    response["id"] = id;
    response["name"] = meta->name;
    response["emoji"] = meta->emoji;
    response["colour"] = meta->colour;
    String json;
    serializeJson(response, json);
    sseBroadcast("appearance", json.c_str());
    httpServer.send(200, "application/json", json);
}

// ── API: POST /api/command ──
// Sends a mode-change command to a collar.
// Body format: device=XXXX&mode=normal|active|lost|powersave
// The device ID is in hex (e.g. "0001"), mode is the profile name.
static void handleApiCommand() {
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "text/plain", "POST only");
        return;
    }
    if (!requireCommandAccess()) return;

    String body = httpServer.arg("plain");

    // Parse the target device ID (hex string → uint16_t)
    uint16_t targetId = 0;
    bp_profile_t mode = PROFILE_UNKNOWN;

    String deviceStr = getPostField(body, "device");
    if (deviceStr.length() > 0) {
        targetId = (uint16_t)strtoul(deviceStr.c_str(), NULL, 16);
    }

    // Parse the desired operating mode
    String modeStr = getPostField(body, "mode");
    if (modeStr.length() > 0) {
        mode = bp_profile_from_name(modeStr.c_str());  // "normal" → PROFILE_NORMAL, etc.
    }

    if (targetId == 0 || mode == PROFILE_UNKNOWN) {
        httpServer.send(400, "text/plain", "Bad request: device=XXXX&mode=name");
        return;
    }

    // Build the command packet and queue it for LoRa TX
    uint16_t seq = sendCommand(targetId, PKT_CMD_MODE, mode);
    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":%s,\"device\":%u,\"cmdSeq\":%u}",
             seq ? "true" : "false", targetId, seq);
    httpServer.send(seq ? 202 : 503, "application/json", response);
}

// ── API: POST /api/find ──
// Triggers the "Find My Pet" feature — makes the collar beep and flash its LED.
// Body format: device=XXXX&pattern=chirp&flash=5
// Pattern options: off, chirp, trill, siren, melody_a, melody_b
static void handleApiFind() {
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "text/plain", "POST only");
        return;
    }
    if (!requireCommandAccess()) return;

    String body = httpServer.arg("plain");

    // Parse target device ID
    uint16_t targetId = 0;
    String deviceStr = getPostField(body, "device");
    if (deviceStr.length() > 0) {
        targetId = (uint16_t)strtoul(deviceStr.c_str(), NULL, 16);
    }

    if (targetId == 0) {
        httpServer.send(400, "text/plain", "Bad request: device=XXXX required");
        return;
    }

    // Parse buzzer pattern — which sound the collar should play
    bp_buzzer_pattern_t pattern = BUZZER_CHIRP;  // Default: 3 short beeps
    int pIdx = body.indexOf("pattern=");
    if (pIdx >= 0) {
        String patStr = body.substring(pIdx + 8);
        int amp = patStr.indexOf('&');
        if (amp >= 0) patStr = patStr.substring(0, amp);
        if      (patStr == "off")      pattern = BUZZER_OFF;
        else if (patStr == "chirp")    pattern = BUZZER_CHIRP;     // 3 short beeps
        else if (patStr == "trill")    pattern = BUZZER_TRILL;     // Rising 5-note trill
        else if (patStr == "siren")    pattern = BUZZER_SIREN;     // Two-tone alternating
        else if (patStr == "melody_a") pattern = BUZZER_MELODY_A;  // Jingle for collar 1
        else if (patStr == "melody_b") pattern = BUZZER_MELODY_B;  // Jingle for collar 2
    }

    // Parse LED flash count — how many times the collar LED should blink
    uint8_t flashCount = 5;  // Default: 5 flashes
    int fIdx = body.indexOf("flash=");
    if (fIdx >= 0) {
        flashCount = (uint8_t)strtoul(body.c_str() + fIdx + 6, NULL, 10);
    }

    // Build and queue the find command for LoRa TX
    uint16_t seq = sendCommandFind(targetId, PKT_CMD_FIND, PROFILE_UNKNOWN, flashCount, pattern);

    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"ok\":%s,\"device\":%u,\"cmdSeq\":%u,\"pattern\":%u,\"flash\":%u}",
        seq ? "true" : "false", targetId, seq, pattern, flashCount);
    httpServer.send(seq ? 202 : 503, "application/json", resp);
}

// ── API: POST /api/device-status ──
// Queues a collar status command. The collar replies with STATUS_RESP,
// including its current profile, TX power, sleep interval, GPS warm-start
// state, home-cycle counter, and original command sequence for ACK matching.
// Body format: device=XXXX where XXXX is a hex device ID, e.g. 03E9 for 1001.
static void handleApiDeviceStatusCommand() {
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "text/plain", "POST only");
        return;
    }
    if (!requireCommandAccess()) return;

    String body = httpServer.arg("plain");

    uint16_t targetId = 0;
    String deviceStr = getPostField(body, "device");
    if (deviceStr.length() > 0) {
        targetId = (uint16_t)strtoul(deviceStr.c_str(), NULL, 16);
    }

    if (targetId == 0) {
        httpServer.send(400, "text/plain", "Bad request: device=XXXX required");
        return;
    }

    uint16_t seq = sendStatusCommand(targetId);

    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"ok\":%s,\"device\":%u,\"cmdSeq\":%u,\"command\":\"status\"}",
        seq ? "true" : "false", targetId, seq);
    httpServer.send(seq ? 202 : 503, "application/json", resp);
}

// ── API: POST /api/config ──
// Saves WiFi and cloud settings to flash, then restarts the ESP32
// so the new WiFi credentials take effect.
// Body format: ssid=MyNetwork&pass=MyPassword&cloud_url=https://...
static bool validWifiCredentials(const String &ssid, const String &pass) {
    if (!ssid.length() || ssid.length() > 32 || ssid.indexOf('\n') >= 0 || ssid.indexOf('\r') >= 0) return false;
    if (pass.indexOf('\n') >= 0 || pass.indexOf('\r') >= 0) return false;
    if (!pass.length() || (pass.length() >= 8 && pass.length() <= 63)) return true;
    if (pass.length() != 64) return false;
    for (size_t i = 0; i < pass.length(); ++i) if (!isxdigit(pass[i])) return false;
    return true;
}

static void handleApiConfig() {
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "text/plain", "POST only");
        return;
    }

    // An open search-party hotspot is not permission to change cloud routing
    // or Wi-Fi credentials. Provisioning must be enabled locally on the hub.
    if (!hubProvisioningMode || hubCommProfile == HUB_COMM_OFF_GRID) {
        httpServer.send(403, "application/json", "{\"error\":\"provisioning_required\"}");
        return;
    }

    String body = httpServer.arg("plain");

    // WebServer already URL-decodes form fields. Omitted network fields and
    // cloud credentials are preserved rather than silently erased on save.
    String newSSID = getPostField(body, "ssid");
    String newPass = getPostField(body, "pass");
    String newSecondary = getPostField(body, "secondary_ssid");
    String newSecondaryPass = getPostField(body, "secondary_pass");
    String newCloud = getPostField(body, "cloud_url");
    String newToken = getPostField(body, "cloud_token");
    if ((newSSID.length() && !validWifiCredentials(newSSID, newPass))
        || (newSecondary.length() && !validWifiCredentials(newSecondary, newSecondaryPass))
        || newCloud.indexOf('\n') >= 0 || newCloud.indexOf('\r') >= 0
        || newToken.indexOf('\n') >= 0 || newToken.indexOf('\r') >= 0) {
        httpServer.send(400, "application/json", "{\"error\":\"invalid_configuration\"}");
        return;
    }
    if (newSSID.length()) { staSSID = newSSID; staPass = newPass; }
    if (getPostField(body, "clear_secondary") == "true") { secondarySSID = ""; secondaryPass = ""; }
    else if (newSecondary.length()) { secondarySSID = newSecondary; secondaryPass = newSecondaryPass; }
    if (newCloud.length() > 0) cloudEndpoint = newCloud;
    if (newToken.length() > 0) cloudToken = newToken;
    if (!saveHubConfigToFlash(true)) {
        httpServer.send(500, "application/json", "{\"error\":\"config_save_failed\"}");
        return;
    }
    httpServer.send(200, "application/json", "{\"ok\":true,\"restart\":true}");

    // Restart the ESP32 so WiFi reconnects with new credentials
    delay(500);
    ESP.restart();
}

// Catch-all handler for any URL not matched by explicit routes.
// Public assets only: configuration and journals share this filesystem but
// must never be downloadable through a guessed path on the open hotspot.
static void handleNotFound() {
    String path = httpServer.uri();
    if (path.startsWith("/tiles/")) {
        // Reserved local tile API. The bundled vector skeleton is the current
        // source; a future SD-backed provider can serve raster/vector bytes.
        httpServer.send(204, "image/png", "");
        return;
    }
    bool publicAsset = path == "/leaflet.js" || path == "/leaflet.css"
        || path == "/basemap.json" || path == "/images/marker-icon.png"
        || path == "/images/marker-icon-2x.png" || path == "/images/marker-shadow.png"
        || path == "/brand-favicon.ico" || path == "/brand-mascot.avif"
        || path == "/location-fit-markers.png" || path == "/map-location.png"
        || path == "/welcome.js" || path == "/feedback.js"
        || path == "/hub-presence.js" || path == "/hub-presence.css";
    if (httpServer.method() == HTTP_GET && publicAsset && LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        // Determine MIME type from file extension
        String contentType = "text/plain";
        if (path.endsWith(".html"))      contentType = "text/html";
        else if (path.endsWith(".css"))   contentType = "text/css";
        else if (path.endsWith(".js"))    contentType = "application/javascript";
        else if (path.endsWith(".json"))  contentType = "application/json";
        else if (path.endsWith(".png"))   contentType = "image/png";
        else if (path.endsWith(".ico"))   contentType = "image/x-icon";
        else if (path.endsWith(".avif"))  contentType = "image/avif";
        if (path == "/welcome.js") httpServer.sendHeader("Cache-Control", "no-store");
        httpServer.streamFile(f, contentType);
        f.close();
        return;
    }
    // Unknown navigation on a captured external hostname (including Windows'
    // portal URL variants) must land on the IP, not stay on a probe origin.
    // Canonical-IP missing assets/private files and API errors remain real 404s.
    if (isCaptivePortalClient() && hasForeignPortalHost() && !path.startsWith("/api/")
        && (httpServer.method() == HTTP_GET || httpServer.method() == HTTP_HEAD)) {
        handleCaptiveProbe();
        return;
    }
    httpServer.send(404, "text/plain", "Not found");
}

// Register all HTTP routes and start the web server
static void initWebServer() {
    const char *headers[] = {"X-Bluepaws-Local-Session"};
    httpServer.collectHeaders(headers, 1);
    // Static file routes
    httpServer.on("/",             HTTP_GET,  handleRoot);     // Main page
    httpServer.on("/welcome", HTTP_GET, handleWelcome);
    httpServer.on("/bluepaws-hub.url", HTTP_GET, []() {
        httpServer.sendHeader("Content-Disposition", "attachment; filename=\"Bluepaws Hub.url\"");
        httpServer.sendHeader("Cache-Control", "no-store");
        httpServer.send(200, "application/octet-stream",
            "[InternetShortcut]\r\nURL=http://192.168.4.1/\r\n");
    });
    httpServer.on("/api/welcome", HTTP_GET, handleApiWelcome);
    httpServer.on("/style.css",    HTTP_GET,  handleCSS);      // Stylesheet
    httpServer.on("/app.js",       HTTP_GET,  handleJS);       // JavaScript app
    httpServer.on("/favicon.svg", HTTP_GET, handleFavicon);
    httpServer.on("/favicon.ico", HTTP_GET, handleFavicon); // older browser fallback
    // SSE real-time event stream
    httpServer.on("/events",       HTTP_GET,  handleEvents);
    // REST API endpoints
    httpServer.on("/api/devices",  HTTP_GET,  handleApiDevices);  // Get all device states
    httpServer.on("/api/history", HTTP_GET, handleApiHistory);
    httpServer.on("/api/history.csv", HTTP_GET, handleApiHistoryCsv);
    httpServer.on("/api/status",   HTTP_GET,  handleApiStatus);   // Get hub diagnostics
    httpServer.on("/api/hub-presence", HTTP_GET, handleHubPresence);
    httpServer.on("/api/hub-preferences", HTTP_POST, handleHubPreferences);
    httpServer.on("/api/device-meta", HTTP_POST, handleApiDeviceMetadata);
    httpServer.on("/api/command",  HTTP_POST, handleApiCommand);  // Send mode command
    httpServer.on("/api/commands", HTTP_GET, handleApiCommands);
    httpServer.on("/api/device-status", HTTP_POST, handleApiDeviceStatusCommand); // Request collar status
    httpServer.on("/api/find",     HTTP_POST, handleApiFind);     // Trigger find (buzzer+LED)
    httpServer.on("/api/config",   HTTP_POST, handleApiConfig);   // Save WiFi/cloud config
    httpServer.on("/api/ble",      HTTP_GET,  handleApiBle);       // BLE scan results (portable mode)
    httpServer.on("/api/hub-mode", HTTP_POST, handleApiHubMode);   // Toggle home/portable mode
    httpServer.on("/api/security", HTTP_GET, handleApiSecurityStatus);
    httpServer.on("/api/security/pin", HTTP_POST, handleApiSecurityPin);
    httpServer.on("/api/security/unlock", HTTP_POST, handleApiSecurityUnlock);
    httpServer.on("/generate_204", HTTP_GET, handleCaptiveProbe);
    httpServer.on("/gen_204", HTTP_GET, handleCaptiveProbe);
    httpServer.on("/hotspot-detect.html", HTTP_GET, handleCaptiveProbe);
    httpServer.on("/library/test/success.html", HTTP_GET, handleCaptiveProbe);
    httpServer.on("/ncsi.txt", HTTP_GET, handleCaptiveProbe);
    httpServer.on("/connecttest.txt", HTTP_GET, handleCaptiveProbe);
    httpServer.on("/redirect", HTTP_GET, handleCaptiveProbe); // Windows NCSI browser launch
    httpServer.on("/fwlink", HTTP_GET, handleCaptiveProbe);   // older Windows portal launch
    httpServer.onNotFound(handleNotFound);                        // Serve other files from flash
    httpServer.begin();
    Serial.printf("[WEB] HTTP server on port %d\n", HTTP_PORT);
}

// Web task — core 1, below LoRa priority. Handles incoming HTTP requests and
// sends periodic SSE heartbeats so the browser knows the connection is alive.
static void webTask(void *param) {
    (void)param;
    while (!networkStackReady) vTaskDelay(pdMS_TO_TICKS(10));
    initWebServer();

    TickType_t lastHeartbeat = 0;

    for (;;) {
        // Process any pending HTTP requests (non-blocking)
        if (clearOfflineSessions.exchange(false)) offlineAccess.disable();
        httpServer.handleClient();

        // Send a heartbeat event every 5 seconds. The browser uses this
        // to detect if the SSE connection has dropped (10s watchdog).
        TickType_t now = xTaskGetTickCount();
        if (now - lastHeartbeat >= pdMS_TO_TICKS(5000)) {
            lastHeartbeat = now;
            char health[200];
            snprintf(health, sizeof(health),
                "{\"hubMode\":\"%s\",\"network_phase\":\"%s\",\"known_wifi_available\":%s}",
                hubCommProfileName(hubCommProfile), WifiFailover::name(wifiPhase.load()),
                knownWifiAvailable ? "true" : "false");
            sseBroadcast("heartbeat", health);
        }

        // 5ms sleep — fast enough for responsive web UI
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ═══════════════════════════════════════════════
// USB Serial Bench Console
//
// This is intentionally a local bench/debug path. It lets us configure Wi-Fi
// and queue collar commands even when the hub's HTTP server is unreachable
// because the hub is on an isolated guest network.
// ═══════════════════════════════════════════════

static void consoleTask(void *param) {
    (void)param;

    String line;
    line.reserve(160);

    for (;;) {
        while (Serial.available() > 0) {
            char c = (char)Serial.read();
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                line.trim();
                if (line.length() > 0) {
                    handleSerialCommand(line);
                }
                line = "";
            } else if (line.length() < 180) {
                line += c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool extractSerialArg(const String &line, const char *key, String &out) {
    String marker = String(key) + "=";
    int idx = line.indexOf(marker);
    if (idx < 0) {
        return false;
    }

    int start = idx + marker.length();
    if (start >= (int)line.length()) {
        out = "";
        return true;
    }

    char quote = 0;
    if (line[start] == '"' || line[start] == '\'') {
        quote = line[start];
        start++;
    }

    int end = start;
    if (quote != 0) {
        end = line.indexOf(quote, start);
        if (end < 0) {
            end = line.length();
        }
    } else {
        while (end < (int)line.length() && line[end] != ' ') {
            end++;
        }
    }

    out = line.substring(start, end);
    out.trim();
    return true;
}

static uint16_t parseSerialDeviceId(String value) {
    value.trim();
    if (value.startsWith("0x") || value.startsWith("0X")) {
        return (uint16_t)strtoul(value.c_str() + 2, NULL, 16);
    }

    bool containsHexAlpha = false;
    for (size_t i = 0; i < value.length(); i++) {
        char c = value[i];
        if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
            containsHexAlpha = true;
            break;
        }
    }

    return (uint16_t)strtoul(value.c_str(), NULL, containsHexAlpha ? 16 : 10);
}

static bool saveHubConfigToFlash(bool resetMode) {
    // Commit complete config via rename; a reset must not leave half a file.
    File f = LittleFS.open("/config.tmp", "w");
    if (!f) {
        return false;
    }

    if (staSSID.length() > 0) f.printf("sta_ssid=%s\n", staSSID.c_str());
    f.printf("sta_pass=%s\n", staPass.c_str());
    f.printf("secondary_ssid=%s\n", secondarySSID.c_str());
    f.printf("secondary_pass=%s\n", secondaryPass.c_str());
    if (cloudEndpoint.length() > 0) f.printf("cloud_url=%s\n", cloudEndpoint.c_str());
    if (cloudToken.length() > 0) f.printf("cloud_token=%s\n", cloudToken.c_str());
    f.printf("hub_mode=%s\n", hubCommProfileName(resetMode ? HUB_COMM_HOME : hubCommProfile.load()));
    f.printf("provisioning=%s\n", hubProvisioningMode ? "true" : "false");
    f.flush();
    bool ok = !f.getWriteError();
    f.close();
    return ok && LittleFS.rename("/config.tmp", CONFIG_FILE_PATH);
}

static void printSerialHelp() {
    Serial.println();
    Serial.println("[CMD] Home Hub bench console");
    Serial.println("      help");
    Serial.println("      status");
    Serial.println("      reboot");
    Serial.println("      wifi ssid=\"Your SSID\" pass=\"Your password\"");
    Serial.println("      wifi ssid=\"Open network\"");
    Serial.println("      wifi secondary ssid=\"Phone hotspot\" pass=\"Your password\"");
    Serial.println("      wifi secondary clear");
    Serial.println("      mode off_grid | mode home confirm | mode portable confirm");
    Serial.println("      cloud token=\"gateway-bearer-token\"");
    Serial.println("      cloud url=\"https://project.supabase.co/functions/v1/ingest-position\"");
    Serial.println("      cmd mode 1001 active");
    Serial.println("      cmd mode 1001 normal");
    Serial.println("      cmd status 1001");
    Serial.println("      cmd find 1001");
    Serial.println();
}

static void printHubSerialStatus() {
    updateConnectivityState();
    Serial.println("[CFG] Current hub relay configuration");
    Serial.printf("      Profile   : %s\n", hubCommProfileName(hubCommProfile));
    Serial.printf("      Wi-Fi SSID: %s\n", staSSID.length() ? staSSID.c_str() : "<not set>");
    Serial.printf("      Wi-Fi pass: %s\n", staPass.length() ? "<stored>" : "<not set>");
    Serial.printf("      Secondary : %s\n", secondarySSID.length() ? secondarySSID.c_str() : "<not configured>");
    Serial.printf("      Network   : %s, recovery remaining %lu ms\n", WifiFailover::name(wifiPhase.load()),
                  (unsigned long)wifiRecoveryRemainingMs.load());
    Serial.printf("      STA       : %s\n", WiFi.status() == WL_CONNECTED ? "connected" : "not connected");
    Serial.printf("      STA IP    : %s\n", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "<none>");
    Serial.printf("      RSSI      : %d\n", WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    Serial.printf("      AP        : %s\n", hubApEnabled ? "enabled" : "disabled");
    Serial.printf("      AP IP     : %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("      AP clients: %u; start failures: %lu; network/web stack free: %u/%u\n",
                  WiFi.softAPgetStationNum(), (unsigned long)apStartFailures.load(),
                  networkTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(networkTaskHandle) : 0,
                  webTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(webTaskHandle) : 0);
    Serial.printf("      Cloud URL : %s\n", cloudEndpoint.c_str());
    Serial.printf("      Token     : %s\n", cloudToken.length() ? "<stored>" : "<not set>");
    Serial.printf("      Gateway   : %04X\n", GATEWAY_GUID16);
    Serial.printf("      Time/NTP  : %s\n", hubTimeSynced ? "synced" : "not synced");
    Serial.printf("      RX/TX     : %lu/%lu\n", (unsigned long)rxCount, (unsigned long)txCount);
}

static void handleSerialCommand(String line) {
    String lower = line;
    lower.toLowerCase();

    if (lower == "help" || lower == "?") {
        printSerialHelp();
        return;
    }

    if (lower == "status") {
        printHubSerialStatus();
        return;
    }

    if (lower == "reboot" || lower == "restart") {
        Serial.println("[CMD] Rebooting hub...");
        delay(250);
        ESP.restart();
        return;
    }

    if (lower.startsWith("mode ")) {
        String mode = lower.substring(5);
        bool confirmed = mode.endsWith(" confirm");
        if (confirmed) mode.remove(mode.length() - 8);
        if (mode != "home" && mode != "portable" && mode != "off_grid") {
            Serial.println("[CMD] Usage: mode off_grid | mode home confirm | mode portable confirm");
        } else if (hubCommProfile == HUB_COMM_OFF_GRID && mode != "off_grid" && !confirmed) {
            Serial.println("[CMD] Add confirm to leave Off-Grid; local users will disconnect");
        } else {
            Serial.println(requestHubMode(hubCommProfileFromString(mode), confirmed)
                ? "[CMD] Network mode change queued" : "[CMD] Another mode change is pending");
        }
        return;
    }

    if (lower == "wifi secondary clear") {
        secondarySSID = "";
        secondaryPass = "";
        if (!saveHubConfigToFlash(true)) { Serial.println("[CMD] Config save failed"); return; }
        Serial.println("[CMD] Secondary Wi-Fi cleared; restarting");
        delay(500);
        ESP.restart();
        return;
    }
    if (lower.startsWith("wifi ")) {
        String ssid;
        String pass;
        if (!extractSerialArg(line, "ssid", ssid) || ssid.length() == 0) {
            Serial.println("[CMD] Usage: wifi ssid=\"Your SSID\" pass=\"Your password\"");
            return;
        }
        extractSerialArg(line, "pass", pass);
        if (!validWifiCredentials(ssid, pass)) {
            Serial.println("[CMD] Invalid Wi-Fi SSID/password length or characters");
            return;
        }
        const bool secondary = lower.startsWith("wifi secondary ");
        if (secondary) { secondarySSID = ssid; secondaryPass = pass; }
        else { staSSID = ssid; staPass = pass; }
        hubProvisioningMode = false;

        if (!saveHubConfigToFlash(true)) {
            Serial.println("[CMD] Failed to save Wi-Fi config to LittleFS.");
            return;
        }

        Serial.printf("[CMD] Saved %s Wi-Fi. Rebooting to reconnect...\n", secondary ? "secondary" : "primary");
        delay(500);
        ESP.restart();
        return;
    }

    if (lower.startsWith("cloud ")) {
        String newToken;
        String newUrl;
        extractSerialArg(line, "token", newToken);
        extractSerialArg(line, "url", newUrl);
        if (newToken.length() == 0 && newUrl.length() == 0) {
            Serial.println("[CMD] Usage: cloud token=\"gateway-bearer-token\" [url=\"https://...\"]");
            return;
        }

        if (newToken.length() > 0) cloudToken = newToken;
        if (newUrl.length() > 0) cloudEndpoint = newUrl;
        if (!saveHubConfigToFlash()) {
            Serial.println("[CMD] Failed to save cloud config to LittleFS.");
            return;
        }

        Serial.printf("[CMD] Cloud configuration saved (token=%s, endpoint=%s).\n",
                      cloudToken.length() ? "stored" : "not set",
                      cloudEndpoint.c_str());
        return;
    }

    if (lower.startsWith("cmd mode ")) {
        int firstSpace = line.indexOf(' ', 9);
        if (firstSpace < 0) {
            Serial.println("[CMD] Usage: cmd mode 1001 active|normal|powersave|lost");
            return;
        }

        String idStr = line.substring(9, firstSpace);
        String modeStr = line.substring(firstSpace + 1);
        idStr.trim();
        modeStr.trim();
        modeStr.toLowerCase();

        uint16_t targetId = parseSerialDeviceId(idStr);
        bp_profile_t mode = bp_profile_from_name(modeStr.c_str());
        if (targetId == 0 || mode == PROFILE_UNKNOWN) {
            Serial.println("[CMD] Bad mode command. Use: cmd mode 1001 active");
            return;
        }

        sendCommand(targetId, PKT_CMD_MODE, mode);
        Serial.printf("[CMD] Serial queued MODE %s for device %u\n", bp_profile_name(mode), targetId);
        return;
    }

    if (lower.startsWith("cmd status ")) {
        String idStr = line.substring(11);
        idStr.trim();
        uint16_t targetId = parseSerialDeviceId(idStr);
        if (targetId == 0) {
            Serial.println("[CMD] Usage: cmd status 1001");
            return;
        }

        sendStatusCommand(targetId);
        Serial.printf("[CMD] Serial queued STATUS for device %u\n", targetId);
        return;
    }

    if (lower.startsWith("cmd find ")) {
        String idStr = line.substring(9);
        idStr.trim();
        uint16_t targetId = parseSerialDeviceId(idStr);
        if (targetId == 0) {
            Serial.println("[CMD] Usage: cmd find 1001");
            return;
        }

        sendCommandFind(targetId, PKT_CMD_FIND, PROFILE_UNKNOWN, 5, BUZZER_CHIRP);
        Serial.printf("[CMD] Serial queued FIND for device %u\n", targetId);
        return;
    }

    Serial.printf("[CMD] Unknown command: %s\n", line.c_str());
    Serial.println("[CMD] Type 'help' for available commands.");
}

// ═══════════════════════════════════════════════
// BLE Portable Mode — Scan Callback
//
// In Portable Mode, the hub stops advertising its home beacon and
// instead scans for collar BLE find beacons ("BP_FIND_XXXX").
// Each detected beacon is stored with RSSI for proximity display.
// ═══════════════════════════════════════════════

class FindBeaconCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!advertisedDevice.haveName()) return;
        String name = advertisedDevice.getName().c_str();
        if (!name.startsWith(BLE_FIND_BEACON_PREFIX)) return;

        uint16_t devId = (uint16_t)strtoul(
            name.c_str() + strlen(BLE_FIND_BEACON_PREFIX), NULL, 16);
        if (devId == 0) return;

        int rssi = advertisedDevice.getRSSI();

        if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(10))) {
            ble_scan_result_t *slot = nullptr;
            for (uint8_t i = 0; i < bleScanCount; i++) {
                if (bleScanResults[i].device_id == devId) {
                    slot = &bleScanResults[i];
                    break;
                }
            }
            if (!slot && bleScanCount < MAX_BLE_DEVICES) {
                slot = &bleScanResults[bleScanCount++];
                slot->device_id = devId;
            }
            if (slot) {
                slot->rssi = rssi;
                slot->last_seen_ms = millis();
            }
            xSemaphoreGive(bleMutex);
        }
    }
};

static FindBeaconCallbacks findBeaconCb;

// ═══════════════════════════════════════════════
// Hub Communications Profile BLE Roles
// ═══════════════════════════════════════════════

static void applyBleRoleForCurrentProfile() {
    hubBeaconAdvertising = false;
    if (hubProfileUsesBleScanning()) {
        // Stop home beacon
        BLEDevice::getAdvertising()->stop();
        Serial.println("[BLE] Home beacon stopped");

        // Start BLE scanning for collar find beacons
        pBLEScan = BLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(&findBeaconCb, true);
        pBLEScan->setActiveScan(false); // find beacon identity is in advertisements
        pBLEScan->setInterval(160);
        pBLEScan->setWindow(32);      // 20% scan duty; leave airtime for Wi-Fi AP

        Serial.printf("[HUB] %s profile — BLE scanning for collars\n",
                      hubCommProfileName(hubCommProfile));
        return;
    }

    // Stop BLE scanning
    if (pBLEScan) {
        pBLEScan->stop();
        pBLEScan->clearResults();
    }

    if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(50))) {
        bleScanCount = 0;
        xSemaphoreGive(bleMutex);
    }

    if (homeBeaconAllowed && hubBeaconEnabled) {
        BLEDevice::getAdvertising()->start();
        hubBeaconAdvertising = true;
        Serial.println("[BLE] Primary Wi-Fi connected: home beacon advertising");
    } else {
        BLEDevice::getAdvertising()->stop();
        Serial.println("[BLE] Home beacon paused until primary Wi-Fi is connected");
    }
}

// ═══════════════════════════════════════════════
// API: GET /api/ble — BLE scan results (portable mode)
// ═══════════════════════════════════════════════

static void handleApiBle() {
    String json = "[";
    if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(50))) {
        uint32_t now = millis();
        for (uint8_t i = 0; i < bleScanCount; i++) {
            if (i > 0) json += ",";
            char entry[128];
            snprintf(entry, sizeof(entry),
                "{\"id\":%u,\"name\":\"%s\",\"rssi\":%d,\"age_ms\":%u}",
                bleScanResults[i].device_id,
                bp_device_name(bleScanResults[i].device_id),
                bleScanResults[i].rssi,
                now - bleScanResults[i].last_seen_ms);
            json += entry;
        }
        xSemaphoreGive(bleMutex);
    }
    json += "]";
    httpServer.send(200, "application/json", json);
}

// ═══════════════════════════════════════════════
// API: POST /api/hub-mode — Select the explicit communications profile
// Body: mode=home, mode=portable, or mode=off_grid
// ═══════════════════════════════════════════════

static void handleApiHubMode() {
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "text/plain", "POST only");
        return;
    }

    String body = httpServer.arg("plain");
    // WebServer parses form-urlencoded bodies into named arguments; "plain"
    // is empty for the browser's normal form POSTs.
    String requestedMode = getPostField(body, "mode");
    if (requestedMode != "home" && requestedMode != "portable"
        && requestedMode != "off_grid" && requestedMode != "off-grid") {
        httpServer.send(400, "text/plain", "Bad request: mode=home|portable|off_grid");
        return;
    }
    bool leavingOffGrid = hubCommProfile == HUB_COMM_OFF_GRID
        && requestedMode != "off_grid" && requestedMode != "off-grid";
    if (leavingOffGrid && getPostField(body, "confirm") != "true") {
        httpServer.send(409, "application/json",
                        "{\"error\":\"confirmation_required\",\"detail\":\"Confirm leaving Off-Grid mode\"}");
        return;
    }
    if (leavingOffGrid && !requireCommandAccess()) return;
    if (!requestHubMode(hubCommProfileFromString(requestedMode), getPostField(body, "confirm") == "true")) {
        httpServer.send(409, "application/json", "{\"error\":\"mode_change_pending\"}");
        return;
    }
    httpServer.send(202, "application/json", "{\"pending\":true}");
}

static bool requireCommandAccess() {
    if (clearOfflineSessions.exchange(false)) offlineAccess.disable();
    if (hubCommProfile != HUB_COMM_OFF_GRID || !offlineAccess.enabled()) return true;
    String token = httpServer.header("X-Bluepaws-Local-Session");
    if (offlineAccess.authorize(token, httpServer.client().remoteIP())) return true;
    httpServer.send(403, "application/json", "{\"error\":\"command_pin_required\"}");
    return false;
}

static void handleApiSecurityStatus() {
    char response[96];
    snprintf(response, sizeof(response),
             "{\"pin_enabled\":%s,\"unlocked_sessions\":%u}",
             offlineAccess.enabled() ? "true" : "false",
             offlineAccess.sessionCount());
    httpServer.send(200, "application/json", response);
}

static void handleApiSecurityPin() {
    if (hubCommProfile != HUB_COMM_OFF_GRID) {
        httpServer.send(409, "application/json", "{\"error\":\"off_grid_only\"}");
        return;
    }
    // Once a PIN exists, changing or disabling it requires an already
    // unlocked browser. This prevents a nearby unauthenticated client from
    // silently removing the command guard.
    if (offlineAccess.enabled() && !requireCommandAccess()) return;
    String body = httpServer.arg("plain");
    if (getPostField(body, "enabled") == "false" || getPostField(body, "enabled") == "0") {
        offlineAccess.disable();
        httpServer.send(200, "application/json", "{\"ok\":true,\"pin_enabled\":false}");
        return;
    }
    if (!offlineAccess.setPin(getPostField(body, "pin"))) {
        httpServer.send(400, "application/json", "{\"error\":\"pin_must_be_four_digits\"}");
        return;
    }
    httpServer.send(200, "application/json", "{\"ok\":true,\"pin_enabled\":true}");
}

static void handleApiSecurityUnlock() {
    String token;
    uint32_t retryAfter = 0;
    if (!offlineAccess.unlock(getPostField(httpServer.arg("plain"), "pin"),
                              httpServer.client().remoteIP(), token, retryAfter)) {
        char response[96];
        snprintf(response, sizeof(response),
                 "{\"error\":\"invalid_pin\",\"retry_after_seconds\":%lu}",
                 (unsigned long)retryAfter);
        httpServer.send(retryAfter ? 429 : 403, "application/json", response);
        return;
    }
    String response = "{\"ok\":true,\"session_token\":\"" + token + "\"}";
    httpServer.send(200, "application/json", response);
}

// ═══════════════════════════════════════════════
// BLE Task — Home Beacon / Portable Scanner
//
// In Home mode: periodically restarts BLE advertising as a keepalive.
// In Portable/Off-Grid mode: runs BLE scans and expires stale results.
// ═══════════════════════════════════════════════

static void bleTask(void *param) {
    (void)param;
    int previousRole = -1;
    for (;;) {
        int role = hubProfileUsesBleScanning() ? 2 : (homeBeaconAllowed && hubBeaconEnabled ? 1 : 0);
        if (role != previousRole) {
            applyBleRoleForCurrentProfile(); // BLE operations never called by web/network tasks
            previousRole = role;
        }
        if (hubProfileUsesBleScanning()) {
            // Short passive scans avoid monopolising the shared 2.4 GHz radio.
            if (pBLEScan) {
                pBLEScan->start(1, false);
                pBLEScan->clearResults();  // Free BLE memory after each cycle
            }

            // Expire stale results (>10s old)
            if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(50))) {
                uint32_t now = millis();
                for (uint8_t i = 0; i < bleScanCount; ) {
                    if (now - bleScanResults[i].last_seen_ms > 10000) {
                        bleScanResults[i] = bleScanResults[--bleScanCount];
                    } else {
                        i++;
                    }
                }
                xSemaphoreGive(bleMutex);
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            // React quickly to loss of primary Wi-Fi. Advertising runs in the stack.
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ═══════════════════════════════════════════════
// Cloud Task — REST POST Relay
//
static String base64Encode(const uint8_t *data, uint8_t len) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String out;
    out.reserve(((len + 2) / 3) * 4);
    for (uint8_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        bool has2 = (i + 1) < len;
        bool has3 = (i + 2) < len;
        if (has2) n |= ((uint32_t)data[i + 1]) << 8;
        if (has3) n |= data[i + 2];
        out += alphabet[(n >> 18) & 0x3F];
        out += alphabet[(n >> 12) & 0x3F];
        out += has2 ? alphabet[(n >> 6) & 0x3F] : '=';
        out += has3 ? alphabet[n & 0x3F] : '=';
    }
    return out;
}

static String buildCloudWrapperJson(const cloud_entry_t &entry) {
    char gateway[5];
    snprintf(gateway, sizeof(gateway), "%04X", (uint16_t)GATEWAY_GUID16);

    String body = "{";
    body += "\"format\":\"tlv\",";
    body += "\"payload_b64\":\"";
    body += base64Encode(entry.buf, entry.len);
    body += "\",";
    body += "\"ingest_path\":\"lora_gateway\",";
    body += "\"gateway_guid16\":\"";
    body += gateway;
    body += "\",";
    body += "\"gateway_rx_time_unix\":";
    body += String(entry.gateway_rx_time_unix);
    body += ",";
    body += "\"link_type\":\"lora\",";
    body += "\"link_rssi_dbm\":";
    body += String(entry.rssi);
    body += ",";
    body += "\"link_snr_db\":";
    body += String(entry.snr, 1);
    body += "}";
    return body;
}

static String batchReplayEndpoint() {
    int slash = cloudEndpoint.lastIndexOf('/');
    if (slash < 0) return String();
    return cloudEndpoint.substring(0, slash + 1) + "ingest-position-batch";
}

static void applyJournalVerification(uint32_t localId, uint16_t sourceId,
                                     bp_journal_sync_state_t state) {
    offlineJournal.updateSyncState(localId, sourceId, state);
    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        device_state_t *device = findDevice(sourceId);
        if (device && device->local_id == localId) device->sync_state = state;
        xSemaphoreGive(deviceMutex);
    }
    char event[112];
    snprintf(event, sizeof(event),
             "{\"device_id\":%u,\"local_id\":%lu,\"verification\":\"%s\"}",
             sourceId, (unsigned long)localId, journalSyncName(state));
    sseBroadcast("verification", event);
}

static void syncCloudSnapshotMetadata() {
    static uint32_t nextAttemptMs = 0;
    if ((int32_t)(millis() - nextAttemptMs) < 0 || !hubProfileAllowsCloudRelay()
        || !hubConnectivity.wifi_connected || cloudToken.length() == 0
        || cloudEndpoint.length() == 0) return;

    int slash = cloudEndpoint.lastIndexOf('/');
    if (slash < 0) return;
    char gateway[5];
    snprintf(gateway, sizeof(gateway), "%04x", (uint16_t)GATEWAY_GUID16);
    String endpoint = cloudEndpoint.substring(0, slash + 1) + "hub-snapshot";
    endpoint += "?gateway_guid16=" + String(gateway) + "&limit=1";

    HTTPClient http;
    http.begin(endpoint);
    http.addHeader("Authorization", "Bearer " + cloudToken);
    int code = http.GET();
    snapshotLastHttpCode = code;
    String response = code == 200 ? http.getString() : String();
    http.end();
    if (code != 200) {
        Serial.printf("[SNAPSHOT] Metadata refresh failed HTTP %d\n", code);
        nextAttemptMs = millis() + 300000;
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
        Serial.println("[SNAPSHOT] Invalid metadata response");
        nextAttemptMs = millis() + 300000;
        return;
    }

    bool changed = false;
    for (JsonObject device : doc["devices"].as<JsonArray>()) {
        uint16_t id = device["device_id"] | 0;
        device_meta_t *meta = findDeviceMetadata(id, true);
        if (!meta || meta->local_override) continue;

        const char *name = device["display_name"] | bp_device_name(id);
        const char *emoji = "🐾";
        const char *colour = "#1d9bf0";
        for (JsonObject appearance : doc["appearances"].as<JsonArray>()) {
            if ((uint16_t)(appearance["device_id"] | 0) != id) continue;
            if (strcmp(appearance["avatar_kind"] | "", "emoji") == 0) {
                emoji = appearance["emoji_value"] | emoji;
            }
            colour = appearance["marker_colour"] | colour;
            break;
        }
        String safeName(name);
        String safeEmoji(emoji);
        String safeColour(colour);
        if (!validLocalName(safeName)) name = bp_device_name(id);
        if (!validLocalEmoji(safeEmoji)) emoji = "🐾";
        if (!validMarkerColour(safeColour)) colour = "#1d9bf0";
        if (strcmp(meta->name, name) != 0 || strcmp(meta->emoji, emoji) != 0
            || strcmp(meta->colour, colour) != 0) {
            strlcpy(meta->name, name, sizeof(meta->name));
            strlcpy(meta->emoji, emoji, sizeof(meta->emoji));
            strlcpy(meta->colour, colour, sizeof(meta->colour));
            changed = true;

            JsonDocument eventDoc;
            eventDoc["id"] = id;
            eventDoc["name"] = meta->name;
            eventDoc["emoji"] = meta->emoji;
            eventDoc["colour"] = meta->colour;
            String event;
            serializeJson(eventDoc, event);
            sseBroadcast("appearance", event.c_str());
        }
    }
    if (changed) saveDeviceMetadata();
    Serial.printf("[SNAPSHOT] Cached %u Family collar appearances\n", deviceMetaCount);
    nextAttemptMs = millis() + 3600000;
}

static void replayPendingJournal() {
    static uint32_t nextAttemptMs = 0;
    static uint32_t backoffMs = 5000;
    if ((int32_t)(millis() - nextAttemptMs) < 0 || !hubProfileAllowsCloudRelay()
        || !hubConnectivity.wifi_connected || cloudToken.length() == 0) return;

    bp_journal_record_t pending[10]{};
    uint16_t count = offlineJournal.collectPending(pending, 10);
    if (count == 0) { backoffMs = 5000; return; }

    String body = "{\"items\":[";
    for (uint16_t i = 0; i < count; ++i) {
        cloud_entry_t entry{};
        memcpy(entry.buf, pending[i].packet, pending[i].packet_len);
        entry.len = pending[i].packet_len;
        entry.rssi = pending[i].rssi_dbm;
        entry.snr = pending[i].snr_x10 / 10.0f;
        entry.gateway_rx_time_unix = pending[i].gateway_rx_time_unix;
        if (i) body += ',';
        body += "{\"local_id\":" + String(pending[i].local_id) + ",\"wrapper\":";
        body += buildCloudWrapperJson(entry);
        body += '}';
    }
    body += "]}";
    if (body.length() > 16384) {
        Serial.println("[REPLAY] Batch unexpectedly exceeds 16KB; retrying fewer records later");
        nextAttemptMs = millis() + backoffMs;
        return;
    }

    HTTPClient http;
    http.begin(batchReplayEndpoint());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + cloudToken);
    int code = http.POST(body);
    String response = code > 0 ? http.getString() : String();
    if (code == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, response) == DeserializationError::Ok) {
            for (JsonObject result : doc["results"].as<JsonArray>()) {
                uint32_t localId = result["local_id"] | 0;
                const char *status = result["status"] | "retryable";
                for (uint16_t i = 0; i < count; ++i) {
                    if (pending[i].local_id != localId) continue;
                    if (strcmp(status, "accepted") == 0 || strcmp(status, "duplicate") == 0) {
                        applyJournalVerification(localId, pending[i].source_id, BP_JOURNAL_VALIDATED);
                    } else if (strcmp(status, "rejected") == 0) {
                        applyJournalVerification(localId, pending[i].source_id, BP_JOURNAL_REJECTED);
                    }
                    break;
                }
            }
            backoffMs = 5000;
            nextAttemptMs = millis() + 1000;
            Serial.printf("[REPLAY] Processed batch of %u offline records\n", count);
        } else {
            nextAttemptMs = millis() + backoffMs;
            backoffMs = backoffMs < 150000 ? backoffMs * 2 : 300000;
        }
    } else {
        Serial.printf("[REPLAY] Batch POST failed HTTP %d\n", code);
        nextAttemptMs = millis() + backoffMs;
        backoffMs = backoffMs < 150000 ? backoffMs * 2 : 300000;
    }
    http.end();
}

// Relays raw TLV packets to a cloud server (e.g. Supabase Edge Function).
// Blocks on the cloudQueue — wakes up when handlePacket() enqueues a packet.
// Only sends if WiFi STA is connected AND a cloud endpoint is configured.
// The raw collar TLV remains unchanged; it is base64 encoded into the HTTPS
// JSON transport wrapper expected by Supabase.
// ═══════════════════════════════════════════════

static void cloudTask(void *param) {
    (void)param;
    cloud_entry_t entry;

    for (;;) {
        pollHubSettings(); // Small settings-only read; no fabricated presence or command claims.
        postHubPresence(); // Minute heartbeat, or prompt confirmation after applied settings.
        // Block until a packet is queued (or timeout after 5s for housekeeping)
        if (xQueueReceive(cloudQueue, &entry, pdMS_TO_TICKS(5000)) == pdTRUE) {
            updateConnectivityState();

            // Off-Grid is deliberately local-only. Drop cloud-forward work
            // without blocking the LoRa receive path or local web GUI.
            if (!hubProfileAllowsCloudRelay()) {
                continue;
            }

            // Skip if no internet connection or no endpoint configured
            if (!hubConnectivity.wifi_connected || cloudEndpoint.length() == 0) {
                continue;
            }

            if (cloudToken.length() == 0) {
                Serial.println("[CLOUD] No gateway bearer token configured — skipping POST");
                continue;
            }

            syncHubClock(false);
            String body = buildCloudWrapperJson(entry);

            HTTPClient http;
            http.begin(cloudEndpoint);
            http.addHeader("Content-Type", "application/json");
            String authHeader = "Bearer ";
            authHeader += cloudToken;
            http.addHeader("Authorization", authHeader);

            int code = http.POST(body);
            String response = code > 0 ? http.getString() : String();
            if (code > 0) {
                Serial.printf("[CLOUD] POST wrapper %dB TLV → %d\n", entry.len, code);
                if (response.length() > 0) {
                    Serial.printf("[CLOUD] Response: %s\n", response.c_str());
                }
                if (code >= 200 && code < 300) {
                    applyJournalVerification(entry.local_id, entry.source_id,
                                             BP_JOURNAL_VALIDATED);
                    queueCloudCommandResponse(response, pkt_device_id(entry.buf));
                } else if (code >= 400 && code < 500 && code != 408 && code != 429) {
                    applyJournalVerification(entry.local_id, entry.source_id,
                                             BP_JOURNAL_REJECTED);
                }
            } else {
                Serial.printf("[CLOUD] POST failed: %s\n", http.errorToString(code).c_str());
            }
            noteCloudPostResult(code);
            http.end();
        } else {
            updateConnectivityState();
            syncCloudSnapshotMetadata();
            replayPendingJournal();
        }
    }
}

// ═══════════════════════════════════════════════
// Command Builder — Hub → Collar
//
// Builds a TLV command packet, queues it for LoRa TX, and registers
// it in the pending ACK tracker so we can retry if the collar doesn't
// acknowledge it. The collar will send back a PKT_MODE_ACK,
// PKT_FIND_ACK, or PKT_STATUS_RESP with the original msg_seq.
// ═══════════════════════════════════════════════

// Convenience wrapper for mode commands (no buzzer/LED parameters)
static uint16_t sendCommand(uint16_t target_id, bp_pkt_type_t type, bp_profile_t mode) {
    return sendCommandFind(target_id, type, mode, 0, BUZZER_OFF);
}

// Convenience wrapper for status request commands.
static uint16_t sendStatusCommand(uint16_t target_id) {
    return sendCommandFind(target_id, PKT_CMD_STATUS, PROFILE_UNKNOWN, 0, BUZZER_OFF);
}

// Full command builder — handles both mode commands and find commands.
static uint16_t sendCommandFind(uint16_t target_id, bp_pkt_type_t type,
                              bp_profile_t mode, uint8_t ledFlash,
                              bp_buzzer_pattern_t buzzerPattern,
                              uint16_t sequenceOverride, uint32_t initialAgeMs) {
    if (initialAgeMs >= LOCAL_COMMAND_TTL_MS) return 0;
    cmd_entry_t cmd;
    uint8_t txReason = (uint8_t)type;      // Temporary downlink compatibility mapping
    uint16_t seq = sequenceOverride;
    if (seq == 0) {
        seq = (uint16_t)(++cmdSeqCounter & 0xFFFF); // Compact command sequence for ACK matching
        if (seq == 0) {
            seq = (uint16_t)(++cmdSeqCounter & 0xFFFF);
        }
    }

    // TLV v1.2 addresses the physical originator and logical recipient
    // independently: this hub originates the command and the collar is the
    // destination. The HMAC/reserved bytes cover both IDs.
    pkt_init(cmd.buf, (uint16_t)GATEWAY_GUID16, target_id, seq, 0,
             STATUS_HOME, PROFILE_NORMAL, 0, txReason);

    // Add TLV payload based on command type
    if (type == PKT_CMD_MODE && mode != PROFILE_UNKNOWN) {
        pkt_add_tlv_u8(cmd.buf, TLV_PROFILE, (uint8_t)mode);  // Which profile to switch to
    }

    if (type == PKT_CMD_FIND) {
        pkt_add_tlv_u8(cmd.buf, TLV_LED_FLASH, ledFlash > 0 ? ledFlash : 5);  // LED blink count
        pkt_add_tlv_u8(cmd.buf, TLV_BUZZER_PATTERN, (uint8_t)buzzerPattern);  // Which sound to play
    }

    // Finalize: append the v1.2 auth-tag bytes and return total packet length.
    cmd.len = pkt_finalize(cmd.buf);
    cmd.targetId = target_id;
    cmd.cmdSeq = seq;
    cmd.type = type;

    bool pendingRegistered = false;
    if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(50))) {
        // Cloud retries must not reset the expiry/ACK state or reuse an active
        // identity for different bytes. Local sequence collisions fail safely.
        for (const auto &p : pendingCmds) {
            if (p.state && p.cmdSeq == seq && p.targetId == target_id
                && millis() - p.createdAtMs < COMMAND_FEEDBACK_TTL_MS) {
                bool same = p.len == cmd.len && memcmp(p.buf, cmd.buf, cmd.len) == 0;
                bool stillActive = p.active && millis() - p.createdAtMs < LOCAL_COMMAND_TTL_MS;
                xSemaphoreGive(pendingMutex);
                return same && sequenceOverride && stillActive ? seq : 0;
            }
        }
        // A newer profile command supersedes an older unacknowledged profile
        // command for the same collar.
        if (type == PKT_CMD_MODE) {
            for (int i = 0; i < MAX_PENDING_CMDS; ++i) {
                if (pendingCmds[i].active && pendingCmds[i].targetId == target_id
                    && pendingCmds[i].type == PKT_CMD_MODE) {
                    pendingCmds[i].active = false;
                    pendingCmds[i].state = "superseded";
                    broadcastCommand(pendingCmds[i]);
                }
            }
        }
        // Prefer unused slots, then the oldest completed slot. Retain recent
        // feedback across browser reconnects within this bounded RAM cache.
        int slot = -1;
        for (int i = 0; i < MAX_PENDING_CMDS; ++i) {
            if (pendingCmds[i].active) continue;
            if (!pendingCmds[i].state) { slot = i; break; }
            if (slot < 0 || millis() - pendingCmds[i].createdAtMs > millis() - pendingCmds[slot].createdAtMs) slot = i;
        }
        if (slot >= 0) {
                int i = slot;
                pendingCmds[i].cmdSeq   = seq;
                pendingCmds[i].targetId = target_id;
                pendingCmds[i].type     = type;
                pendingCmds[i].sentAtMs = 0;
                pendingCmds[i].createdAtMs = millis() - initialAgeMs;
                pendingCmds[i].state = "queued";
                pendingCmds[i].retries  = 0;
                memcpy(pendingCmds[i].buf, cmd.buf, cmd.len);
                pendingCmds[i].len      = cmd.len;
                pendingCmds[i].active   = true;
                pendingCmds[i].waitingOpportunity = false;
                pendingRegistered = true;
        }
        xSemaphoreGive(pendingMutex);
    }

    if (!pendingRegistered) {
        Serial.printf("[CMD] No pending ACK slot available for %s\n",
                      bp_device_name(target_id));
        return 0;
    }

    // Queue the packet for the loraTask to transmit.
    if (xQueueSend(cmdQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.printf("[CMD] Queued type=0x%02X for %s (seq %u)\n",
                      type, bp_device_name(target_id), seq);
        if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(50))) {
            for (const auto &p : pendingCmds) {
                if (p.cmdSeq == seq && p.targetId == target_id) broadcastCommand(p);
            }
            xSemaphoreGive(pendingMutex);
        }
        return seq;
    } else {
        if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(50))) {
            for (int i = 0; i < MAX_PENDING_CMDS; i++) {
                if (pendingCmds[i].active && pendingCmds[i].cmdSeq == seq && pendingCmds[i].targetId == target_id) {
                    pendingCmds[i].active = false;
                    pendingCmds[i].state = "failed";
                    broadcastCommand(pendingCmds[i]);
                    break;
                }
            }
            xSemaphoreGive(pendingMutex);
        }
        Serial.printf("[CMD] Queue full; dropped command seq %u for %s\n",
                      seq, bp_device_name(target_id));
    }
    return 0;
}

// Convert the authenticated cloud queue response into the existing LoRa
// profile command. The backend sequence is retained verbatim so the collar's
// TLV ACK can close the correct database row.
static bool queueCloudCommandResponse(const String &response, uint16_t expectedDeviceId) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    if (error || !doc["command_pending"].as<bool>()) return false;

    JsonObject command = doc["command"].as<JsonObject>();
    uint16_t sequence = command["sequence_id"] | 0;
    const char *type = command["type"] | "";
    const char *profileName = command["payload"]["profile"] | "";
    if (sequence == 0) {
        Serial.println("[CLOUD CMD] Ignored command with invalid sequence");
        return false;
    }

    bp_profile_t profile = PROFILE_UNKNOWN;
    if (strcmp(type, "set_profile") == 0) {
        profile = bp_profile_from_name(profileName);
    } else if (strcmp(type, "enter_lost_alert") == 0) {
        profile = PROFILE_LOST;
    } else if (strcmp(type, "exit_lost_alert") == 0) {
        const char *fallback = command["payload"]["fallback_profile"] | "active";
        profile = bp_profile_from_name(fallback);
    }

    if (profile == PROFILE_UNKNOWN) {
        Serial.printf("[CLOUD CMD] Unsupported command '%s' for device %u\n", type, expectedDeviceId);
        return false;
    }

    // Respect the cloud's original deadline, not ten new minutes after relay.
    const char *expires = command["expires_at"] | "";
    struct tm expiryTm = {};
    time_t now = time(nullptr);
    if (!hubTimeSynced || !strptime(expires, "%Y-%m-%dT%H:%M:%S", &expiryTm)) {
        Serial.println("[CLOUD CMD] Waiting for an accurate clock and valid command expiry");
        return false;
    }
    // The hub's configTime uses UTC (zero timezone/DST offsets).
    time_t expiry = mktime(&expiryTm);
    if (expiry <= now) return false;
    uint32_t remaining = (uint32_t)std::min<time_t>(600, expiry - now) * 1000;
    if (!sendCommandFind(expectedDeviceId, PKT_CMD_MODE, profile, 0, BUZZER_OFF,
                         sequence, LOCAL_COMMAND_TTL_MS - remaining)) return false;
    Serial.printf("[CLOUD CMD] Queued %s seq=%u for device %u\n",
                  bp_profile_name(profile), sequence, expectedDeviceId);
    return true;
}

#include "hub_presence_impl.h"
