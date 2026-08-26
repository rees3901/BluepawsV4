#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <RadioLib.h>

#include "bp_config.h"
#include "bp_protocol.h"
#include "pins.h"

namespace {

constexpr uint8_t HISTORY_SIZE = 10;
constexpr uint8_t MAX_SOURCES = 8;
constexpr uint16_t SCREEN_W = 320;
constexpr uint16_t SCREEN_H = 170;
constexpr uint8_t LINE_H = 16;
constexpr uint8_t STATUS_H = 14;
constexpr uint16_t BODY_BOTTOM = SCREEN_H - STATUS_H;

SPIClass loraSpi(HSPI);
SX1262 radio = new Module(SNIFFER_LORA_NSS, SNIFFER_LORA_DIO1,
                          SNIFFER_LORA_RST, SNIFFER_LORA_BUSY, loraSpi);
Adafruit_ST7789 tft(SNIFFER_TFT_CS, SNIFFER_TFT_DC, SNIFFER_TFT_RST);

volatile bool packetReady = false;

struct PacketRecord {
    uint32_t number = 0;
    uint32_t receivedMs = 0;
    float rssi = 0;
    float snr = 0;
    uint8_t length = 0;
    uint8_t bytes[BP_MAX_PACKET_SIZE] = {};
    bool structureValid = false;
};

struct SourceStat {
    uint16_t id = 0;
    uint32_t count = 0;
    uint32_t lastMs = 0;
};

PacketRecord history[HISTORY_SIZE];
SourceStat sources[MAX_SOURCES];
uint8_t historyCount = 0;
int8_t historyHead = -1;
uint8_t viewOffset = 0;
uint8_t sourceCount = 0;
uint32_t receivedCount = 0;
uint32_t invalidCount = 0;
uint32_t radioErrorCount = 0;
uint32_t lastPacketMs = 0;
bool summaryView = false;

struct ButtonState {
    uint8_t pin;
    int8_t direction;
    bool previous = HIGH;
    bool pending = false;
    uint32_t edgeMs = 0;
};

ButtonState userButton;
ButtonState bootButton;

void IRAM_ATTR onRadioPacket() { packetReady = true; }

const char *directionLabel(const uint8_t *packet) {
    if (bp_is_hub_id(pkt_source_id(packet))) return "DOWN";
    if (pkt_tx_reason(packet) == TX_ACK) return "ACK";
    return "UP";
}

void clearBody() {
    tft.fillRect(0, 0, SCREEN_W, BODY_BOTTOM, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextWrap(false);
}

void drawLine(uint8_t row, uint16_t color, const char *format, ...) {
    char text[64];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    tft.setTextColor(color, ST77XX_BLACK);
    tft.setCursor(2, row * LINE_H);
    tft.print(text);
}

void drawStatus() {
    tft.fillRect(0, BODY_BOTTOM, SCREEN_W, STATUS_H, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setCursor(2, BODY_BOTTOM + 3);
    tft.setTextColor(lastPacketMs && millis() - lastPacketMs < 120000UL
                         ? ST77XX_GREEN : ST77XX_YELLOW,
                     ST77XX_BLACK);
    uint32_t age = lastPacketMs ? (millis() - lastPacketMs) / 1000 : 0;
    tft.printf("%s age:%lus ok:%lu bad:%lu radio:%lu",
               summaryView ? "SUMMARY" : (viewOffset ? "FROZEN" : "LIVE"),
               (unsigned long)age, (unsigned long)receivedCount,
               (unsigned long)invalidCount, (unsigned long)radioErrorCount);
    tft.setTextSize(2);
}

void tallySource(uint16_t id) {
    for (uint8_t i = 0; i < sourceCount; ++i) {
        if (sources[i].id == id) {
            ++sources[i].count;
            sources[i].lastMs = millis();
            return;
        }
    }
    if (sourceCount < MAX_SOURCES) {
        sources[sourceCount].id = id;
        sources[sourceCount].count = 1;
        sources[sourceCount].lastMs = millis();
        ++sourceCount;
    }
}

void renderSummary() {
    clearBody();
    drawLine(0, ST77XX_CYAN, "TLV v1.2 SUMMARY");
    drawLine(1, ST77XX_YELLOW, "%.1f SF%d BW%.0f", LORA_FREQUENCY,
             LORA_SPREADING, LORA_BANDWIDTH);
    drawLine(2, ST77XX_WHITE, "ok:%lu bad:%lu",
             (unsigned long)receivedCount, (unsigned long)invalidCount);
    for (uint8_t i = 0; i < sourceCount && i < 6; ++i) {
        uint32_t age = (millis() - sources[i].lastMs) / 1000;
        drawLine(3 + i, age < 300 ? ST77XX_GREEN : age < 600 ? ST77XX_YELLOW : ST77XX_RED,
                 "%04X %-6s x%lu %lus", sources[i].id,
                 bp_is_hub_id(sources[i].id) ? "HUB" : "COLLAR",
                 (unsigned long)sources[i].count, (unsigned long)age);
    }
    drawStatus();
}

void renderPacket(uint8_t offset) {
    if (!historyCount) return;
    int index = (historyHead - offset + HISTORY_SIZE) % HISTORY_SIZE;
    const PacketRecord &record = history[index];
    clearBody();

    if (!record.structureValid) {
        drawLine(0, ST77XX_RED, "INVALID #%lu", (unsigned long)record.number);
        drawLine(1, ST77XX_WHITE, "len:%u R:%.0f S:%.1f", record.length,
                 record.rssi, record.snr);
        drawLine(2, ST77XX_YELLOW, "Unsupported/malformed");
        drawLine(3, ST77XX_YELLOW, "Auth not evaluated");
        drawStatus();
        return;
    }

    const uint8_t *packet = record.bytes;
    uint16_t source = pkt_source_id(packet);
    uint16_t destination = pkt_destination_id(packet);
    uint8_t flags = pkt_flags(packet);
    bool hasGnss = (flags & FLAG_GNSS_VALID) != 0;
    drawLine(0, pkt_tx_reason(packet) == TX_CONFIG ? ST77XX_ORANGE : ST77XX_GREEN,
             "%s #%lu R%.0f S%.1f", directionLabel(packet),
             (unsigned long)record.number, record.rssi, record.snr);
    drawLine(1, ST77XX_CYAN, "v%u %04X > %04X", pkt_version(packet), source, destination);
    drawLine(2, ST77XX_WHITE, "seq:%u %s", pkt_msg_seq(packet),
             bp_tx_reason_display(pkt_tx_reason(packet)));
    drawLine(3, ST77XX_WHITE, "%s / %s", bp_status_display((bp_status_t)pkt_status(packet)),
             bp_profile_name((bp_profile_t)pkt_power_profile(packet)));
    drawLine(4, ST77XX_WHITE, "flags:%02X batt:%umV", flags, pkt_batt_mV(packet));
    if (hasGnss) {
        drawLine(5, ST77XX_WHITE, "lat:%.5f", pkt_lat_e7(packet) / 10000000.0);
        drawLine(6, ST77XX_WHITE, "lon:%.5f", pkt_lon_e7(packet) / 10000000.0);
    } else {
        drawLine(5, ST77XX_YELLOW, "GNSS: no new fix");
        drawLine(6, ST77XX_WHITE, "TLV:%u total:%u", pkt_tlv_len(packet), record.length);
    }
    drawLine(7, ST77XX_YELLOW, "AUTH: not checked");
    drawStatus();
}

void renderCurrent() {
    if (summaryView) renderSummary();
    else renderPacket(viewOffset);
}

void handleButton(ButtonState &button) {
    bool current = digitalRead(button.pin);
    uint32_t now = millis();
    if (button.previous == HIGH && current == LOW && now - button.edgeMs > 180) {
        button.edgeMs = now;
        if (button.pending && now - button.edgeMs < 400) {
            button.pending = false;
            summaryView = true;
            renderSummary();
        } else {
            button.pending = true;
        }
    }
    button.previous = current;
    if (button.pending && now - button.edgeMs > 400) {
        button.pending = false;
        if (summaryView) {
            summaryView = false;
        } else if (historyCount) {
            viewOffset = (viewOffset + button.direction + historyCount) % historyCount;
        }
        renderCurrent();
    }
}

void printHex(const uint8_t *bytes, uint8_t length) {
    Serial.print("[RX] Hex:");
    for (uint8_t i = 0; i < length; ++i) Serial.printf(" %02X", bytes[i]);
    Serial.println();
}

void handlePacket() {
    uint8_t bytes[BP_MAX_PACKET_SIZE];
    size_t length = radio.getPacketLength(false);
    int state = RADIOLIB_ERR_PACKET_TOO_LONG;
    if (length > 0 && length <= BP_MAX_PACKET_SIZE) state = radio.readData(bytes, length);
    float rssi = radio.getRSSI();
    float snr = radio.getSNR();
    radio.startReceive();

    if (state != RADIOLIB_ERR_NONE) {
        ++radioErrorCount;
        Serial.printf("[RX] Radio error=%d len=%u\n", state, (unsigned)length);
        drawStatus();
        return;
    }

    ++receivedCount;
    historyHead = (historyHead + 1) % HISTORY_SIZE;
    if (historyCount < HISTORY_SIZE) ++historyCount;
    PacketRecord &record = history[historyHead];
    record = PacketRecord();
    record.number = receivedCount;
    record.receivedMs = millis();
    record.rssi = rssi;
    record.snr = snr;
    record.length = (uint8_t)length;
    memcpy(record.bytes, bytes, length);
    record.structureValid = pkt_validate_structure(bytes, (uint8_t)length);
    lastPacketMs = millis();

    Serial.printf("[RX] #%lu len=%u structure=%s auth=unchecked rssi=%.1f snr=%.1f\n",
                  (unsigned long)record.number, record.length,
                  record.structureValid ? "valid" : "invalid", rssi, snr);
    printHex(bytes, (uint8_t)length);
    if (record.structureValid) {
        uint16_t source = pkt_source_id(bytes);
        tallySource(source);
        uint16_t destination = pkt_destination_id(bytes);
        const char *sourceRole = bp_is_hub_id(source) ? "hub" : "collar";
        const char *destinationRole = destination == BP_DEST_CLOUD ? "cloud" :
                                      destination == BP_ID_BROADCAST ? "broadcast" :
                                      bp_is_hub_id(destination) ? "hub" : "collar";
        Serial.printf("[RX] v=%u source=%04X(%s) destination=%04X(%s) seq=%u "
                      "status=%s profile=%s flags=0x%02X reason=%s\n",
                      pkt_version(bytes), source, sourceRole,
                      destination, destinationRole,
                      pkt_msg_seq(bytes), bp_status_display((bp_status_t)pkt_status(bytes)),
                      bp_profile_name((bp_profile_t)pkt_power_profile(bytes)),
                      pkt_flags(bytes), bp_tx_reason_display(pkt_tx_reason(bytes)));
        Serial.printf("[RX] lat=%.7f lon=%.7f batt=%umV acc=%um fix_age=%us sats=%u tlv_len=%u\n",
                      pkt_lat_e7(bytes) / 10000000.0, pkt_lon_e7(bytes) / 10000000.0,
                      pkt_batt_mV(bytes), pkt_acc_m(bytes), pkt_fix_age_s(bytes),
                      pkt_sat_count(bytes), pkt_tlv_len(bytes));
    } else {
        ++invalidCount;
    }

    if (!summaryView && viewOffset == 0) renderPacket(0);
    else if (summaryView) renderSummary();

    digitalWrite(SNIFFER_TFT_BL, LOW);
    delay(35);
    digitalWrite(SNIFFER_TFT_BL, HIGH);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("[T190] Bluepaws V4 passive TLV v1.2 sniffer starting");
    Serial.println("[T190] Structure validation only; HMAC authentication is not checked");

    pinMode(SNIFFER_HEARTBEAT, OUTPUT);
    userButton.pin = SNIFFER_USER_BTN;
    userButton.direction = 1;
    bootButton.pin = SNIFFER_BOOT_BTN;
    bootButton.direction = -1;
    pinMode(SNIFFER_USER_BTN, INPUT_PULLUP);
    pinMode(SNIFFER_BOOT_BTN, INPUT_PULLUP);
    pinMode(SNIFFER_TFT_POWER, OUTPUT);
    digitalWrite(SNIFFER_TFT_POWER, LOW);
    pinMode(SNIFFER_TFT_BL, OUTPUT);
    digitalWrite(SNIFFER_TFT_BL, HIGH);

    SPI.begin(SNIFFER_TFT_SCK, -1, SNIFFER_TFT_MOSI, SNIFFER_TFT_CS);
    tft.init(170, 320);
    tft.setRotation(1);
    tft.invertDisplay(true);
    clearBody();
    drawLine(0, ST77XX_CYAN, "Bluepaws V4 Sniffer");
    drawLine(1, ST77XX_WHITE, "TLV v1.2 passive RX");
    drawLine(2, ST77XX_YELLOW, "%.1f SF%d BW%.0f", LORA_FREQUENCY,
             LORA_SPREADING, LORA_BANDWIDTH);
    drawLine(3, ST77XX_WHITE, "Waiting for packets...");
    drawLine(5, ST77XX_YELLOW, "Auth not checked");
    drawStatus();

    loraSpi.begin(SNIFFER_LORA_SCK, SNIFFER_LORA_MISO,
                  SNIFFER_LORA_MOSI, SNIFFER_LORA_NSS);
    int state = radio.begin(LORA_FREQUENCY);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[T190] Radio init failed: %d\n", state);
        drawLine(4, ST77XX_RED, "RADIO FAIL %d", state);
        return;
    }
    radio.setSpreadingFactor(LORA_SPREADING);
    radio.setBandwidth(LORA_BANDWIDTH);
    radio.setCodingRate(LORA_CODING_RATE);
    radio.setPreambleLength(LORA_PREAMBLE_LEN);
    radio.setSyncWord(LORA_SYNC_WORD);
    radio.setCRC(LORA_CRC_ENABLED);
    radio.setDio1Action(onRadioPacket);
    radio.startReceive();
    Serial.printf("[T190] RX ready %.1f MHz SF%d BW%.0f CR4/%d preamble %d sync 0x%02X CRC on\n",
                  LORA_FREQUENCY, LORA_SPREADING, LORA_BANDWIDTH,
                  LORA_CODING_RATE, LORA_PREAMBLE_LEN, LORA_SYNC_WORD);
}

void loop() {
    static uint32_t lastHeartbeat = 0;
    if (millis() - lastHeartbeat >= 500) {
        lastHeartbeat = millis();
        digitalWrite(SNIFFER_HEARTBEAT, !digitalRead(SNIFFER_HEARTBEAT));
    }
    handleButton(userButton);
    handleButton(bootButton);
    if (packetReady) {
        packetReady = false;
        handlePacket();
    }
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus >= 1000) {
        lastStatus = millis();
        drawStatus();
    }
    delay(2);
}
