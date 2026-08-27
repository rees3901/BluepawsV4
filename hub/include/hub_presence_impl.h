// Included at end of main.cpp: uses the existing authenticated HTTPS and local PIN boundary.
// UC6580 UART is owned by one low-priority task; no GNSS/TLS/flash work in LoRa RX.
#include <TinyGPS++.h>
#include <Preferences.h>

struct HubSelf {
    double lat = 0, lon = 0;
    bool hasFix = false;
    uint32_t fixMs = 0;
    uint64_t revision = 0;
    char name[65] = "Home Hub";
    char homeEmoji[65] = "🏡";
    char portableEmoji[65] = "📱";
    char colour[8] = "#38bdf8";
};
static HubSelf hubSelf;
static SemaphoreHandle_t hubSelfMutex;
static std::atomic<bool> hubSelfDirty{false};
static uint32_t lastSelfPostMs = 0;
static bool selfPosted = false;
static bool validHubText(const String &s) {
    if (s.length()==0 || s.length()>64) return false;
    for (unsigned i=0;i<s.length();++i) if ((uint8_t)s[i]<0x20) return false;
    return true;
}

static HubSelf copyHubSelf() {
    HubSelf out;
    if (xSemaphoreTake(hubSelfMutex, portMAX_DELAY)) {
        out = hubSelf;
        xSemaphoreGive(hubSelfMutex);
    }
    return out;
}

static void hubGnssTask(void *) {
    HardwareSerial uart(1);
    TinyGPSPlus gps;
    Preferences prefs;
    prefs.begin("bp-hub-self", false);
    pinMode(PIN_GNSS_POWER, OUTPUT); digitalWrite(PIN_GNSS_POWER, HIGH);
    pinMode(PIN_GNSS_RESET, OUTPUT); digitalWrite(PIN_GNSS_RESET, LOW);
    vTaskDelay(pdMS_TO_TICKS(100));
    digitalWrite(PIN_GNSS_RESET, HIGH);
    vTaskDelay(pdMS_TO_TICKS(500));
    uart.setRxBufferSize(2048);
    uart.begin(115200, SERIAL_8N1, PIN_GNSS_RX, PIN_GNSS_TX);
    Serial.println("[HUB GNSS] UC6580 UART1 RX33/TX34 @115200; awaiting real fix");
    for (;;) {
        // Bound UART work per scheduling slice.
        for (unsigned n = 0; n < 1024 && uart.available(); ++n) gps.encode(uart.read());
        if (gps.location.isUpdated() && gps.location.isValid() && gps.location.age() < 2000) {
            double lat = gps.location.lat(), lon = gps.location.lng();
            if (isfinite(lat) && isfinite(lon) && fabs(lat) <= 90 && fabs(lon) <= 180) {
                if (xSemaphoreTake(hubSelfMutex, pdMS_TO_TICKS(20))) {
                    hubSelf.lat = lat; hubSelf.lon = lon; hubSelf.hasFix = true; hubSelf.fixMs = millis();
                    xSemaphoreGive(hubSelfMutex);
                }
            }
        }
        // Persist settings only on changes, never at the GNSS sentence/report rate.
        if (hubSelfDirty.exchange(false)) {
            auto s = copyHubSelf();
            prefs.putString("name",s.name); prefs.putString("home",s.homeEmoji);
            prefs.putString("portable",s.portableEmoji); prefs.putString("colour",s.colour);
            prefs.putBool("beacon",hubBeaconEnabled.load()); prefs.putULong64("revision",s.revision);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void initHubPresence() {
    hubSelfMutex = xSemaphoreCreateMutex();
    configASSERT(hubSelfMutex);
    Preferences p;
    if (p.begin("bp-hub-self", true)) {
        strlcpy(hubSelf.name,p.getString("name","Home Hub").c_str(),sizeof(hubSelf.name));
        strlcpy(hubSelf.homeEmoji,p.getString("home","🏡").c_str(),sizeof(hubSelf.homeEmoji));
        strlcpy(hubSelf.portableEmoji,p.getString("portable","📱").c_str(),sizeof(hubSelf.portableEmoji));
        strlcpy(hubSelf.colour,p.getString("colour","#38bdf8").c_str(),sizeof(hubSelf.colour));
        hubSelf.revision = p.getULong64("revision",0);
        hubBeaconEnabled = p.getBool("beacon",true); p.end();
    }
    if (xTaskCreatePinnedToCore(hubGnssTask,"hub-gnss",6144,nullptr,1,nullptr,0) != pdPASS)
        Serial.println("[HUB GNSS] Task allocation failed; no position will be fabricated");
}

static String hubPresenceJson(bool cloud) {
    auto s = copyHubSelf();
    JsonDocument doc;
    char guid[5]; snprintf(guid,sizeof(guid),"%04X",(unsigned)GATEWAY_GUID16);
    doc["format"]="hub_status"; doc["ingest_path"]="hub_self"; doc["gateway_guid16"]=guid;
    doc["mode"]=hubCommProfileName(hubCommProfile);
    doc["uptime_s"]=millis()/1000; doc["free_heap"]=ESP.getFreeHeap();
    if (staConnected) doc["wifi_rssi_dbm"]=WiFi.RSSI(); else doc["wifi_rssi_dbm"]=nullptr;
    doc["ble_enabled"]=hubBeaconEnabled.load(); doc["ble_advertising"]=hubBeaconAdvertising.load();
    doc["applied_revision"]=s.revision;
    uint32_t age=(millis()-s.fixMs)/1000;
    if (s.hasFix && age<=604800) {
        doc["latitude"]=s.lat; doc["longitude"]=s.lon; doc["fix_age_s"]=age;
    } else { doc["latitude"]=nullptr; doc["longitude"]=nullptr; doc["fix_age_s"]=nullptr; }
    if (!cloud) {
        doc["display_name"]=s.name; doc["home_emoji"]=s.homeEmoji;
        doc["portable_emoji"]=s.portableEmoji; doc["marker_colour"]=s.colour;
    }
    String json; serializeJson(doc,json); return json;
}

static void handleHubPresence() {
    httpServer.sendHeader("Cache-Control","no-store");
    httpServer.send(200,"application/json",hubPresenceJson(false));
}

static void handleHubPreferences() {
    if (!requireCommandAccess()) return;
    JsonDocument doc;
    if (httpServer.arg("plain").length()>1024 || deserializeJson(doc,httpServer.arg("plain"))) {
        httpServer.send(400,"application/json","{\"error\":\"invalid_preferences\"}"); return;
    }
    auto s=copyHubSelf();
    String name=doc["display_name"] | s.name;
    String home=doc["home_emoji"] | s.homeEmoji;
    String portable=doc["portable_emoji"] | s.portableEmoji;
    String colour=doc["marker_colour"] | s.colour;
    if (!validHubText(name) || !validHubText(home) || !validHubText(portable)
        || !validMarkerColour(colour) || (doc["ble_enabled"].isNull()==false && !doc["ble_enabled"].is<bool>())) {
        httpServer.send(400,"application/json","{\"error\":\"invalid_preferences\"}"); return;
    }
    if (xSemaphoreTake(hubSelfMutex,pdMS_TO_TICKS(50))) {
        strlcpy(hubSelf.name,name.c_str(),sizeof(hubSelf.name));
        strlcpy(hubSelf.homeEmoji,home.c_str(),sizeof(hubSelf.homeEmoji));
        strlcpy(hubSelf.portableEmoji,portable.c_str(),sizeof(hubSelf.portableEmoji));
        strlcpy(hubSelf.colour,colour.c_str(),sizeof(hubSelf.colour));
        if (doc["ble_enabled"].is<bool>()) hubBeaconEnabled=doc["ble_enabled"].as<bool>();
        xSemaphoreGive(hubSelfMutex);
        hubSelfDirty=true;
        httpServer.send(200,"application/json","{\"accepted\":true}");
    } else httpServer.send(503,"application/json","{\"error\":\"busy\"}");
}

static void postHubPresence() {
    // No offline history upload for self status: report ONLY the current state.
    if (!hubProfileAllowsCloudRelay() || !staConnected || cloudToken.isEmpty() || cloudEndpoint.isEmpty()) return;
    // Live collar relay/command responses take priority over hub housekeeping.
    if (uxQueueMessagesWaiting(cloudQueue)>0) return;
    uint32_t now=millis();
    if (selfPosted && now-lastSelfPostMs<55000) return; // + <=5s queue wait = about one minute
    lastSelfPostMs=now; selfPosted=true;
    HTTPClient http;
    http.setConnectTimeout(2000); http.setTimeout(2000);
    if (!http.begin(cloudEndpoint)) return;
    http.addHeader("Content-Type","application/json");
    http.addHeader("Authorization","Bearer "+cloudToken);
    int code=http.POST(hubPresenceJson(true));
    Serial.printf("[HUB SELF] status POST -> HTTP %d\n",code);
    if (code>=200 && code<300 && http.getSize()<4096) {
        JsonDocument response;
        if (!deserializeJson(response,http.getString())) {
            JsonObject settings=response["settings"].as<JsonObject>();
            uint64_t rev=settings["revision"] | uint64_t(0);
            if (rev>copyHubSelf().revision && settings["ble_enabled"].is<bool>()) {
                String name=settings["display_name"] | "";
                String home=settings["home_emoji"] | "";
                String portable=settings["portable_emoji"] | "";
                String colour=settings["marker_colour"] | "";
                if (validHubText(name) && validHubText(home) && validHubText(portable) && validMarkerColour(colour)
                    && xSemaphoreTake(hubSelfMutex,pdMS_TO_TICKS(50))) {
                    strlcpy(hubSelf.name,name.c_str(),sizeof(hubSelf.name));
                    strlcpy(hubSelf.homeEmoji,home.c_str(),sizeof(hubSelf.homeEmoji));
                    strlcpy(hubSelf.portableEmoji,portable.c_str(),sizeof(hubSelf.portableEmoji));
                    strlcpy(hubSelf.colour,colour.c_str(),sizeof(hubSelf.colour));
                    hubSelf.revision=rev; hubBeaconEnabled=settings["ble_enabled"].as<bool>();
                    xSemaphoreGive(hubSelfMutex); hubSelfDirty=true;
                }
            }
        }
    }
    noteCloudPostResult(code);
    http.end();
}
