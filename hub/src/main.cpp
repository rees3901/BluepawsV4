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
  │    webTask   (core 0, prio 2) — HTTP server + SSE push      │
  │    bleTask   (core 0, prio 1) — BLE home beacon advertising │
  │    cloudTask (core 0, prio 2) — REST POST relay to cloud    │
  │  Main loop() yields to scheduler (does nothing).            │
  └─────────────────────────────────────────────────────────────┘
*/

// ── Arduino / ESP32 core ──
#include <Arduino.h>
#include <RadioLib.h>        // SX1262 LoRa radio driver
#include <WiFi.h>            // WiFi AP+STA dual mode
#include <WebServer.h>       // Lightweight HTTP server (port 80)
#include <LittleFS.h>        // On-chip flash filesystem (stores web files + logs)
#include <ESPmDNS.h>         // mDNS so you can browse to http://bluepaws.local
#include <BLEDevice.h>       // BLE for home beacon advertising
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <HTTPClient.h>      // HTTP client for cloud POST relay
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
#define LOG_FILE_PATH    "/log.csv"       // CSV telemetry log
#define MAX_LOG_ENTRIES  5000             // Rotate log after this many rows
#define CONFIG_FILE_PATH "/config.json"   // Saved WiFi/cloud credentials

// ═══════════════════════════════════════════════
// FreeRTOS Task Configuration
// Stack sizes are in bytes. Priority: higher number = higher priority.
// ═══════════════════════════════════════════════
#define STACK_LORA   4096   // LoRa RX/TX — moderate stack (radio + crypto)
#define STACK_WEB    8192   // Web server — large stack (JSON building, string ops)
#define STACK_BLE    4096   // BLE beacon + scanning in portable mode
#define STACK_CLOUD  16384  // Cloud relay — HTTPS/TLS + HTTPClient need generous stack

#define PRIO_LORA    3      // Highest — LoRa packets are time-sensitive
#define PRIO_WEB     2      // Medium — serves the GUI
#define PRIO_CLOUD   2      // Medium — cloud relay not urgent
#define PRIO_BLE     1      // Lowest — BLE beacon just needs to stay alive

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

// ── SSE (Server-Sent Events) ──
// Instead of WebSocket, we use SSE for real-time push to the browser.
// SSE is simpler (no extra library), uni-directional (server→client),
// and works in all modern browsers. The client opens GET /events and
// the server pushes "event: <type>\ndata: <json>\n\n" lines.
// We support up to 4 simultaneous SSE clients.
static WiFiClient sseClients[4];          // Connected SSE client sockets
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
};

// ── Legacy AES-128 key ──
// TLV v1.1 uplink packets are raw authenticated TLV over private LoRa. Keep
// this only until the downlink command protocol is revised.
static const uint8_t aesKey[16] = LORA_AES_KEY;

// ── Packet Statistics (displayed in web GUI status) ──
static uint32_t rxCount = 0;         // Total valid packets received
static uint32_t crcFailCount = 0;    // Packets that failed TLV structure/auth checks
static uint32_t txCount = 0;         // Total commands transmitted
static uint32_t cmdSeqCounter = 0;   // Incrementing sequence for outgoing commands

// ── Pending Command ACK Tracking ──
// When we send a command to a collar, we track it here and wait for
// an ACK packet back. If no ACK within CMD_ACK_TIMEOUT_MS, we retry
// up to CMD_MAX_RETRIES times before marking it expired.
struct pending_cmd_t {
    uint32_t cmdSeq;        // msg_seq we assigned to this command
    uint16_t targetId;      // device_id of the target collar
    bp_pkt_type_t type;     // command type (PKT_CMD_MODE, PKT_CMD_FIND, etc.)
    uint32_t sentAtMs;      // millis() timestamp when last sent
    uint8_t  retries;       // how many retransmissions so far
    uint8_t  buf[BP_MAX_PACKET_SIZE];  // original packet (for retransmission)
    uint8_t  len;           // packet length
    bool     active;        // true = this slot is tracking a pending command
};

#define MAX_PENDING_CMDS 4                          // Up to 4 commands in-flight at once
static pending_cmd_t pendingCmds[MAX_PENDING_CMDS];
static SemaphoreHandle_t pendingMutex = NULL;       // Protects pendingCmds array

// ── WiFi / Cloud State ──
static bool staConnected = false;             // true if connected to home router
static String staSSID = WIFI_STA_SSID;        // Current STA SSID (may be loaded from config file)
static String staPass = WIFI_STA_PASS;        // Current STA password
static String cloudEndpoint = CLOUD_ENDPOINT; // Cloud POST URL
static String cloudToken = CLOUD_BEARER_TOKEN; // Gateway bearer token for Supabase Edge Function
static bool hubProvisioningMode = HUB_PROVISIONING_MODE_DEFAULT;
static bool hubApEnabled = false;
static bool hubTimeSynced = false;
static uint32_t lastNtpSyncMs = 0;

// ── Hub Communications Profile ──
// This is the user-selected operating profile. It must not flap merely because
// WiFi, phone hotspot, or cloud reachability changes temporarily.
enum hub_comm_profile_t : uint8_t {
    HUB_COMM_HOME = 0,       // Fixed at home, normal household WiFi/cloud path.
    HUB_COMM_PORTABLE = 1,   // User deliberately took the hub roaming; cloud is best-effort.
    HUB_COMM_OFF_GRID = 2,   // Local-only roaming/search mode; cloud relay intentionally disabled.
};

static hub_comm_profile_t hubCommProfile = HUB_COMM_HOME;

struct hub_connectivity_t {
    bool wifi_connected;
    bool internet_reachable;       // Placeholder for future active internet probe.
    bool cloud_reachable;
    bool lora_rx_active;
    uint32_t last_cloud_success_ms;
    uint8_t cloud_failures;
};

static hub_connectivity_t hubConnectivity = {
    false, false, false, false, 0, 0
};

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
static uint32_t logEntryCount = 0;  // Number of CSV rows in the log file

// ── FreeRTOS Task Handles ──
static TaskHandle_t loraTaskHandle  = NULL;
static TaskHandle_t webTaskHandle   = NULL;
static TaskHandle_t bleTaskHandle   = NULL;
static TaskHandle_t cloudTaskHandle = NULL;

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
    uint8_t  error;          // bp_error_t — active subsystem fault (0 = none)
    int16_t  rssi;           // LoRa RSSI when hub received the packet (dBm)
    float    snr;            // LoRa SNR when hub received the packet (dB)
    uint32_t local_millis;   // millis() on the hub when this packet arrived
    bool     has_gps;        // true if collar had a valid GPS fix
};

static device_state_t devices[MAX_DEVICES];    // Device state table
static uint8_t deviceCount = 0;                // How many unique collars we've seen
static SemaphoreHandle_t deviceMutex = NULL;   // Protects devices[] array

// ═══════════════════════════════════════════════
// Forward Declarations
// ═══════════════════════════════════════════════

// FreeRTOS task entry points
static void loraTask(void *param);
static void webTask(void *param);
static void bleTask(void *param);
static void cloudTask(void *param);

// Hardware initialisation
static void initLoRa();
static void femInit();
static void femSetRx();
static void femSetTx();
static void initWiFi();
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
                             char *out, size_t outLen);
static void updateDeviceState(const uint8_t *buf, int16_t rssi, float snr);
static void logToStorage(const uint8_t *buf, uint8_t len, int16_t rssi, float snr);
static void sseBroadcast(const char *event, const char *data);
static String base64Encode(const uint8_t *data, uint8_t len);
static String buildCloudWrapperJson(const cloud_entry_t &entry);
static device_state_t *findDevice(uint16_t id);

// Command building & ACK tracking
static void sendCommand(uint16_t target_id, bp_pkt_type_t type, bp_profile_t mode);
static void sendCommandFind(uint16_t target_id, bp_pkt_type_t type,
                              bp_profile_t mode, uint8_t ledFlash,
                              bp_buzzer_pattern_t buzzerPattern);
static void sendStatusCommand(uint16_t target_id);
static void checkPendingAcks();
static void handleAck(const uint8_t *buf);

// Portable mode (BLE scanning for collar find beacons)
static void enterPortableMode();
static void enterHomeMode();
static void enterOffGridMode();
static void handleApiBle();
static void handleApiHubMode();

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

    // Zero out the pending command tracking slots
    memset(pendingCmds, 0, sizeof(pendingCmds));

    // Initialise hardware subsystems (order matters: storage first to load config)
    initStorage();  // Mount LittleFS, load saved WiFi/cloud config
    initLoRa();     // SPI + SX1262 radio setup, start listening
    initWiFi();     // AP + STA + mDNS
    initBLE();      // BLE home beacon advertising

    // Create FreeRTOS tasks, each pinned to a specific core.
    // ESP32-S3 has 2 cores: core 0 handles WiFi/BLE, core 1 is free for LoRa.
    // Pinning LoRa to core 1 avoids WiFi interrupt contention.
    xTaskCreatePinnedToCore(loraTask,  "lora",  STACK_LORA,  NULL, PRIO_LORA,  &loraTaskHandle,  1);  // Core 1
    xTaskCreatePinnedToCore(webTask,   "web",   STACK_WEB,   NULL, PRIO_WEB,   &webTaskHandle,   0);  // Core 0
    xTaskCreatePinnedToCore(bleTask,   "ble",   STACK_BLE,   NULL, PRIO_BLE,   &bleTaskHandle,   0);  // Core 0
    xTaskCreatePinnedToCore(cloudTask, "cloud", STACK_CLOUD, NULL, PRIO_CLOUD, &cloudTaskHandle, 0);  // Core 0

    Serial.println("[INIT] All tasks started");
}

// Arduino loop() — does nothing. All work happens in FreeRTOS tasks.
// We just sleep forever so the scheduler can run the real tasks.
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// ═══════════════════════════════════════════════
// Hub Communications Profile / Connectivity Health
//
// The selected profile is intentional user state. Connectivity is passive
// health state. Do not switch HOME/PORTABLE/OFF_GRID just because WiFi or cloud
// goes up/down.
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

    hubCommProfile = profile;
    Serial.printf("[HUB] Communications profile → %s\n", hubCommProfileName(hubCommProfile));
    applyWifiRoleForCurrentProfile();
    applyBleRoleForCurrentProfile();
}

static void applyWifiRoleForCurrentProfile() {
    bool shouldEnableAp = hubProfileNeedsLocalAp();
    wifi_mode_t desiredMode = shouldEnableAp ? WIFI_AP_STA : WIFI_STA;

    if (WiFi.getMode() != desiredMode) {
        WiFi.mode(desiredMode);
    }

    if (shouldEnableAp && !hubApEnabled) {
        WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHANNEL);
        hubApEnabled = true;
        Serial.printf("[WIFI] AP enabled: %s @ %s\n",
                      WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
    } else if (!shouldEnableAp && hubApEnabled) {
        WiFi.softAPdisconnect(true);
        hubApEnabled = false;
        WiFi.mode(WIFI_STA);
        Serial.println("[WIFI] AP disabled for normal Home/Portable STA mode");
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
    Serial.println("[LORA] TLV v1.1 raw binary uplink enabled");
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

static void initWiFi() {
    // Intentional Wi-Fi roles:
    //   Home/Portable: STA for uplink/local LAN; AP off unless provisioning.
    //   Off-Grid: AP/local GUI is primary; cloud relay is disabled.
    WiFi.mode(hubProfileNeedsLocalAp() ? WIFI_AP_STA : WIFI_STA);
    applyWifiRoleForCurrentProfile();

    // If we have saved STA credentials, try to connect to home router.
    // This gives us internet access for cloud relay.
    // Timeout after 10 seconds — don't block forever.
    if (staSSID.length() > 0) {
        Serial.printf("[WIFI] Connecting to '%s'...\n", staSSID.c_str());
        WiFi.begin(staSSID.c_str(), staPass.c_str());
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(250);
        }
        if (WiFi.status() == WL_CONNECTED) {
            staConnected = true;
            hubConnectivity.wifi_connected = true;
            Serial.printf("[WIFI] STA connected: %s\n", WiFi.localIP().toString().c_str());
            syncHubClock(true);
        } else {
            updateConnectivityState();
            Serial.println("[WIFI] STA connection failed — LoRa RX remains active");
        }
    }

    // Register mDNS so the hub is reachable at http://bluepaws.local
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        Serial.printf("[MDNS] http://%s.local\n", MDNS_HOSTNAME);
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

    if (hubProfileUsesBleScanning()) {
        // Portable/Off-Grid was persisted — start scanning instead of advertising.
        Serial.printf("[BLE] %s profile restored from config\n",
                      hubCommProfileName(hubCommProfile));
        applyBleRoleForCurrentProfile();
    } else {
        pAdv->start();  // Begin broadcasting — runs autonomously in background
        Serial.printf("[BLE] Beacon advertising: %s\n", BLE_HOME_BEACON_NAME);
    }
}

static void initStorage() {
    // Mount LittleFS (on-chip flash filesystem). The 'true' parameter
    // auto-formats the partition if it's not already formatted.
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed!");
        return;
    }
    Serial.println("[FS] LittleFS mounted");

    // Count existing log entries so we know when to rotate.
    // We just count newline characters (one per CSV row).
    if (LittleFS.exists(LOG_FILE_PATH)) {
        File f = LittleFS.open(LOG_FILE_PATH, "r");
        if (f) {
            while (f.available()) {
                if (f.read() == '\n') logEntryCount++;
            }
            f.close();
        }
        Serial.printf("[FS] Existing log: %u entries\n", logEntryCount);
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
        if ((now - lastCmdTx) >= pdMS_TO_TICKS(CMD_QUEUE_INTERVAL_MS)) {
            if (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
                // Take SPI mutex, transmit, then go back to RX mode
                if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(200))) {
                    femSetTx();
                    int state = lora.transmit(cmd.buf, cmd.len);
                    femSetRx();
                    lora.startReceive();  // Always return to RX after TX
                    xSemaphoreGive(loraMutex);

                    if (state == RADIOLIB_ERR_NONE) {
                        txCount++;
                        Serial.printf("[LORA] CMD TX %d bytes\n", cmd.len);
                    } else {
                        Serial.printf("[LORA] TX failed: %d\n", state);
                    }
                    lastCmdTx = now;
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
    if (!pkt_tlv_get_u32(buf, TLV_CMD_MSG_ID, &ackedSeq)) return;

    if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(50))) {
        for (int i = 0; i < MAX_PENDING_CMDS; i++) {
            if (pendingCmds[i].active && pendingCmds[i].cmdSeq == ackedSeq) {
                // Found the matching command — calculate round-trip time
                uint32_t rtt = millis() - pendingCmds[i].sentAtMs;
                Serial.printf("[ACK] Cmd seq %lu ACK'd by %s (RTT %lums)\n",
                              ackedSeq, bp_device_name(pkt_device_id(buf)), rtt);
                pendingCmds[i].active = false;  // Free this slot

                // Notify the web GUI that the command was acknowledged
                char json[128];
                snprintf(json, sizeof(json),
                    "{\"cmdSeq\":%u,\"device\":%u,\"rtt\":%u,\"status\":\"acked\"}",
                    ackedSeq, pkt_device_id(buf), rtt);
                sseBroadcast("cmd_ack", json);
                break;
            }
        }
        xSemaphoreGive(pendingMutex);
    }
}

// Called every loop iteration by loraTask.
// Checks if any pending commands have timed out waiting for ACK.
// If so, retransmit up to CMD_MAX_RETRIES times, then expire.
static void checkPendingAcks() {
    if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(20))) {
        uint32_t now = millis();
        for (int i = 0; i < MAX_PENDING_CMDS; i++) {
            if (!pendingCmds[i].active) continue;                      // Skip unused slots
            if (now - pendingCmds[i].sentAtMs < CMD_ACK_TIMEOUT_MS) continue;  // Not timed out yet

            if (pendingCmds[i].retries < CMD_MAX_RETRIES) {
                // Haven't exhausted retries — re-queue the packet for TX
                pendingCmds[i].retries++;
                pendingCmds[i].sentAtMs = now;  // Reset timeout timer

                cmd_entry_t cmd;
                memcpy(cmd.buf, pendingCmds[i].buf, pendingCmds[i].len);
                cmd.len = pendingCmds[i].len;
                xQueueSend(cmdQueue, &cmd, 0);  // Put back in TX queue

                Serial.printf("[ACK] Retry %d/%d for seq %lu → %s\n",
                              pendingCmds[i].retries, CMD_MAX_RETRIES,
                              pendingCmds[i].cmdSeq,
                              bp_device_name(pendingCmds[i].targetId));
            } else {
                // All retries exhausted — give up on this command
                Serial.printf("[ACK] EXPIRED seq %lu → %s (no ACK after %d retries)\n",
                              pendingCmds[i].cmdSeq,
                              bp_device_name(pendingCmds[i].targetId),
                              CMD_MAX_RETRIES);

                // Tell the web GUI the command failed
                char json[128];
                snprintf(json, sizeof(json),
                    "{\"cmdSeq\":%u,\"device\":%u,\"status\":\"expired\"}",
                    pendingCmds[i].cmdSeq, pendingCmds[i].targetId);
                sseBroadcast("cmd_ack", json);

                pendingCmds[i].active = false;  // Free this slot
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
    // Step 1: Validate TLV v1.1 structure. Supabase validates HMAC.
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
    uint16_t devId = pkt_device_id(buf);
    uint8_t pktType = pkt_pkt_type(buf);

    Serial.printf("[LORA] RX #%u from %s | type=0x%02X rssi=%d snr=%.1f\n",
                  rxCount, bp_device_name(devId), pktType, rssi, snr);

    // Step 3: If this is an ACK/response to a command we sent, match it up
    if (pktType == PKT_MODE_ACK || pktType == PKT_FIND_ACK || pktType == PKT_STATUS_RESP) {
        handleAck(buf);
    }

    // Step 4: Store/update the device's latest telemetry in our state table
    updateDeviceState(buf, rssi, snr);

    // Step 5: Append a CSV row to the on-chip log file
    logToStorage(buf, len, rssi, snr);

    // Step 6: Queue the raw packet for the cloudTask to POST to the server.
    // If the queue is full, we silently drop — cloud relay is best-effort.
    cloud_entry_t ce;
    memcpy(ce.buf, buf, len);
    ce.len  = len;
    ce.rssi = rssi;
    ce.snr  = snr;
    time_t rxTime = time(nullptr);
    ce.gateway_rx_time_unix = unixTimeLooksValid(rxTime)
        ? (uint32_t)rxTime
        : pkt_time_unix(buf);
    xQueueSend(cloudQueue, &ce, 0);

    // Step 7: Push the telemetry as JSON to all connected web browsers via SSE
    char jsonBuf[384];
    buildDeviceJson(buf, rssi, snr, jsonBuf, sizeof(jsonBuf));
    sseBroadcast("telemetry", jsonBuf);
}

// Build a JSON string from a raw packet for the web GUI.
// This JSON gets pushed via SSE ("telemetry" event) and also used
// in the /api/devices endpoint. The browser parses it to update
// the device card and map marker.
static void buildDeviceJson(const uint8_t *buf, int16_t rssi, float snr,
                             char *out, size_t outLen) {
    uint16_t devId    = pkt_device_id(buf);
    uint16_t flags    = pkt_flags(buf);
    bool hasGps       = (flags & FLAG_HAS_GPS) != 0;
    double lat        = hasGps ? pkt_lat_e7(buf) / 1e7 : 0.0;  // Convert from integer×10^7 to degrees
    double lon        = hasGps ? pkt_lon_e7(buf) / 1e7 : 0.0;
    uint8_t profile   = pkt_power_profile(buf);
    uint8_t error     = 0;
    pkt_tlv_get_u8(buf, TLV_RESET_REASON, &error);

    snprintf(out, outLen,
        "{\"id\":%u,\"name\":\"%s\",\"seq\":%u,\"time\":%u,"
        "\"status\":\"%s\",\"profile\":\"%s\",\"error\":\"%s\","
        "\"lat\":%.7f,\"lon\":%.7f,\"hasGps\":%s,"
        "\"batt\":%u,\"acc\":%u,\"fixAge\":%u,"
        "\"rssi\":%d,\"snr\":%.1f,"
        "\"bleHome\":%s,\"cellular\":false,\"txReason\":\"%s\"}",
        devId, bp_device_name(devId), pkt_msg_seq(buf), pkt_time_unix(buf),
        bp_status_display((bp_status_t)pkt_status(buf)),
        bp_profile_name((bp_profile_t)profile),
        bp_error_display((bp_error_t)error),
        lat, lon, hasGps ? "true" : "false",
        pkt_batt_mV(buf), pkt_acc_m(buf), pkt_fix_age_s(buf),
        rssi, snr,
        (flags & FLAG_HOME_BEACON_SEEN) ? "true" : "false",
        bp_tx_reason_display(pkt_tx_reason(buf))
    );
}

// Update our in-memory device state table with the latest packet data.
// This is what the web GUI reads when a new browser connects.
static void updateDeviceState(const uint8_t *buf, int16_t rssi, float snr) {
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
            dev->rssi        = rssi;
            dev->snr         = snr;
            dev->local_millis = millis();  // Record when WE received it (for "last seen" age)
            dev->has_gps     = (flags & FLAG_HAS_GPS) != 0;
            if (dev->has_gps) {
                dev->lat_e7 = pkt_lat_e7(buf);
                dev->lon_e7 = pkt_lon_e7(buf);
            }
            dev->profile = pkt_power_profile(buf);
            dev->error = 0;
            pkt_tlv_get_u8(buf, TLV_RESET_REASON, &dev->error);
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

// ═══════════════════════════════════════════════
// Storage — CSV Log
//
// Every received telemetry packet is appended as one CSV row to
// /log.csv on the ESP32's flash. This provides a local history
// even without cloud connectivity. The log auto-rotates (deletes
// and restarts) after MAX_LOG_ENTRIES rows to avoid filling flash.
// ═══════════════════════════════════════════════

static void logToStorage(const uint8_t *buf, uint8_t len, int16_t rssi, float snr) {
    File f = LittleFS.open(LOG_FILE_PATH, "a");  // Open for append
    if (!f) return;

    uint16_t devId = pkt_device_id(buf);
    uint16_t flags = pkt_flags(buf);
    bool hasGps    = (flags & FLAG_HAS_GPS) != 0;

    uint8_t profile = pkt_power_profile(buf);

    // CSV columns: timestamp, device_id, msg_seq, status, lat, lon, batt_mV, rssi, snr, profile
    f.printf("%u,%u,%u,%u,%.7f,%.7f,%u,%d,%.1f,%u\n",
        pkt_time_unix(buf),
        devId,
        pkt_msg_seq(buf),
        pkt_status(buf),
        hasGps ? pkt_lat_e7(buf) / 1e7 : 0.0,
        hasGps ? pkt_lon_e7(buf) / 1e7 : 0.0,
        pkt_batt_mV(buf),
        rssi,
        snr,
        profile
    );
    f.close();

    logEntryCount++;

    // Simple log rotation: delete the entire file once it hits the limit.
    // A circular buffer would be more elegant but this is fine for flash wear.
    if (logEntryCount > MAX_LOG_ENTRIES) {
        LittleFS.remove(LOG_FILE_PATH);
        logEntryCount = 0;
        Serial.println("[FS] Log rotated");
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
            if (sseClients[i].connected()) {
                // Send the SSE-formatted message
                sseClients[i].printf("event: %s\ndata: %s\n\n", event, data);
            } else {
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

// Serve the main HTML page from flash
static void handleRoot() {
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

    // Register this client in our SSE client list (max 4 simultaneous)
    if (xSemaphoreTake(sseMutex, pdMS_TO_TICKS(100))) {
        if (sseClientCount < 4) {
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
            char json[384];
            snprintf(json, sizeof(json),
                "{\"id\":%u,\"name\":\"%s\",\"seq\":%u,\"time\":%u,"
                "\"status\":\"%s\",\"profile\":\"%s\","
                "\"lat\":%.7f,\"lon\":%.7f,\"hasGps\":%s,"
                "\"batt\":%u,\"acc\":%u,\"fixAge\":%u,"
                "\"rssi\":%d,\"snr\":%.1f,\"bleHome\":false,\"cellular\":false}",
                d->device_id, bp_device_name(d->device_id),
                d->last_seq, d->last_time,
                bp_status_display((bp_status_t)d->status),
                bp_profile_name((bp_profile_t)d->profile),
                d->has_gps ? d->lat_e7 / 1e7 : 0.0,
                d->has_gps ? d->lon_e7 / 1e7 : 0.0,
                d->has_gps ? "true" : "false",
                d->batt_mV, d->acc_m, d->fix_age_s,
                d->rssi, d->snr
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
            char buf[384];
            snprintf(buf, sizeof(buf),
                "{\"id\":%u,\"name\":\"%s\",\"seq\":%u,\"time\":%u,"
                "\"status\":\"%s\",\"profile\":\"%s\","
                "\"lat\":%.7f,\"lon\":%.7f,\"hasGps\":%s,"
                "\"batt\":%u,\"acc\":%u,\"fixAge\":%u,"
                "\"rssi\":%d,\"snr\":%.1f,\"age\":%u}",
                d->device_id, bp_device_name(d->device_id),
                d->last_seq, d->last_time,
                bp_status_display((bp_status_t)d->status),
                bp_profile_name((bp_profile_t)d->profile),
                d->has_gps ? d->lat_e7 / 1e7 : 0.0,
                d->has_gps ? d->lon_e7 / 1e7 : 0.0,
                d->has_gps ? "true" : "false",
                d->batt_mV, d->acc_m, d->fix_age_s,
                d->rssi, d->snr,
                (millis() - d->local_millis) / 1000
            );
            json += buf;
        }
        xSemaphoreGive(deviceMutex);
    }
    json += "]";
    httpServer.send(200, "application/json", json);
}

// ── API: GET /api/status ──
// Returns hub diagnostic info: uptime, packet counts, memory, WiFi state.
// Displayed in the Settings modal in the web GUI.
static void handleApiStatus() {
    updateConnectivityState();

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"uptime\":%u,\"rxCount\":%u,\"txCount\":%u,"
        "\"crcFails\":%u,\"devices\":%u,\"logEntries\":%u,"
        "\"staConnected\":%s,\"staIP\":\"%s\",\"apEnabled\":%s,\"apIP\":\"%s\","
        "\"freeHeap\":%u,\"hubMode\":\"%s\","
        "\"provisioning_mode\":%s,\"time_synced\":%s,\"hub_time_unix\":%u,"
        "\"mode\":\"%s\",\"wifi_connected\":%s,"
        "\"internet_reachable\":%s,\"cloud_reachable\":%s,"
        "\"last_cloud_success_ms\":%u,\"lora_rx_active\":%s}",
        millis() / 1000, rxCount, txCount,
        crcFailCount, deviceCount, logEntryCount,
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
        hubConnectivity.last_cloud_success_ms,
        hubConnectivity.lora_rx_active ? "true" : "false"
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

// ── API: POST /api/command ──
// Sends a mode-change command to a collar.
// Body format: device=XXXX&mode=normal|active|lost|powersave
// The device ID is in hex (e.g. "0001"), mode is the profile name.
static void handleApiCommand() {
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "text/plain", "POST only");
        return;
    }

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
    sendCommand(targetId, PKT_CMD_MODE, mode);
    httpServer.send(200, "application/json", "{\"ok\":true}");
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
    sendCommandFind(targetId, PKT_CMD_FIND, PROFILE_UNKNOWN, flashCount, pattern);

    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"device\":%u,\"pattern\":%u,\"flash\":%u}",
        targetId, pattern, flashCount);
    httpServer.send(200, "application/json", resp);
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

    sendStatusCommand(targetId);

    char resp[128];
    snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"device\":%u,\"command\":\"status\"}",
        targetId);
    httpServer.send(200, "application/json", resp);
}

// ── API: POST /api/config ──
// Saves WiFi and cloud settings to flash, then restarts the ESP32
// so the new WiFi credentials take effect.
// Body format: ssid=MyNetwork&pass=MyPassword&cloud_url=https://...
static void handleApiConfig() {
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "text/plain", "POST only");
        return;
    }

    String body = httpServer.arg("plain");

    // Helper lambda to extract a URL-encoded parameter value
    auto parseParam = [&](const char *key) -> String {
        String k = String(key) + "=";
        int idx = body.indexOf(k);
        if (idx < 0) return "";
        String val = body.substring(idx + k.length());
        int amp = val.indexOf('&');
        if (amp >= 0) val = val.substring(0, amp);
        return val;
    };

    String newSSID  = parseParam("ssid");
    String newPass  = parseParam("pass");
    String newCloud = parseParam("cloud_url");
    String newToken = parseParam("cloud_token");

    // Write config to flash as simple key=value text file
    File f = LittleFS.open(CONFIG_FILE_PATH, "w");
    if (f) {
        if (newSSID.length() > 0)  f.printf("sta_ssid=%s\n", newSSID.c_str());
        if (newPass.length() > 0)  f.printf("sta_pass=%s\n", newPass.c_str());
        if (newCloud.length() > 0) f.printf("cloud_url=%s\n", newCloud.c_str());
        if (newToken.length() > 0) f.printf("cloud_token=%s\n", newToken.c_str());
        f.printf("hub_mode=%s\n", hubCommProfileName(hubCommProfile));
        f.close();
    }

    // Update in-memory state
    staSSID = newSSID;
    staPass = newPass;
    if (newCloud.length() > 0) cloudEndpoint = newCloud;
    if (newToken.length() > 0) cloudToken = newToken;

    httpServer.send(200, "application/json", "{\"ok\":true,\"restart\":true}");

    // Restart the ESP32 so WiFi reconnects with new credentials
    delay(500);
    ESP.restart();
}

// Catch-all handler for any URL not matched by explicit routes.
// Tries to serve the file from LittleFS (e.g. favicon.ico, images).
static void handleNotFound() {
    String path = httpServer.uri();
    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        // Determine MIME type from file extension
        String contentType = "text/plain";
        if (path.endsWith(".html"))      contentType = "text/html";
        else if (path.endsWith(".css"))   contentType = "text/css";
        else if (path.endsWith(".js"))    contentType = "application/javascript";
        else if (path.endsWith(".json"))  contentType = "application/json";
        else if (path.endsWith(".png"))   contentType = "image/png";
        else if (path.endsWith(".ico"))   contentType = "image/x-icon";
        httpServer.streamFile(f, contentType);
        f.close();
        return;
    }
    httpServer.send(404, "text/plain", "Not found");
}

// Register all HTTP routes and start the web server
static void initWebServer() {
    // Static file routes
    httpServer.on("/",             HTTP_GET,  handleRoot);     // Main page
    httpServer.on("/style.css",    HTTP_GET,  handleCSS);      // Stylesheet
    httpServer.on("/app.js",       HTTP_GET,  handleJS);       // JavaScript app
    // SSE real-time event stream
    httpServer.on("/events",       HTTP_GET,  handleEvents);
    // REST API endpoints
    httpServer.on("/api/devices",  HTTP_GET,  handleApiDevices);  // Get all device states
    httpServer.on("/api/status",   HTTP_GET,  handleApiStatus);   // Get hub diagnostics
    httpServer.on("/api/command",  HTTP_POST, handleApiCommand);  // Send mode command
    httpServer.on("/api/device-status", HTTP_POST, handleApiDeviceStatusCommand); // Request collar status
    httpServer.on("/api/find",     HTTP_POST, handleApiFind);     // Trigger find (buzzer+LED)
    httpServer.on("/api/config",   HTTP_POST, handleApiConfig);   // Save WiFi/cloud config
    httpServer.on("/api/ble",      HTTP_GET,  handleApiBle);       // BLE scan results (portable mode)
    httpServer.on("/api/hub-mode", HTTP_POST, handleApiHubMode);   // Toggle home/portable mode
    httpServer.onNotFound(handleNotFound);                        // Serve other files from flash
    httpServer.begin();
    Serial.printf("[WEB] HTTP server on port %d\n", HTTP_PORT);
}

// Web task — runs on core 0. Handles incoming HTTP requests and
// sends periodic SSE heartbeats so the browser knows the connection is alive.
static void webTask(void *param) {
    (void)param;
    initWebServer();

    TickType_t lastHeartbeat = 0;
    TickType_t lastConnectivityCheck = 0;

    for (;;) {
        // Process any pending HTTP requests (non-blocking)
        httpServer.handleClient();

        // Send a heartbeat event every 5 seconds. The browser uses this
        // to detect if the SSE connection has dropped (10s watchdog).
        TickType_t now = xTaskGetTickCount();
        if (now - lastHeartbeat >= pdMS_TO_TICKS(5000)) {
            lastHeartbeat = now;
            sseBroadcast("heartbeat", "{}");
        }

        // Keep connectivity health fresh without ever disturbing LoRa RX or
        // the local AP. Home and Portable may try to reconnect STA; Off-Grid
        // remains local-only by design.
        if (now - lastConnectivityCheck >= pdMS_TO_TICKS(10000)) {
            lastConnectivityCheck = now;
            updateConnectivityState();
            if (hubProfileAllowsCloudRelay()
                && !hubConnectivity.wifi_connected
                && staSSID.length() > 0) {
                Serial.printf("[WIFI] STA reconnect attempt in %s profile\n",
                              hubCommProfileName(hubCommProfile));
                WiFi.begin(staSSID.c_str(), staPass.c_str());
            }
        }

        // 5ms sleep — fast enough for responsive web UI
        vTaskDelay(pdMS_TO_TICKS(5));
    }
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
    if (hubProfileUsesBleScanning()) {
        // Stop home beacon
        BLEDevice::getAdvertising()->stop();
        Serial.println("[BLE] Home beacon stopped");

        // Start BLE scanning for collar find beacons
        pBLEScan = BLEDevice::getScan();
        pBLEScan->setAdvertisedDeviceCallbacks(&findBeaconCb, true);
        pBLEScan->setActiveScan(true);
        pBLEScan->setInterval(160);   // 100ms in 0.625ms units
        pBLEScan->setWindow(128);     // 80ms in 0.625ms units

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

    // Restart home beacon
    BLEDevice::getAdvertising()->start();
    Serial.println("[HUB] HOME profile — beacon advertising");
}

static void enterPortableMode() {
    setHubCommunicationsProfile(HUB_COMM_PORTABLE);
}

static void enterHomeMode() {
    setHubCommunicationsProfile(HUB_COMM_HOME);
}

static void enterOffGridMode() {
    setHubCommunicationsProfile(HUB_COMM_OFF_GRID);
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
    if (body.indexOf("mode=portable") >= 0) {
        enterPortableMode();
    } else if (body.indexOf("mode=off_grid") >= 0 || body.indexOf("mode=off-grid") >= 0) {
        enterOffGridMode();
    } else if (body.indexOf("mode=home") >= 0) {
        enterHomeMode();
    } else {
        httpServer.send(400, "text/plain", "Bad request: mode=home|portable|off_grid");
        return;
    }

    char resp[40];
    snprintf(resp, sizeof(resp), "{\"mode\":\"%s\"}", hubCommProfileName(hubCommProfile));
    httpServer.send(200, "application/json", resp);
}

// ═══════════════════════════════════════════════
// BLE Task — Home Beacon / Portable Scanner
//
// In Home mode: periodically restarts BLE advertising as a keepalive.
// In Portable/Off-Grid mode: runs BLE scans and expires stale results.
// ═══════════════════════════════════════════════

static void bleTask(void *param) {
    (void)param;
    for (;;) {
        if (hubProfileUsesBleScanning()) {
            // Run a 5-second scan cycle
            if (pBLEScan) {
                pBLEScan->start(5, false);
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
            // Home mode — just keep beacon alive
            BLEAdvertising *pAdv = BLEDevice::getAdvertising();
            if (pAdv) pAdv->start();
            vTaskDelay(pdMS_TO_TICKS(30000));
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
            if (code > 0) {
                Serial.printf("[CLOUD] POST wrapper %dB TLV → %d\n", entry.len, code);
            } else {
                Serial.printf("[CLOUD] POST failed: %s\n", http.errorToString(code).c_str());
            }
            noteCloudPostResult(code);
            http.end();
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
static void sendCommand(uint16_t target_id, bp_pkt_type_t type, bp_profile_t mode) {
    sendCommandFind(target_id, type, mode, 0, BUZZER_OFF);
}

// Convenience wrapper for status request commands.
static void sendStatusCommand(uint16_t target_id) {
    sendCommandFind(target_id, PKT_CMD_STATUS, PROFILE_UNKNOWN, 0, BUZZER_OFF);
}

// Full command builder — handles both mode commands and find commands.
static void sendCommandFind(uint16_t target_id, bp_pkt_type_t type,
                              bp_profile_t mode, uint8_t ledFlash,
                              bp_buzzer_pattern_t buzzerPattern) {
    cmd_entry_t cmd;
    uint8_t txReason = (uint8_t)type;      // Temporary downlink compatibility mapping
    uint32_t seq = ++cmdSeqCounter;        // Unique sequence number for ACK matching

    // Build the fixed header. Downlink commands still use the compatibility
    // pkt_type names until the command protocol is formalized separately.
    pkt_init(cmd.buf, DEVICE_ID_HUB, seq, 0, STATUS_OK, txReason);

    // Add TLV payload based on command type
    if (type == PKT_CMD_MODE && mode != PROFILE_UNKNOWN) {
        pkt_add_tlv_u8(cmd.buf, TLV_PROFILE, (uint8_t)mode);  // Which profile to switch to
    }

    if (type == PKT_CMD_FIND) {
        pkt_add_tlv_u8(cmd.buf, TLV_LED_FLASH, ledFlash > 0 ? ledFlash : 5);  // LED blink count
        pkt_add_tlv_u8(cmd.buf, TLV_BUZZER_PATTERN, (uint8_t)buzzerPattern);  // Which sound to play
    }

    // Override the device_id field with the TARGET collar's ID
    // (pkt_init set it to DEVICE_ID_HUB, but commands are addressed to the collar)
    memcpy(&cmd.buf[1], &target_id, 2);

    // Finalize: append the v1.1 auth-tag bytes and return total packet length.
    cmd.len = pkt_finalize(cmd.buf);

    // Queue the packet for the loraTask to transmit
    if (xQueueSend(cmdQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.printf("[CMD] Queued type=0x%02X for %s (seq %lu)\n",
                      type, bp_device_name(target_id), seq);

        // Register in the pending ACK tracker so we can retry if needed
        if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(50))) {
            for (int i = 0; i < MAX_PENDING_CMDS; i++) {
                if (!pendingCmds[i].active) {
                    // Found a free slot — store the command details
                    pendingCmds[i].cmdSeq   = seq;
                    pendingCmds[i].targetId = target_id;
                    pendingCmds[i].type     = type;
                    pendingCmds[i].sentAtMs = millis();
                    pendingCmds[i].retries  = 0;
                    memcpy(pendingCmds[i].buf, cmd.buf, cmd.len);
                    pendingCmds[i].len      = cmd.len;
                    pendingCmds[i].active   = true;
                    break;
                }
            }
            xSemaphoreGive(pendingMutex);
        }
    }
}
