/*
  ┌─────────────────────────────────────────────────────────────┐
  │  BLUEPAWS V4 — HOME HUB FIRMWARE                            │
  │  Hardware: Seeed XIAO ESP32-S3 + SX1262 LoRa                │
  │                                                             │
  │  FreeRTOS Tasks:                                            │
  │    loraTask  — RX packets, validate, dispatch; TX commands  │
  │    webTask   — HTTP server, WebSocket, mDNS                 │
  │    bleTask   — BLE home beacon advertising                  │
  │    cloudTask — REST POST relay to Supabase                  │
  │  Main loop() yields to scheduler.                           │
  └─────────────────────────────────────────────────────────────┘
*/

#include <Arduino.h>
#include <RadioLib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <HTTPClient.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <bp_protocol.h>
#include <bp_config.h>
#include "hub_pins.h"

// ═══════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════

// WiFi AP settings (always active)
#define WIFI_AP_SSID     "BluePaws-Hub"
#define WIFI_AP_PASS     "bluepaws4"
#define WIFI_AP_CHANNEL  6

// WiFi STA settings (connect to home network)
// Set via web GUI or build flags
#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID    ""
#endif
#ifndef WIFI_STA_PASS
#define WIFI_STA_PASS    ""
#endif

// Cloud endpoint
#ifndef CLOUD_ENDPOINT
#define CLOUD_ENDPOINT   ""
#endif

// mDNS hostname
#define MDNS_HOSTNAME    "bluepaws"

// WebSocket port (HTTP on 80, WS on 81)
#define WS_PORT          81
#define HTTP_PORT        80

// Storage
#define LOG_FILE_PATH    "/log.csv"
#define MAX_LOG_ENTRIES  5000
#define CONFIG_FILE_PATH "/config.json"

// ═══════════════════════════════════════════════
// Task Configuration
// ═══════════════════════════════════════════════
#define STACK_LORA   4096
#define STACK_WEB    8192
#define STACK_BLE    2048
#define STACK_CLOUD  4096

#define PRIO_LORA    3
#define PRIO_WEB     2
#define PRIO_CLOUD   2
#define PRIO_BLE     1

// ═══════════════════════════════════════════════
// Globals
// ═══════════════════════════════════════════════

// LoRa radio
static SPIClass loraSPI(HSPI);
static SX1262 lora = new Module(PIN_LORA_NSS, PIN_LORA_DIO1,
                                 PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);
static volatile bool loraPacketReceived = false;
static SemaphoreHandle_t loraMutex = NULL;

// Web server
static WebServer httpServer(HTTP_PORT);

// WebSocket — lightweight manual implementation over raw TCP
// ESP32 Arduino doesn't include WebSocketsServer by default;
// we use a simple broadcast approach via the HTTP server's
// Server-Sent Events (SSE) for real-time push.
//
// SSE is simpler, requires no extra library, and works in all browsers.
// The client opens GET /events and receives a stream of updates.
static WiFiClient sseClients[4];
static uint8_t sseClientCount = 0;
static SemaphoreHandle_t sseMutex = NULL;

// Command TX queue (hub → collar)
#define CMD_QUEUE_SIZE   8
static QueueHandle_t cmdQueue = NULL;

struct cmd_entry_t {
    uint8_t  buf[BP_MAX_PACKET_SIZE];
    uint8_t  len;
};

// Cloud relay queue
#define CLOUD_QUEUE_SIZE 16
static QueueHandle_t cloudQueue = NULL;

struct cloud_entry_t {
    uint8_t  buf[BP_MAX_PACKET_SIZE];
    uint8_t  len;
    int16_t  rssi;
    float    snr;
};

// Packet stats
static uint32_t rxCount = 0;
static uint32_t crcFailCount = 0;
static uint32_t txCount = 0;
static uint32_t cmdSeqCounter = 0;

// WiFi state
static bool staConnected = false;
static String staSSID = WIFI_STA_SSID;
static String staPass = WIFI_STA_PASS;
static String cloudEndpoint = CLOUD_ENDPOINT;

// Storage
static uint32_t logEntryCount = 0;

// Task handles
static TaskHandle_t loraTaskHandle  = NULL;
static TaskHandle_t webTaskHandle   = NULL;
static TaskHandle_t bleTaskHandle   = NULL;
static TaskHandle_t cloudTaskHandle = NULL;

// Latest telemetry per device (for web GUI)
#define MAX_DEVICES 16
struct device_state_t {
    uint16_t device_id;
    uint32_t last_seq;
    uint32_t last_time;
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint16_t batt_mV;
    uint16_t acc_m;
    uint16_t fix_age_s;
    uint8_t  status;
    uint8_t  profile;
    int16_t  rssi;
    float    snr;
    uint32_t local_millis;  // millis() when received
    bool     has_gps;
};

static device_state_t devices[MAX_DEVICES];
static uint8_t deviceCount = 0;
static SemaphoreHandle_t deviceMutex = NULL;

// ═══════════════════════════════════════════════
// Forward Declarations
// ═══════════════════════════════════════════════
static void loraTask(void *param);
static void webTask(void *param);
static void bleTask(void *param);
static void cloudTask(void *param);

static void initLoRa();
static void initWiFi();
static void initBLE();
static void initStorage();
static void initWebServer();

static void handlePacket(const uint8_t *buf, uint8_t len, int16_t rssi, float snr);
static void updateDeviceState(const uint8_t *buf, int16_t rssi, float snr);
static void logToStorage(const uint8_t *buf, uint8_t len, int16_t rssi);
static void sseBroadcast(const char *event, const char *data);
static device_state_t *findDevice(uint16_t id);
static void sendCommand(uint16_t target_id, bp_pkt_type_t type, bp_profile_t mode);

// DIO1 ISR
static void IRAM_ATTR onLoRaDio1() {
    loraPacketReceived = true;
}

// ═══════════════════════════════════════════════
// Setup
// ═══════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }

    Serial.println("=================================");
    Serial.println("  Bluepaws V4 — Home Hub");
    Serial.printf("  Protocol v%d | Max %d bytes\n",
                  BP_PROTOCOL_VERSION, BP_MAX_PACKET_SIZE);
    Serial.println("=================================");

    // Create synchronization primitives
    loraMutex   = xSemaphoreCreateMutex();
    sseMutex    = xSemaphoreCreateMutex();
    deviceMutex = xSemaphoreCreateMutex();
    cmdQueue    = xQueueCreate(CMD_QUEUE_SIZE, sizeof(cmd_entry_t));
    cloudQueue  = xQueueCreate(CLOUD_QUEUE_SIZE, sizeof(cloud_entry_t));

    initStorage();
    initLoRa();
    initWiFi();
    initBLE();

    // Create FreeRTOS tasks pinned to cores
    xTaskCreatePinnedToCore(loraTask,  "lora",  STACK_LORA,  NULL, PRIO_LORA,  &loraTaskHandle,  1);
    xTaskCreatePinnedToCore(webTask,   "web",   STACK_WEB,   NULL, PRIO_WEB,   &webTaskHandle,   0);
    xTaskCreatePinnedToCore(bleTask,   "ble",   STACK_BLE,   NULL, PRIO_BLE,   &bleTaskHandle,   0);
    xTaskCreatePinnedToCore(cloudTask, "cloud", STACK_CLOUD, NULL, PRIO_CLOUD, &cloudTaskHandle, 0);

    Serial.println("[INIT] All tasks started");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// ═══════════════════════════════════════════════
// Initialisation
// ═══════════════════════════════════════════════

static void initLoRa() {
    loraSPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

    Serial.print("[LORA] Initialising SX1262... ");
    int state = lora.begin(
        LORA_FREQUENCY,
        LORA_BANDWIDTH,
        LORA_SPREADING,
        LORA_CODING_RATE,
        LORA_SYNC_WORD,
        10,  // default TX power for hub (lower than collar)
        LORA_PREAMBLE_LEN
    );

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("FAILED (err %d)\n", state);
        return;
    }

    lora.setCRC(LORA_CRC_ENABLED);
    lora.setDio1Action(onLoRaDio1);
    lora.startReceive();

    Serial.println("OK");
}

static void initWiFi() {
    // AP+STA hybrid mode
    WiFi.mode(WIFI_AP_STA);

    // Start AP (always on)
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHANNEL);
    Serial.printf("[WIFI] AP started: %s @ %s\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());

    // Attempt STA connection if credentials exist
    if (staSSID.length() > 0) {
        Serial.printf("[WIFI] Connecting to '%s'...\n", staSSID.c_str());
        WiFi.begin(staSSID.c_str(), staPass.c_str());
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(250);
        }
        if (WiFi.status() == WL_CONNECTED) {
            staConnected = true;
            Serial.printf("[WIFI] STA connected: %s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("[WIFI] STA connection failed — AP only");
        }
    }

    // mDNS
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        Serial.printf("[MDNS] http://%s.local\n", MDNS_HOSTNAME);
    }
}

static void initBLE() {
    BLEDevice::init(BLE_HOME_BEACON_NAME);
    BLEServer *pServer = BLEDevice::createServer();
    BLEAdvertising *pAdv = BLEDevice::getAdvertising();

    BLEAdvertisementData advData;
    advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    advData.setName(BLE_HOME_BEACON_NAME);
    pAdv->setAdvertisementData(advData);
    pAdv->start();

    Serial.printf("[BLE] Beacon advertising: %s\n", BLE_HOME_BEACON_NAME);
}

static void initStorage() {
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed!");
        return;
    }
    Serial.println("[FS] LittleFS mounted");

    // Count existing log entries
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

    // Load saved WiFi config
    if (LittleFS.exists(CONFIG_FILE_PATH)) {
        File f = LittleFS.open(CONFIG_FILE_PATH, "r");
        if (f) {
            String line;
            // Simple key=value format
            while (f.available()) {
                line = f.readStringUntil('\n');
                line.trim();
                int eq = line.indexOf('=');
                if (eq < 0) continue;
                String key = line.substring(0, eq);
                String val = line.substring(eq + 1);
                if (key == "sta_ssid")  staSSID = val;
                if (key == "sta_pass")  staPass = val;
                if (key == "cloud_url") cloudEndpoint = val;
            }
            f.close();
            Serial.println("[FS] Config loaded");
        }
    }
}

// ═══════════════════════════════════════════════
// LoRa Task — RX & TX
// ═══════════════════════════════════════════════

static void loraTask(void *param) {
    (void)param;
    uint8_t rxBuf[BP_MAX_PACKET_SIZE];
    TickType_t lastCmdTx = 0;

    for (;;) {
        // Check for received packet
        if (loraPacketReceived) {
            loraPacketReceived = false;

            if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(100))) {
                int len = lora.getPacketLength();
                if (len > 0 && len <= BP_MAX_PACKET_SIZE) {
                    int state = lora.readData(rxBuf, len);
                    int16_t rssi = lora.getRSSI();
                    float snr = lora.getSNR();

                    // Restart RX
                    lora.startReceive();
                    xSemaphoreGive(loraMutex);

                    if (state == RADIOLIB_ERR_NONE) {
                        handlePacket(rxBuf, (uint8_t)len, rssi, snr);
                    }
                } else {
                    lora.startReceive();
                    xSemaphoreGive(loraMutex);
                }
            }
        }

        // Process command TX queue (rate-limited)
        cmd_entry_t cmd;
        TickType_t now = xTaskGetTickCount();
        if ((now - lastCmdTx) >= pdMS_TO_TICKS(CMD_QUEUE_INTERVAL_MS)) {
            if (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
                if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(200))) {
                    int state = lora.transmit(cmd.buf, cmd.len);
                    lora.startReceive();  // Back to RX
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

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ═══════════════════════════════════════════════
// Packet Handler
// ═══════════════════════════════════════════════

static void handlePacket(const uint8_t *buf, uint8_t len, int16_t rssi, float snr) {
    // Validate CRC
    if (!pkt_validate_crc(buf, len)) {
        crcFailCount++;
        Serial.printf("[LORA] CRC fail #%u (len=%u)\n", crcFailCount, len);
        return;
    }

    // Check protocol version
    if (pkt_version(buf) != BP_PROTOCOL_VERSION) {
        Serial.printf("[LORA] Version mismatch: %d\n", pkt_version(buf));
        return;
    }

    rxCount++;
    uint16_t devId = pkt_device_id(buf);
    uint16_t pktType = pkt_pkt_type(buf);

    Serial.printf("[LORA] RX #%u from %s | type=0x%02X rssi=%d snr=%.1f\n",
                  rxCount, bp_device_name(devId), pktType, rssi, snr);

    // Update device state for web GUI
    updateDeviceState(buf, rssi, snr);

    // Log to storage
    logToStorage(buf, len, rssi, snr);

    // Queue for cloud relay
    cloud_entry_t ce;
    memcpy(ce.buf, buf, len);
    ce.len  = len;
    ce.rssi = rssi;
    ce.snr  = snr;
    xQueueSend(cloudQueue, &ce, 0);  // Drop if full

    // Broadcast to SSE clients
    char jsonBuf[384];
    buildDeviceJson(buf, rssi, snr, jsonBuf, sizeof(jsonBuf));
    sseBroadcast("telemetry", jsonBuf);
}

// Build JSON representation of a packet for web GUI
static void buildDeviceJson(const uint8_t *buf, int16_t rssi, float snr,
                             char *out, size_t outLen) {
    uint16_t devId    = pkt_device_id(buf);
    uint16_t flags    = pkt_flags(buf);
    bool hasGps       = (flags & FLAG_HAS_GPS) != 0;
    double lat        = hasGps ? pkt_lat_e7(buf) / 1e7 : 0.0;
    double lon        = hasGps ? pkt_lon_e7(buf) / 1e7 : 0.0;
    uint8_t profile   = 0;
    pkt_tlv_get_u8(buf, TLV_PROFILE, &profile);

    snprintf(out, outLen,
        "{\"id\":%u,\"name\":\"%s\",\"seq\":%u,\"time\":%u,"
        "\"status\":\"%s\",\"profile\":\"%s\","
        "\"lat\":%.7f,\"lon\":%.7f,\"hasGps\":%s,"
        "\"batt\":%u,\"acc\":%u,\"fixAge\":%u,"
        "\"rssi\":%d,\"snr\":%.1f,"
        "\"bleHome\":%s,\"cellular\":%s}",
        devId, bp_device_name(devId), pkt_msg_seq(buf), pkt_time_unix(buf),
        bp_status_display((bp_status_t)pkt_status(buf)),
        bp_profile_name((bp_profile_t)profile),
        lat, lon, hasGps ? "true" : "false",
        pkt_batt_mV(buf), pkt_acc_m(buf), pkt_fix_age_s(buf),
        rssi, snr,
        (flags & FLAG_BLE_HOME) ? "true" : "false",
        (flags & FLAG_CELLULAR) ? "true" : "false"
    );
}

static void updateDeviceState(const uint8_t *buf, int16_t rssi, float snr) {
    uint16_t devId = pkt_device_id(buf);
    uint16_t flags = pkt_flags(buf);

    if (xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(50))) {
        device_state_t *dev = findDevice(devId);
        if (!dev && deviceCount < MAX_DEVICES) {
            dev = &devices[deviceCount++];
            dev->device_id = devId;
        }
        if (dev) {
            dev->last_seq    = pkt_msg_seq(buf);
            dev->last_time   = pkt_time_unix(buf);
            dev->status      = pkt_status(buf);
            dev->batt_mV     = pkt_batt_mV(buf);
            dev->acc_m       = pkt_acc_m(buf);
            dev->fix_age_s   = pkt_fix_age_s(buf);
            dev->rssi        = rssi;
            dev->snr         = snr;
            dev->local_millis = millis();
            dev->has_gps     = (flags & FLAG_HAS_GPS) != 0;
            if (dev->has_gps) {
                dev->lat_e7 = pkt_lat_e7(buf);
                dev->lon_e7 = pkt_lon_e7(buf);
            }
            pkt_tlv_get_u8(buf, TLV_PROFILE, &dev->profile);
        }
        xSemaphoreGive(deviceMutex);
    }
}

static device_state_t *findDevice(uint16_t id) {
    for (uint8_t i = 0; i < deviceCount; i++) {
        if (devices[i].device_id == id) return &devices[i];
    }
    return NULL;
}

// ═══════════════════════════════════════════════
// Storage — CSV Log
// ═══════════════════════════════════════════════

static void logToStorage(const uint8_t *buf, uint8_t len, int16_t rssi, float snr) {
    File f = LittleFS.open(LOG_FILE_PATH, "a");
    if (!f) return;

    uint16_t devId = pkt_device_id(buf);
    uint16_t flags = pkt_flags(buf);
    bool hasGps    = (flags & FLAG_HAS_GPS) != 0;

    // Extract profile from TLV (0 if not present)
    uint8_t profile = 0;
    pkt_tlv_get_u8(buf, TLV_PROFILE, &profile);

    // CSV: timestamp,device_id,msg_seq,status,lat,lon,batt,rssi,snr,profile
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

    // Rotate if too many entries
    if (logEntryCount > MAX_LOG_ENTRIES) {
        LittleFS.remove(LOG_FILE_PATH);
        logEntryCount = 0;
        Serial.println("[FS] Log rotated");
    }
}

// ═══════════════════════════════════════════════
// SSE (Server-Sent Events) Push
// ═══════════════════════════════════════════════

static void sseBroadcast(const char *event, const char *data) {
    if (xSemaphoreTake(sseMutex, pdMS_TO_TICKS(50))) {
        for (int i = sseClientCount - 1; i >= 0; i--) {
            if (sseClients[i].connected()) {
                sseClients[i].printf("event: %s\ndata: %s\n\n", event, data);
            } else {
                // Remove disconnected client
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
// ═══════════════════════════════════════════════

static void handleRoot() {
    File f = LittleFS.open("/index.html", "r");
    if (f) {
        httpServer.streamFile(f, "text/html");
        f.close();
    } else {
        httpServer.send(200, "text/html",
            "<h1>Bluepaws V4 Hub</h1><p>Upload web files to LittleFS.</p>");
    }
}

static void handleCSS() {
    File f = LittleFS.open("/style.css", "r");
    if (f) {
        httpServer.streamFile(f, "text/css");
        f.close();
    } else {
        httpServer.send(404, "text/plain", "Not found");
    }
}

static void handleJS() {
    File f = LittleFS.open("/app.js", "r");
    if (f) {
        httpServer.streamFile(f, "application/javascript");
        f.close();
    } else {
        httpServer.send(404, "text/plain", "Not found");
    }
}

// SSE endpoint — client connects and receives real-time events
static void handleEvents() {
    WiFiClient client = httpServer.client();
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

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/event-stream");
    client.println("Cache-Control: no-cache");
    client.println("Connection: keep-alive");
    client.println("Access-Control-Allow-Origin: *");
    client.println();
    client.flush();

    // Send current state as initial snapshot
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

// API: get all device states
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

// API: hub status
static void handleApiStatus() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"uptime\":%u,\"rxCount\":%u,\"txCount\":%u,"
        "\"crcFails\":%u,\"devices\":%u,\"logEntries\":%u,"
        "\"staConnected\":%s,\"staIP\":\"%s\",\"apIP\":\"%s\","
        "\"freeHeap\":%u}",
        millis() / 1000, rxCount, txCount,
        crcFailCount, deviceCount, logEntryCount,
        staConnected ? "true" : "false",
        staConnected ? WiFi.localIP().toString().c_str() : "",
        WiFi.softAPIP().toString().c_str(),
        ESP.getFreeHeap()
    );
    httpServer.send(200, "application/json", buf);
}

// API: send command to collar
static void handleApiCommand() {
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "text/plain", "POST only");
        return;
    }

    String body = httpServer.arg("plain");
    // Simple parsing: device=XXXX&mode=normal|active|lost|powersave
    uint16_t targetId = 0;
    bp_profile_t mode = PROFILE_UNKNOWN;

    // Parse device parameter
    int dIdx = body.indexOf("device=");
    if (dIdx >= 0) {
        targetId = (uint16_t)strtoul(body.c_str() + dIdx + 7, NULL, 16);
    }

    // Parse mode parameter
    int mIdx = body.indexOf("mode=");
    if (mIdx >= 0) {
        String modeStr = body.substring(mIdx + 5);
        int ampIdx = modeStr.indexOf('&');
        if (ampIdx >= 0) modeStr = modeStr.substring(0, ampIdx);
        mode = bp_profile_from_name(modeStr.c_str());
    }

    if (targetId == 0 || mode == PROFILE_UNKNOWN) {
        httpServer.send(400, "text/plain", "Bad request: device=XXXX&mode=name");
        return;
    }

    sendCommand(targetId, PKT_CMD_MODE, mode);
    httpServer.send(200, "application/json", "{\"ok\":true}");
}

// API: save WiFi/cloud config
static void handleApiConfig() {
    if (httpServer.method() != HTTP_POST) {
        httpServer.send(405, "text/plain", "POST only");
        return;
    }

    String body = httpServer.arg("plain");

    // Parse ssid, pass, cloud_url
    auto parseParam = [&](const char *key) -> String {
        String k = String(key) + "=";
        int idx = body.indexOf(k);
        if (idx < 0) return "";
        String val = body.substring(idx + k.length());
        int amp = val.indexOf('&');
        if (amp >= 0) val = val.substring(0, amp);
        return val;
    };

    String newSSID = parseParam("ssid");
    String newPass = parseParam("pass");
    String newCloud = parseParam("cloud_url");

    // Save to file
    File f = LittleFS.open(CONFIG_FILE_PATH, "w");
    if (f) {
        if (newSSID.length() > 0)  f.printf("sta_ssid=%s\n", newSSID.c_str());
        if (newPass.length() > 0)  f.printf("sta_pass=%s\n", newPass.c_str());
        if (newCloud.length() > 0) f.printf("cloud_url=%s\n", newCloud.c_str());
        f.close();
    }

    staSSID = newSSID;
    staPass = newPass;
    if (newCloud.length() > 0) cloudEndpoint = newCloud;

    httpServer.send(200, "application/json", "{\"ok\":true,\"restart\":true}");
    // Restart to apply WiFi changes
    delay(500);
    ESP.restart();
}

static void handleNotFound() {
    // Try to serve from LittleFS
    String path = httpServer.uri();
    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
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

static void initWebServer() {
    httpServer.on("/",             HTTP_GET,  handleRoot);
    httpServer.on("/style.css",    HTTP_GET,  handleCSS);
    httpServer.on("/app.js",       HTTP_GET,  handleJS);
    httpServer.on("/events",       HTTP_GET,  handleEvents);
    httpServer.on("/api/devices",  HTTP_GET,  handleApiDevices);
    httpServer.on("/api/status",   HTTP_GET,  handleApiStatus);
    httpServer.on("/api/command",  HTTP_POST, handleApiCommand);
    httpServer.on("/api/config",   HTTP_POST, handleApiConfig);
    httpServer.onNotFound(handleNotFound);
    httpServer.begin();
    Serial.printf("[WEB] HTTP server on port %d\n", HTTP_PORT);
}

static void webTask(void *param) {
    (void)param;
    initWebServer();

    TickType_t lastHeartbeat = 0;

    for (;;) {
        httpServer.handleClient();

        // Send SSE heartbeat every 5 seconds so the browser knows we're alive
        TickType_t now = xTaskGetTickCount();
        if (now - lastHeartbeat >= pdMS_TO_TICKS(5000)) {
            lastHeartbeat = now;
            sseBroadcast("heartbeat", "{}");
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ═══════════════════════════════════════════════
// BLE Task — Home Beacon
// ═══════════════════════════════════════════════

static void bleTask(void *param) {
    (void)param;
    // BLE advertising was started in initBLE().
    // This task just keeps it alive and can restart if needed.
    for (;;) {
        // BLE advertising runs autonomously on ESP32.
        // Periodically verify it's still active.
        BLEAdvertising *pAdv = BLEDevice::getAdvertising();
        if (pAdv) {
            pAdv->start();  // No-op if already running
        }
        vTaskDelay(pdMS_TO_TICKS(30000));  // Check every 30s
    }
}

// ═══════════════════════════════════════════════
// Cloud Task — REST POST Relay
// ═══════════════════════════════════════════════

static void cloudTask(void *param) {
    (void)param;
    cloud_entry_t entry;

    for (;;) {
        // Block until a packet is queued
        if (xQueueReceive(cloudQueue, &entry, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (!staConnected || cloudEndpoint.length() == 0) {
                continue;  // No WiFi or no endpoint configured
            }

            HTTPClient http;
            http.begin(cloudEndpoint);
            http.addHeader("Content-Type", "application/octet-stream");
            http.addHeader("X-BP-Version", String(BP_PROTOCOL_VERSION));
            http.addHeader("X-BP-Device", String(pkt_device_id(entry.buf)));
            http.addHeader("X-BP-RSSI", String(entry.rssi));

            int code = http.POST(entry.buf, entry.len);
            if (code > 0) {
                Serial.printf("[CLOUD] POST %d → %d\n", entry.len, code);
            } else {
                Serial.printf("[CLOUD] POST failed: %s\n", http.errorToString(code).c_str());
            }
            http.end();
        }
    }
}

// ═══════════════════════════════════════════════
// Command Builder — Hub → Collar
// ═══════════════════════════════════════════════

static void sendCommand(uint16_t target_id, bp_pkt_type_t type, bp_profile_t mode) {
    cmd_entry_t cmd;
    uint16_t flags = (uint16_t)type;

    pkt_init(cmd.buf, DEVICE_ID_HUB, ++cmdSeqCounter, 0, STATUS_OK, flags);

    if (type == PKT_CMD_MODE && mode != PROFILE_UNKNOWN) {
        pkt_add_tlv_u8(cmd.buf, TLV_PROFILE, (uint8_t)mode);
    }

    // Set target device ID in the packet (overwrite device_id field)
    // Convention: for commands, device_id = target collar
    memcpy(&cmd.buf[1], &target_id, 2);

    cmd.len = pkt_finalize(cmd.buf);

    if (xQueueSend(cmdQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.printf("[CMD] Queued mode=%s for %s\n",
                      bp_profile_name(mode), bp_device_name(target_id));
    }
}
