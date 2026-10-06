// Personal-only UI adapter. Canonical tasks, packets, settings and routes remain
// compiled from their original files, in the same translation unit for read-only
// access to their state. Do not modify canonical main.cpp to add this display.
// Expose dependencies to PlatformIO's scanner across the included .cpp adapter.
#include <Preferences.h>
#include <RadioLib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <BLEDevice.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TinyGPS++.h>
#include <bp_protocol.h>
#include "../../../collar/personal-esp32s3/include/personal_radio_timing.h"
#include <bp_crypto.h>
#define setup canonicalHubSetup
#define loop canonicalHubLoop
#include "../../platformio/src/main.cpp"
#undef setup
#undef loop

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "display_button.h"
#include <stdarg.h>

namespace personal_display {
// Heltec V2: ST7735 160x80 TFT, not an SSD1306 OLED. GPIO3 is shared Vext
// and is owned by the canonical GNSS task: never cycle it to blank the display.
constexpr int Cs = 38, Dc = 40, Reset = 39, Clock = 41, Mosi = 42, Backlight = 21;
constexpr int UserButton = 0;
constexpr uint8_t PageCount = 8;
SPIClass displaySPI(FSPI); // SX1262 uses HSPI; these buses must stay independent.
class TrackerDisplay : public Adafruit_ST7735 {
public:
    TrackerDisplay() : Adafruit_ST7735(&displaySPI, Cs, Dc, Reset) {}
    void beginTracker() {
        initR(INITR_MINI160x80);
        // Exact V2 offsets from Heltec's HT_st7735.h (not V1's 1,26).
        _colstart = 24; _rowstart = 0;
        setRotation(1);
        const uint8_t orientation = ST77XX_MADCTL_MY | ST77XX_MADCTL_MV | ST7735_MADCTL_BGR;
        sendCommand(ST77XX_MADCTL, &orientation, 1);
        invertDisplay(false); // V2 vendor initialization explicitly disables inversion.
        setSPISpeed(16000000);
    }
} screen;
Button button;
uint8_t page = 0;
bool redraw = true;
uint32_t refreshed = 0;
char previous[5][27] = {};
uint16_t previousColour[5] = {};

const char* compactProfile(uint8_t profile) {
    switch (profile) {
        case PROFILE_NORMAL: return "NORMAL";
        case PROFILE_POWERSAVE: return "SAVE";
        case PROFILE_ACTIVE: return "ACTIVE";
        case PROFILE_LOST: return "LOST";
        default: return "UNKNOWN";
    }
}
const char* compactStatus(uint8_t state) {
    switch (state) {
        case STATUS_HOME: return "HOME";
        case STATUS_OUT_AND_ABOUT: return "AWAY";
        case STATUS_LOST: return "LOST";
        case STATUS_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
void ageText(uint32_t seconds, char* out, size_t size) {
    if (seconds < 60) snprintf(out,size,"%lus",(unsigned long)seconds);
    else if (seconds < 3600) snprintf(out,size,"%lum%02lus",(unsigned long)(seconds/60),(unsigned long)(seconds%60));
    else snprintf(out,size,"%luh%02lum",(unsigned long)(seconds/3600),(unsigned long)((seconds/60)%60));
}
void row(uint8_t index, uint16_t colour, const char* format, ...) {
    char text[27];
    va_list args; va_start(args,format); vsnprintf(text,sizeof(text),format,args); va_end(args);
    // Fixed ASCII font cannot render arbitrary device/SSID Unicode safely.
    for (char* p=text; *p; ++p) if (uint8_t(*p)<32 || uint8_t(*p)>126) *p='?';
    if (!redraw && colour==previousColour[index] && strcmp(text,previous[index])==0) return;
    strlcpy(previous[index],text,sizeof(previous[index])); previousColour[index]=colour;
    screen.fillRect(0,21+index*10,160,10,ST77XX_BLACK);
    screen.setTextSize(1); screen.setTextColor(colour); screen.setCursor(2,22+index*10); screen.print(text);
}
void header(const char* title) {
    if (!redraw) return;
    memset(previous,0,sizeof(previous));
    memset(previousColour,0,sizeof(previousColour));
    screen.fillScreen(ST77XX_BLACK);
    screen.setTextWrap(false); screen.setTextSize(2); screen.setTextColor(ST77XX_CYAN);
    screen.setCursor(2,2); screen.print(title);
    screen.drawFastHLine(0,19,160,0x3186);
    screen.setTextSize(1); screen.setTextColor(0xAD55); screen.setCursor(2,73);
    screen.printf("%u/8 TAP>NEXT  HOLD>HOME",page+1);
}
void overview(uint32_t now) {
    char title[14]; snprintf(title,sizeof(title),"HUB %04X",unsigned(GATEWAY_GUID16)); header(title);
    const bool wifi = staConnected.load();
    row(0,ST77XX_WHITE,"Mode: %s",hubCommProfileName(hubCommProfile.load()));
    row(1,wifi?ST77XX_GREEN:ST77XX_ORANGE,wifi?"WiFi: connected %d dBm":"WiFi: disconnected",wifi?WiFi.RSSI():0);
    const uint32_t last=hubConnectivity.last_cloud_success_ms.load();
    char age[18]; ageText((now-last)/1000,age,sizeof(age));
    row(2,hubConnectivity.cloud_reachable.load()?ST77XX_GREEN:ST77XX_ORANGE,
        last?"Cloud last OK: %s":"Cloud: waiting",age);
    row(3,hubBeaconAdvertising.load()?ST77XX_GREEN:ST77XX_YELLOW,
        "Home beacon: %s",hubBeaconAdvertising.load()?"ON":"OFF");
    row(4,hubConnectivity.lora_rx_active.load()?ST77XX_GREEN:ST77XX_ORANGE,
        "LoRa: %s  queue:%u",hubConnectivity.lora_rx_active.load()?"listening":"starting",
        cloudQueue?unsigned(uxQueueMessagesWaiting(cloudQueue)):0);
}
void network(uint32_t now) {
    header("NETWORK");
    const bool online=staConnected.load();
    row(0,ST77XX_WHITE,"%s",online?WiFi.SSID().c_str():"No WiFi connection");
    row(1,ST77XX_CYAN,"IP %s",online?WiFi.localIP().toString().c_str():"--");
    row(2,ST77XX_WHITE,"AP: %s",hubApEnabled.load()?WiFi.softAPIP().toString().c_str():"off");
    row(3,ST77XX_WHITE,"Cloud failures: %u",unsigned(hubConnectivity.cloud_failures.load()));
    char age[18]; ageText(now/1000,age,sizeof(age));
    row(4,ST77XX_WHITE,"Uptime %s  heap %uk",age,unsigned(ESP.getFreeHeap()/1024));
}
void collar(uint16_t id, uint32_t now) {
    char title[14]; snprintf(title,sizeof(title),"COLLAR %u",id); header(title);
    device_state_t d{}; bool found=false;
    // Snapshot under the existing mutex; never hold any hub lock while drawing.
    if (xSemaphoreTake(deviceMutex,pdMS_TO_TICKS(5))) {
        auto* source=findDevice(id); if (source) { d=*source; found=true; }
        xSemaphoreGive(deviceMutex);
    } else {
        row(0,ST77XX_YELLOW,"Refreshing..."); return;
    }
    if (!found) {
        row(0,ST77XX_YELLOW,"No reports received"); row(1,ST77XX_WHITE,"Waiting for this collar");
        row(2,ST77XX_WHITE,""); row(3,ST77XX_WHITE,""); row(4,ST77XX_WHITE,""); return;
    }
    const uint32_t elapsed=(now-d.local_millis)/1000;
    char age[18]; ageText(elapsed,age,sizeof(age));
    row(0,d.error_present?ST77XX_ORANGE:ST77XX_WHITE,"%s / %s",compactStatus(d.status),compactProfile(d.profile));
    row(1,d.heard_this_boot?ST77XX_WHITE:ST77XX_YELLOW,
        d.heard_this_boot?"RX %s  %d dBm":"Stored report; no live RX",age,d.rssi);
    if (d.heard_this_boot && d.fix_age_s!=UINT16_MAX) {
        ageText(uint32_t(d.fix_age_s)+elapsed,age,sizeof(age));
        row(2,(d.has_gps && uint32_t(d.fix_age_s)+elapsed<=GPS_STALE_THRESHOLD_S)?ST77XX_GREEN:ST77XX_YELLOW,
            "GPS age %s  SNR %.1f",age,d.snr);
    } else row(2,ST77XX_YELLOW,"GPS age unknown");
    row(3,d.sync_state==BP_JOURNAL_VALIDATED?ST77XX_GREEN:ST77XX_ORANGE,"Cloud: %s",
        d.sync_state==BP_JOURNAL_VALIDATED?"accepted":d.sync_state==BP_JOURNAL_REJECTED?"rejected":"pending");
    char command[20]="refreshing"; uint32_t latest=0;
    if (xSemaphoreTake(pendingMutex,pdMS_TO_TICKS(5))) {
        strlcpy(command,"none",sizeof(command));
        for (const auto& c:pendingCmds) if (c.targetId==id && c.state && c.createdAtMs>=latest) {
            latest=c.createdAtMs; strlcpy(command,c.state,sizeof(command));
        }
        xSemaphoreGive(pendingMutex);
    }
    row(4,ST77XX_WHITE,"Cmd: %s",command);
}
void gpsPage(uint32_t now) {
    header("HUB GPS");
    HubSelf state;
    if (!xSemaphoreTake(hubSelfMutex,pdMS_TO_TICKS(5))) return;
    state=hubSelf; xSemaphoreGive(hubSelfMutex);
    if (state.hasFix) {
        char age[18]; ageText((now-state.fixMs)/1000,age,sizeof(age));
        row(0,ST77XX_WHITE,"Hub fix age: %s",age);
        row(1,ST77XX_CYAN,"Lat %.6f",state.lat); row(2,ST77XX_CYAN,"Lon %.6f",state.lon);
    } else {
        row(0,ST77XX_YELLOW,"No hub GPS fix yet"); row(1,ST77XX_WHITE,"Needs a clear sky view"); row(2,ST77XX_WHITE,"");
    }
    row(3,ST77XX_WHITE,"Hub battery: unmeasured");
    row(4,ST77XX_WHITE,"GPS is hub's own position");
}
void radioPage() {
    header("RADIO / CLOUD");
    row(0,ST77XX_CYAN,"%.1f MHz SF%d BW%.0f",LORA_FREQUENCY,LORA_SPREADING,LORA_BANDWIDTH);
    row(1,ST77XX_WHITE,"Relay queue %u/%u",cloudQueue?unsigned(uxQueueMessagesWaiting(cloudQueue)):0,CLOUD_QUEUE_SIZE);
    row(2,ST77XX_WHITE,"Command queue %u/%u",cmdQueue?unsigned(uxQueueMessagesWaiting(cmdQueue)):0,CMD_QUEUE_SIZE);
    row(3,ST77XX_WHITE,"Store queue %u/%u",storageQueue?unsigned(uxQueueMessagesWaiting(storageQueue)):0,STORAGE_QUEUE_SIZE);
    row(4,ST77XX_YELLOW,"RX alone is not cloud ACK");
}
void begin() {
    pinMode(UserButton,INPUT_PULLUP);
    pinMode(Backlight,OUTPUT); digitalWrite(Backlight,LOW);
    displaySPI.begin(Clock,-1,Mosi,Cs);
    screen.beginTracker();
    digitalWrite(Backlight,HIGH);
    Serial.println("[DISPLAY] V2 ST7735 160x80 on FSPI; USER GPIO0: tap next, hold home");
}
void tick() {
    const uint32_t now=millis();
    const auto action=button.tick(digitalRead(UserButton)==LOW,now);
    if (action!=ButtonAction::None) {
        page=action==ButtonAction::Home?0:(page+1)%PageCount; redraw=true;
        Serial.printf("[DISPLAY] page %u/%u\n",page+1,PageCount);
    }
    if (!redraw && now-refreshed<1000) return;
    refreshed=now;
    if (page==0) overview(now);
    else if (page==1) network(now);
    else if (page<=5) collar(3001+page-2,now);
    else if (page==6) gpsPage(now);
    else radioPage();
    redraw=false;
}
}

void setup() {
    canonicalHubSetup();
    personal_display::begin();
}
void loop() {
    personal_display::tick();
    vTaskDelay(pdMS_TO_TICKS(10));
}
