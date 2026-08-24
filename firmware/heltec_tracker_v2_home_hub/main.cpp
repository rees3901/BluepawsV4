/*
  Bluepaws V4 — Heltec Wireless Tracker V2 Home Hub substitute

  Receives raw Bluepaws TLV v1.1 packets from the RAK4631 collar testbed,
  validates/prints them locally, base64-encodes the unchanged TLV payload, and
  relays it to the Supabase ingest-position Edge Function as HTTPS JSON.

  Secrets are configured at runtime over the USB serial monitor and stored in
  ESP32 NVS. Reesnet Guest is used as the current open test-network default;
  do not commit production Wi-Fi credentials or gateway bearer tokens.

  The Home Hub is an always-on FreeRTOS application: LoRa receive, cloud relay,
  local web UI, Wi-Fi management, and serial/profile control run as separate
  tasks. There is intentionally no sleep path on the hub.
*/

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <bp_config.h>
#include <bp_protocol.h>

// Heltec Wireless Tracker V2 / HTIT-Tracker V2.3 pinout from the legacy
// BluePawzReceiver project. V3 protocol/radio settings remain read-only
// reference; this firmware uses the V4 locked LoRa profile from bp_config.h.
static constexpr uint8_t PIN_LORA_NSS = 8;
static constexpr uint8_t PIN_LORA_SCK = 9;
static constexpr uint8_t PIN_LORA_MOSI = 10;
static constexpr uint8_t PIN_LORA_MISO = 11;
static constexpr uint8_t PIN_LORA_RST = 12;
static constexpr uint8_t PIN_LORA_BUSY = 13;
static constexpr uint8_t PIN_LORA_DIO1 = 14;
static constexpr uint8_t PIN_VEXT = 3;       // active LOW, powers TFT/GNSS rails on legacy board
static constexpr uint8_t PIN_LED = 18;

static constexpr char AP_SSID[] = "BluePaws-Hub-V4";
static constexpr char AP_PASS[] = "bluepaws4";
static constexpr char DEFAULT_STA_SSID[] = "Reesnet Guest";
static constexpr char DEFAULT_STA_PASS[] = "";
static constexpr char NTP_PRIMARY[] = "pool.ntp.org";
static constexpr char NTP_SECONDARY[] = "time.google.com";
static constexpr char NTP_TERTIARY[] = "time.cloudflare.com";
static constexpr uint32_t MIN_VALID_UNIX = 1700000000UL; // 2023-11-14, filters unset clocks
static constexpr uint32_t NTP_RETRY_INTERVAL_MS = 60000UL;
static constexpr uint32_t NTP_REFRESH_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
static constexpr char MDNS_NAME[] = "bluepaws-hub";
static constexpr uint16_t GATEWAY_GUID16 = 0x0016;
static constexpr uint8_t LORA_RX_BUFFER_LEN = BP_MAX_PACKET_SIZE + 16;

enum HubCommProfile : uint8_t {
  HUB_PROFILE_HOME = 0,
  HUB_PROFILE_PORTABLE = 1,
  HUB_PROFILE_OFF_GRID = 2,
};

struct HubConfig {
  String wifiSsid;
  String wifiPass;
  String cloudUrl;
  String gatewayToken;
};

struct CloudEntry {
  uint8_t bytes[BP_MAX_PACKET_SIZE];
  uint8_t len = 0;
  int16_t rssi = 0;
  float snr = 0.0f;
  uint32_t rxMillis = 0;
};

struct HubStats {
  uint32_t rxValid = 0;
  uint32_t rxInvalid = 0;
  uint32_t cloudOk = 0;
  uint32_t cloudFail = 0;
  uint16_t lastDevice = 0;
  uint16_t lastSeq = 0;
  int16_t lastRssi = 0;
  float lastSnr = 0.0f;
  int lastHttpCode = 0;
};

static SPIClass loraSPI(HSPI);
static SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);
static WebServer server(80);
static Preferences prefs;
static HubConfig config;
static HubStats stats;
static QueueHandle_t cloudQueue = nullptr;
static SemaphoreHandle_t statsMutex = nullptr;
static volatile bool radioIrq = false;
static String serialLine;
static HubCommProfile hubProfile = HUB_PROFILE_HOME;
static bool provisioningMode = false;
static bool apRunning = false;
static bool ntpConfigured = false;
static uint32_t lastNtpAttemptMs = 0;
static uint32_t lastNtpSyncMs = 0;
static BLEAdvertising *bleAdvertising = nullptr;
static bool bleHomeBeaconAdvertising = false;

static void onRadioIrq() {
  radioIrq = true;
}

static String htmlEscape(const String &value) {
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      default: out += c; break;
    }
  }
  return out;
}

static String base64Encode(const uint8_t *data, uint8_t len) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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

static String gatewayHex() {
  char out[5];
  snprintf(out, sizeof(out), "%04X", GATEWAY_GUID16);
  return String(out);
}

static const char *hubProfileSlug(HubCommProfile profile) {
  switch (profile) {
    case HUB_PROFILE_HOME: return "home";
    case HUB_PROFILE_PORTABLE: return "portable";
    case HUB_PROFILE_OFF_GRID: return "off_grid";
    default: return "home";
  }
}

static const char *hubProfileDisplay(HubCommProfile profile) {
  switch (profile) {
    case HUB_PROFILE_HOME: return "Home";
    case HUB_PROFILE_PORTABLE: return "Portable";
    case HUB_PROFILE_OFF_GRID: return "Off-grid";
    default: return "Home";
  }
}

static bool hubProfileAllowsCloud() {
  return hubProfile == HUB_PROFILE_HOME || hubProfile == HUB_PROFILE_PORTABLE;
}

static bool hubNeedsProvisioning() {
  return config.wifiSsid.length() == 0 || config.cloudUrl.length() == 0 || config.gatewayToken.length() == 0;
}

static bool hubShouldRunAp() {
  return hubProfile == HUB_PROFILE_OFF_GRID || provisioningMode || hubNeedsProvisioning();
}

static bool hubProfileAdvertisesHomeBeacon() {
  return hubProfile == HUB_PROFILE_HOME;
}

static bool parseHubProfile(const String &value, HubCommProfile &out) {
  String key = value;
  key.trim();
  key.toLowerCase();
  key.replace("-", "_");

  if (key == "home" || key == "0") {
    out = HUB_PROFILE_HOME;
    return true;
  }
  if (key == "portable" || key == "1") {
    out = HUB_PROFILE_PORTABLE;
    return true;
  }
  if (key == "offgrid" || key == "off_grid" || key == "off" || key == "2") {
    out = HUB_PROFILE_OFF_GRID;
    return true;
  }
  return false;
}

static bool hubTimeSynced() {
  time_t now = time(nullptr);
  return now >= (time_t)MIN_VALID_UNIX && now <= (time_t)0xFFFFFFFFUL;
}

static uint32_t currentHubUnixOrFallback(uint32_t fallbackUnix) {
  time_t now = time(nullptr);
  if (now >= (time_t)MIN_VALID_UNIX && now <= (time_t)0xFFFFFFFFUL) {
    return (uint32_t)now;
  }
  return fallbackUnix;
}

static bool syncHubClock(bool force) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  const uint32_t nowMs = millis();
  const bool synced = hubTimeSynced();
  const uint32_t interval = synced ? NTP_REFRESH_INTERVAL_MS : NTP_RETRY_INTERVAL_MS;
  if (!force && lastNtpAttemptMs != 0 && (nowMs - lastNtpAttemptMs) < interval) {
    return synced;
  }

  lastNtpAttemptMs = nowMs;
  if (!ntpConfigured || force) {
    configTime(0, 0, NTP_PRIMARY, NTP_SECONDARY, NTP_TERTIARY);
    ntpConfigured = true;
  }

  for (uint8_t i = 0; i < 20; i++) {
    if (hubTimeSynced()) {
      lastNtpSyncMs = millis();
      Serial.printf("[TIME] NTP synced: %lu\n", (unsigned long)time(nullptr));
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  Serial.println("[TIME] NTP sync not ready yet; gateway wrapper will temporarily fall back to collar TLV time.");
  return false;
}

static void printHex(const uint8_t *bytes, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    if (bytes[i] < 0x10) Serial.print('0');
    Serial.print(bytes[i], HEX);
    if (i + 1 < len) Serial.print(' ');
  }
  Serial.println();
}

static void loadConfig() {
  prefs.begin("bluepaws", false);
  config.wifiSsid = prefs.getString("ssid", DEFAULT_STA_SSID);
  config.wifiPass = prefs.getString("pass", DEFAULT_STA_PASS);
  config.cloudUrl = prefs.getString("url", "");
  config.gatewayToken = prefs.getString("token", "");
  uint8_t storedProfile = prefs.getUChar("profile", HUB_PROFILE_HOME);
  if (storedProfile > HUB_PROFILE_OFF_GRID) storedProfile = HUB_PROFILE_HOME;
  hubProfile = (HubCommProfile)storedProfile;
  provisioningMode = prefs.getBool("provision", false);
}

static void saveConfig() {
  prefs.putString("ssid", config.wifiSsid);
  prefs.putString("pass", config.wifiPass);
  prefs.putString("url", config.cloudUrl);
  prefs.putString("token", config.gatewayToken);
}

static void printConfig() {
  Serial.println();
  Serial.println("[CFG] Current hub relay configuration");
  Serial.printf("      Profile   : %s\n", hubProfileDisplay(hubProfile));
  Serial.printf("      Wi-Fi SSID: %s\n", config.wifiSsid.length() ? config.wifiSsid.c_str() : "<not set>");
  Serial.printf("      Wi-Fi pass: %s\n", config.wifiPass.length() ? "<stored>" : "<not set>");
  Serial.printf("      Cloud URL : %s\n", config.cloudUrl.length() ? config.cloudUrl.c_str() : "<not set>");
  Serial.printf("      Token     : %s\n", config.gatewayToken.length() ? "<stored>" : "<not set>");
  Serial.printf("      Gateway   : %s\n", gatewayHex().c_str());
  Serial.printf("      Cloud     : %s\n", hubProfileAllowsCloud() ? "allowed by profile" : "disabled by off-grid profile");
  Serial.printf("      BLE Home  : %s\n", hubProfileAdvertisesHomeBeacon() ? "advertising" : "disabled for roaming/off-grid");
  Serial.printf("      Provision : %s\n", provisioningMode ? "on" : "off");
  Serial.printf("      Local AP  : %s\n", hubShouldRunAp() ? "enabled" : "disabled unless provisioning/off-grid");
  Serial.printf("      Time/NTP  : %s", hubTimeSynced() ? "synced" : "not synced");
  if (hubTimeSynced()) {
    Serial.printf(" (%lu)", (unsigned long)time(nullptr));
  }
  Serial.println();
}

static void printHelp() {
  Serial.println();
  Serial.println("[CMD] Heltec V4 Home Hub serial commands:");
  Serial.println("      ssid <your-wifi-name>");
  Serial.println("      pass <your-wifi-password>");
  Serial.println("      url https://<project-ref>.supabase.co/functions/v1/ingest-position");
  Serial.println("      token <gateway-bearer-token>");
  Serial.println("      profile home|portable|offgrid");
  Serial.println("      home");
  Serial.println("      portable");
  Serial.println("      offgrid");
  Serial.println("      provision on|off");
  Serial.println("      time");
  Serial.println("      connect");
  Serial.println("      show");
  Serial.println("      clear");
  Serial.println("      help");
}

static void connectWifi() {
  if (!hubProfileAllowsCloud()) {
    Serial.printf("[WIFI] STA disabled in %s profile; AP/local status page remains available.\n",
                  hubProfileDisplay(hubProfile));
    return;
  }
  if (config.wifiSsid.length() == 0) {
    Serial.println("[WIFI] STA SSID not set; AP-only mode remains available.");
    return;
  }
  Serial.printf("[WIFI] Connecting STA to %s...\n", config.wifiSsid.c_str());
  if (config.wifiPass.length() == 0) {
    WiFi.begin(config.wifiSsid.c_str());
  } else {
    WiFi.begin(config.wifiSsid.c_str(), config.wifiPass.c_str());
  }
}

static void syncWifiMode() {
  const bool apShouldRun = hubShouldRunAp();
  const bool staShouldRun = hubProfileAllowsCloud();

  if (staShouldRun && apShouldRun) {
    WiFi.mode(WIFI_AP_STA);
  } else if (staShouldRun) {
    WiFi.mode(WIFI_STA);
  } else {
    WiFi.mode(WIFI_AP);
  }

  if (apShouldRun && !apRunning) {
    WiFi.softAP(AP_SSID, AP_PASS, 6);
    apRunning = true;
    Serial.printf("[WIFI] AP started: %s | %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  } else if (!apShouldRun && apRunning) {
    WiFi.softAPdisconnect(true);
    apRunning = false;
    Serial.println("[WIFI] AP stopped; Home/Portable profile is no longer in provisioning/off-grid mode.");
  }

  if (!staShouldRun && WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(false, false);
  }
}

static void startWifi() {
  syncWifiMode();
  connectWifi();
}

static void initBleBeacon() {
  BLEDevice::init(BLE_HOME_BEACON_NAME);
  BLEServer *bleServer = BLEDevice::createServer();
  (void)bleServer;
  bleAdvertising = BLEDevice::getAdvertising();
  bleAdvertising->setScanResponse(true);
  bleAdvertising->setMinPreferred(0x06);
  bleAdvertising->setMinPreferred(0x12);
}

static void setBleHomeBeaconEnabled(bool enabled) {
  if (bleAdvertising == nullptr) return;

  if (enabled && !bleHomeBeaconAdvertising) {
    BLEDevice::startAdvertising();
    bleHomeBeaconAdvertising = true;
    Serial.printf("[BLE] Advertising Home beacon name '%s'\n", BLE_HOME_BEACON_NAME);
  } else if (!enabled && bleHomeBeaconAdvertising) {
    bleAdvertising->stop();
    bleHomeBeaconAdvertising = false;
    Serial.println("[BLE] Home beacon advertising disabled for this hub profile.");
  }
}

static void applyHubProfile() {
  setBleHomeBeaconEnabled(hubProfileAdvertisesHomeBeacon());
  syncWifiMode();

  if (!hubProfileAllowsCloud()) {
    Serial.println("[PROFILE] Off-grid mode: LoRa/AP/web stay active, cloud relay is disabled.");
    return;
  }

  if (WiFi.status() != WL_CONNECTED && config.wifiSsid.length() > 0) {
    connectWifi();
  }
}

static void setHubProfile(HubCommProfile profile) {
  hubProfile = profile;
  prefs.putUChar("profile", (uint8_t)hubProfile);
  Serial.printf("[PROFILE] Hub profile set to %s\n", hubProfileDisplay(hubProfile));
  applyHubProfile();
}

static void configureRadioOrHalt() {
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  loraSPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);

  Serial.println("[LORA] Initialising Heltec Tracker V2 SX1262 with V4 locked profile...");
  int state = radio.begin(LORA_FREQUENCY);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] FATAL init failed: %d\n", state);
    while (true) {
      digitalWrite(PIN_LED, !digitalRead(PIN_LED));
      delay(100);
    }
  }

  radio.setSpreadingFactor(LORA_SPREADING);
  radio.setBandwidth(LORA_BANDWIDTH);
  radio.setCodingRate(LORA_CODING_RATE);
  radio.setPreambleLength(LORA_PREAMBLE_LEN);
  radio.setSyncWord(LORA_SYNC_WORD);
  radio.setCRC(LORA_CRC_ENABLED);
  radio.setDio1Action(onRadioIrq);

  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] FATAL startReceive failed: %d\n", state);
    while (true) {
      digitalWrite(PIN_LED, !digitalRead(PIN_LED));
      delay(250);
    }
  }

  Serial.printf("[LORA] RX ready %.1f MHz | SF%d | BW %.0f kHz | CR 4/%d | preamble %d | sync 0x%02X | CRC %s\n",
                LORA_FREQUENCY,
                LORA_SPREADING,
                LORA_BANDWIDTH,
                LORA_CODING_RATE,
                LORA_PREAMBLE_LEN,
                LORA_SYNC_WORD,
                LORA_CRC_ENABLED ? "on" : "off");
}

static String buildCloudJson(const CloudEntry &entry) {
  String body;
  body.reserve(320);
  body += F("{\"format\":\"tlv\",\"payload_b64\":\"");
  body += base64Encode(entry.bytes, entry.len);
  body += F("\",\"ingest_path\":\"lora_gateway\",\"gateway_guid16\":\"");
  body += gatewayHex();
  body += F("\",\"gateway_rx_time_unix\":");
  body += String(currentHubUnixOrFallback(pkt_time_unix(entry.bytes)));
  body += F(",\"link_type\":\"lora\",\"link_rssi_dbm\":");
  body += String(entry.rssi);
  body += F(",\"link_snr_db\":");
  body += String(entry.snr, 1);
  body += "}";
  return body;
}

static void updateStatsFromPacket(const uint8_t *buf, uint8_t len, int16_t rssi, float snr, bool valid) {
  if (xSemaphoreTake(statsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (valid) {
      stats.rxValid++;
      stats.lastDevice = pkt_device_id(buf);
      stats.lastSeq = pkt_msg_seq(buf);
      stats.lastRssi = rssi;
      stats.lastSnr = snr;
    } else {
      stats.rxInvalid++;
    }
    xSemaphoreGive(statsMutex);
  }
}

static void loraTask(void *param) {
  (void)param;
  configureRadioOrHalt();

  for (;;) {
    if (!radioIrq) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    radioIrq = false;

    uint8_t rx[LORA_RX_BUFFER_LEN] = {0};
    size_t len = 0;
    int state = radio.readData(rx, LORA_RX_BUFFER_LEN);
    int16_t rssi = (int16_t)radio.getRSSI();
    float snr = radio.getSNR();

    if (state == RADIOLIB_ERR_NONE) {
      len = radio.getPacketLength();
      if (len > BP_MAX_PACKET_SIZE) len = BP_MAX_PACKET_SIZE;

      const bool valid = pkt_validate_structure(rx, (uint8_t)len);
      updateStatsFromPacket(rx, (uint8_t)len, rssi, snr, valid);

      if (valid) {
        Serial.printf("[LORA] RX valid %uB device=%u seq=%u status=%s profile=%s reason=%s RSSI=%d SNR=%.1f\n",
                      (unsigned)len,
                      pkt_device_id(rx),
                      pkt_msg_seq(rx),
                      bp_status_display((bp_status_t)pkt_status(rx)),
                      bp_profile_name((bp_profile_t)pkt_power_profile(rx)),
                      bp_tx_reason_display(pkt_tx_reason(rx)),
                      rssi,
                      snr);
        Serial.print("[LORA] TLV hex: ");
        printHex(rx, (uint8_t)len);

        CloudEntry entry;
        memcpy(entry.bytes, rx, len);
        entry.len = (uint8_t)len;
        entry.rssi = rssi;
        entry.snr = snr;
        entry.rxMillis = millis();
        if (xQueueSend(cloudQueue, &entry, 0) != pdTRUE) {
          Serial.println("[CLOUD] Queue full; dropping relay entry");
        }
      } else {
        Serial.printf("[LORA] RX invalid structure len=%u RSSI=%d SNR=%.1f\n", (unsigned)len, rssi, snr);
      }
    } else {
      Serial.printf("[LORA] readData failed: %d\n", state);
    }

    radio.startReceive();
  }
}

static void cloudTask(void *param) {
  (void)param;
  CloudEntry entry;
  for (;;) {
    if (xQueueReceive(cloudQueue, &entry, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (!hubProfileAllowsCloud()) {
      Serial.printf("[CLOUD] %s profile is local-only; skipping relay for device=%u seq=%u\n",
                    hubProfileDisplay(hubProfile),
                    pkt_device_id(entry.bytes),
                    pkt_msg_seq(entry.bytes));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[CLOUD] STA Wi-Fi not connected; cannot relay yet");
      continue;
    }
    if (config.cloudUrl.length() == 0 || config.gatewayToken.length() == 0) {
      Serial.println("[CLOUD] URL/token not configured; cannot relay yet");
      continue;
    }

    String body = buildCloudJson(entry);
    HTTPClient http;
    http.begin(config.cloudUrl);
    http.addHeader("Content-Type", "application/json");
    String auth = "Bearer ";
    auth += config.gatewayToken;
    http.addHeader("Authorization", auth);

    const int code = http.POST(body);
    String response = http.getString();
    if (xSemaphoreTake(statsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      stats.lastHttpCode = code;
      if (code >= 200 && code < 300) stats.cloudOk++;
      else stats.cloudFail++;
      xSemaphoreGive(statsMutex);
    }
    Serial.printf("[CLOUD] POST %uB TLV wrapper -> HTTP %d\n", entry.len, code);
    if (response.length() > 0) {
      Serial.printf("[CLOUD] Response: %s\n", response.c_str());
    }
    http.end();
  }
}

static String statusJson() {
  HubStats snapshot;
  if (xSemaphoreTake(statsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snapshot = stats;
    xSemaphoreGive(statsMutex);
  }

  String out;
  out.reserve(420);
  out += "{";
  out += "\"profile\":\"" + String(hubProfileSlug(hubProfile)) + "\",";
  out += "\"profile_display\":\"" + String(hubProfileDisplay(hubProfile)) + "\",";
  out += "\"always_on\":true,";
  out += "\"cloud_allowed\":" + String(hubProfileAllowsCloud() ? "true" : "false") + ",";
  out += "\"provisioning_mode\":" + String(provisioningMode ? "true" : "false") + ",";
  out += "\"ap_enabled\":" + String(apRunning ? "true" : "false") + ",";
  out += "\"ble_home_advertising\":" + String(bleHomeBeaconAdvertising ? "true" : "false") + ",";
  out += "\"time_synced\":" + String(hubTimeSynced() ? "true" : "false") + ",";
  out += "\"hub_time_unix\":" + String(hubTimeSynced() ? (uint32_t)time(nullptr) : 0) + ",";
  out += "\"last_ntp_sync_ms\":" + String(lastNtpSyncMs) + ",";
  out += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  out += "\"sta_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  out += "\"sta_ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")) + "\",";
  out += "\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  out += "\"cloud_configured\":" + String((config.cloudUrl.length() && config.gatewayToken.length()) ? "true" : "false") + ",";
  out += "\"rx_valid\":" + String(snapshot.rxValid) + ",";
  out += "\"rx_invalid\":" + String(snapshot.rxInvalid) + ",";
  out += "\"cloud_ok\":" + String(snapshot.cloudOk) + ",";
  out += "\"cloud_fail\":" + String(snapshot.cloudFail) + ",";
  out += "\"last_device\":" + String(snapshot.lastDevice) + ",";
  out += "\"last_seq\":" + String(snapshot.lastSeq) + ",";
  out += "\"last_rssi\":" + String(snapshot.lastRssi) + ",";
  out += "\"last_snr\":" + String(snapshot.lastSnr, 1) + ",";
  out += "\"last_http_code\":" + String(snapshot.lastHttpCode);
  out += "}";
  return out;
}

static void handleRoot() {
  String page = F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>Bluepaws V4 Home Hub</title>"
                  "<style>body{font-family:system-ui;background:#071826;color:#eaf6ff;margin:24px}"
                  ".card{border:1px solid #1e5a86;border-radius:14px;background:#0d2940;padding:18px;max-width:780px}"
                  "code{color:#7bd0ff}button{padding:10px 14px;border-radius:8px}</style></head><body>"
                  "<div class='card'><p><code>BLUEPAWS V4</code></p><h1>Home Hub Relay</h1>"
                  "<p>This always-on Heltec Tracker V2 is receiving raw TLV over LoRa, serving this local status page, and relaying base64-wrapped HTTPS JSON when its profile permits cloud access.</p>"
                  "<pre id='status'>Loading...</pre></div>"
                  "<script>async function tick(){document.getElementById('status').textContent=JSON.stringify(await (await fetch('/api/status')).json(),null,2)} tick(); setInterval(tick,2000)</script>"
                  "</body></html>");
  server.send(200, "text/html", page);
}

static void webTask(void *param) {
  (void)param;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, []() {
    server.send(200, "application/json", statusJson());
  });
  server.begin();
  Serial.println("[WEB] Local status page ready on AP IP and STA IP.");

  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void wifiTask(void *param) {
  (void)param;
  for (;;) {
    if (hubProfileAllowsCloud() && config.wifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] STA disconnected; retrying according to active hub profile.");
      connectWifi();
    } else if (WiFi.status() == WL_CONNECTED) {
      syncHubClock(false);
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

static void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  int space = line.indexOf(' ');
  String cmd = (space >= 0) ? line.substring(0, space) : line;
  String value = (space >= 0) ? line.substring(space + 1) : "";
  cmd.toLowerCase();
  value.trim();

  if (cmd == "ssid") {
    config.wifiSsid = value;
    saveConfig();
    Serial.println("[CFG] Stored Wi-Fi SSID");
    syncWifiMode();
  } else if (cmd == "pass") {
    config.wifiPass = value;
    saveConfig();
    Serial.println("[CFG] Stored Wi-Fi password");
    syncWifiMode();
  } else if (cmd == "url") {
    config.cloudUrl = value;
    saveConfig();
    Serial.println("[CFG] Stored cloud URL");
    syncWifiMode();
  } else if (cmd == "token") {
    config.gatewayToken = value;
    saveConfig();
    Serial.println("[CFG] Stored gateway bearer token");
    syncWifiMode();
  } else if (cmd == "profile") {
    HubCommProfile nextProfile;
    if (parseHubProfile(value, nextProfile)) {
      setHubProfile(nextProfile);
    } else {
      Serial.println("[PROFILE] Unknown profile. Use: home, portable, or offgrid.");
    }
  } else if (cmd == "home") {
    setHubProfile(HUB_PROFILE_HOME);
  } else if (cmd == "portable") {
    setHubProfile(HUB_PROFILE_PORTABLE);
  } else if (cmd == "offgrid") {
    setHubProfile(HUB_PROFILE_OFF_GRID);
  } else if (cmd == "provision") {
    String key = value;
    key.toLowerCase();
    if (key == "on" || key == "1" || key == "true") {
      provisioningMode = true;
      prefs.putBool("provision", provisioningMode);
      Serial.println("[PROVISION] Provisioning AP mode enabled.");
      syncWifiMode();
    } else if (key == "off" || key == "0" || key == "false") {
      provisioningMode = false;
      prefs.putBool("provision", provisioningMode);
      Serial.println("[PROVISION] Provisioning AP mode disabled.");
      syncWifiMode();
    } else {
      Serial.println("[PROVISION] Use: provision on|off");
    }
  } else if (cmd == "connect") {
    connectWifi();
  } else if (cmd == "time") {
    syncHubClock(true);
    Serial.println(statusJson());
  } else if (cmd == "show") {
    printConfig();
    Serial.println(statusJson());
  } else if (cmd == "clear") {
    prefs.clear();
    config = HubConfig{String(DEFAULT_STA_SSID), String(DEFAULT_STA_PASS), String(), String()};
    hubProfile = HUB_PROFILE_HOME;
    provisioningMode = false;
    applyHubProfile();
    Serial.println("[CFG] Cleared stored config; restart recommended");
  } else if (cmd == "help" || cmd == "?") {
    printHelp();
  } else {
    Serial.println("[CMD] Unknown command. Type 'help'.");
  }
}

static void serialTask(void *param) {
  (void)param;
  printHelp();
  printConfig();
  for (;;) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\r' || c == '\n') {
        handleSerialLine(serialLine);
        serialLine = "";
      } else {
        serialLine += c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("════════════════════════════════════════════");
  Serial.println("  Bluepaws V4 — Heltec Tracker V2 Home Hub");
  Serial.printf("  Gateway GUID16: %s\n", gatewayHex().c_str());
  Serial.println("  Role: raw LoRa TLV -> base64 HTTPS Supabase relay");
  Serial.println("  Runtime: always-on FreeRTOS tasks, no hub sleep path");
  Serial.println("════════════════════════════════════════════");

  loadConfig();
  statsMutex = xSemaphoreCreateMutex();
  cloudQueue = xQueueCreate(16, sizeof(CloudEntry));
  if (!statsMutex || !cloudQueue) {
    Serial.println("[INIT] FATAL: could not create RTOS primitives");
    while (true) delay(1000);
  }

  startWifi();
  initBleBeacon();
  applyHubProfile();

  xTaskCreatePinnedToCore(loraTask, "lora", 8192, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(cloudTask, "cloud", 8192, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(wifiTask, "wifi", 4096, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(serialTask, "serial", 4096, nullptr, 1, nullptr, 0);

  Serial.println("[INIT] FreeRTOS tasks started.");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(10000));
}
