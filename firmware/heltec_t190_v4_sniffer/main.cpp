/*
  Bluepaws V4 — Heltec Vision Master T190 plain TLV sniffer

  Receive-only diagnostic firmware for the ESP32-S3 + SX1262 T190 board.
  It listens using the locked Bluepaws V4 LoRa PHY profile and prints raw TLV
  packets to USB serial. The built-in TFT shows a compact last-packet summary.

  Legacy V3 sniffer values are intentionally not used here.
*/

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include <bp_config.h>
#include <bp_protocol.h>

// Heltec Vision Master T190 pins from the local handover notes.
static constexpr int PIN_LORA_SCLK = 9;
static constexpr int PIN_LORA_MOSI = 10;
static constexpr int PIN_LORA_MISO = 11;
static constexpr int PIN_LORA_NSS = 8;
static constexpr int PIN_LORA_RST = 12;
static constexpr int PIN_LORA_BUSY = 13;
static constexpr int PIN_LORA_DIO1 = 14;

static constexpr int PIN_TFT_SCLK = 38;
static constexpr int PIN_TFT_MOSI = 48;
static constexpr int PIN_TFT_CS = 39;
static constexpr int PIN_TFT_DC = 47;
static constexpr int PIN_TFT_RST = 40;
static constexpr int PIN_TFT_BL = 17;
static constexpr int PIN_TFT_POWER = 7;  // active LOW

static constexpr int PIN_USER_BUTTON = 21;
static constexpr int PIN_BOOT_BUTTON = 0;
static constexpr int PIN_HEARTBEAT = 35;

static constexpr uint16_t COLOR_BG = 0x0008;
static constexpr uint16_t COLOR_PANEL = 0x018C;
static constexpr uint16_t COLOR_BLUE = 0x04BF;
static constexpr uint16_t COLOR_GREEN = 0x07E0;
static constexpr uint16_t COLOR_RED = 0xF800;
static constexpr uint16_t COLOR_YELLOW = 0xFFE0;
static constexpr uint16_t COLOR_WHITE = 0xFFFF;
static constexpr uint16_t COLOR_MUTED = 0x9CF3;

SPIClass loraSPI(FSPI);
SPIClass tftSPI(HSPI);

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);
Adafruit_ST7789 tft(&tftSPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

static uint32_t packetCount = 0;
static uint32_t validCount = 0;
static uint32_t errorCount = 0;
static uint32_t lastHeartbeatMs = 0;

static void printHexLine(const uint8_t *bytes, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (bytes[i] < 0x10) {
      Serial.print('0');
    }
    Serial.print(bytes[i], HEX);
    if (i + 1 < len) {
      Serial.print(' ');
    }
  }
  Serial.println();
}

static size_t inferBluepawsLength(const uint8_t *buf, size_t receivedLen) {
  if (receivedLen >= BP_HEADER_SIZE && buf[0] == BP_PROTOCOL_VERSION && buf[31] <= BP_MAX_TLV_SIZE) {
    const size_t packetLen = BP_HEADER_SIZE + buf[31] + BP_AUTH_TAG_SIZE;
    if (packetLen <= BP_MAX_PACKET_SIZE) {
      return packetLen;
    }
  }
  return receivedLen;
}

static void drawHeader(const char *state, uint16_t stateColor) {
  tft.fillScreen(COLOR_BG);
  tft.fillRect(0, 0, 320, 34, COLOR_PANEL);
  tft.setTextWrap(false);
  tft.setTextColor(COLOR_BLUE);
  tft.setTextSize(1);
  tft.setCursor(8, 6);
  tft.print("BLUEPAWS V4");
  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 18);
  tft.print("TLV Sniffer");

  tft.setTextSize(1);
  tft.setTextColor(stateColor);
  tft.setCursor(220, 10);
  tft.print(state);
}

static void drawListening() {
  drawHeader("LISTENING", COLOR_GREEN);
  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, 48);
  tft.printf("%.1f MHz  SF%d  BW %.0f", LORA_FREQUENCY, LORA_SPREADING, LORA_BANDWIDTH);
  tft.setCursor(8, 64);
  tft.printf("CR 4/%d  PRE %d  SW 0x%02X  CRC %s",
             LORA_CODING_RATE,
             LORA_PREAMBLE_LEN,
             LORA_SYNC_WORD,
             LORA_CRC_ENABLED ? "ON" : "OFF");
  tft.setTextColor(COLOR_MUTED);
  tft.setCursor(8, 92);
  tft.print("Waiting for raw TLV packets...");
}

static void drawPacket(const uint8_t *buf, size_t len, bool valid, float rssi, float snr) {
  drawHeader(valid ? "VALID" : "RAW/ERR", valid ? COLOR_GREEN : COLOR_YELLOW);
  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, 44);
  tft.printf("Packets %lu  valid %lu  errors %lu", packetCount, validCount, errorCount);
  tft.setCursor(8, 60);
  tft.printf("RSSI %.1f dBm  SNR %.1f dB", rssi, snr);
  tft.setCursor(8, 76);
  tft.printf("Len %u bytes", static_cast<unsigned>(len));

  if (valid) {
    tft.setCursor(8, 100);
    tft.printf("Device %u   Seq %u", pkt_device_id(buf), pkt_msg_seq(buf));
    tft.setCursor(8, 116);
    tft.printf("Status %s  Profile %s",
               bp_status_display(static_cast<bp_status_t>(pkt_status(buf))),
               bp_profile_name(static_cast<bp_profile_t>(pkt_power_profile(buf))));
    tft.setCursor(8, 132);
    tft.printf("Reason %s", bp_tx_reason_display(pkt_tx_reason(buf)));
    tft.setCursor(8, 148);
    tft.printf("Lat %.7f", pkt_lat_e7(buf) / 10000000.0);
    tft.setCursor(8, 164);
    tft.printf("Lon %.7f", pkt_lon_e7(buf) / 10000000.0);
  } else {
    tft.setTextColor(COLOR_YELLOW);
    tft.setCursor(8, 104);
    tft.print("Packet received but TLV v1.1 structure did not validate.");
  }
}

static void configureDisplay() {
  pinMode(PIN_TFT_POWER, OUTPUT);
  digitalWrite(PIN_TFT_POWER, LOW);
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);

  tftSPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.init(170, 320);
  tft.setRotation(1);
  tft.invertDisplay(true);
  drawListening();
}

static void configureRadioOrHalt() {
  loraSPI.begin(PIN_LORA_SCLK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  Serial.println("[LORA] Initialising SX1262 with Bluepaws V4 locked profile...");

  int state = radio.begin(LORA_FREQUENCY);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] FATAL init failed: %d\n", state);
    drawHeader("LORA FAIL", COLOR_RED);
    tft.setCursor(8, 52);
    tft.setTextColor(COLOR_RED);
    tft.printf("RadioLib code %d", state);
    while (true) {
      digitalWrite(PIN_HEARTBEAT, !digitalRead(PIN_HEARTBEAT));
      delay(250);
    }
  }

  radio.setSpreadingFactor(LORA_SPREADING);
  radio.setBandwidth(LORA_BANDWIDTH);
  radio.setCodingRate(LORA_CODING_RATE);
  radio.setPreambleLength(LORA_PREAMBLE_LEN);
  radio.setSyncWord(LORA_SYNC_WORD);
  radio.setCRC(LORA_CRC_ENABLED);

  Serial.printf("[LORA] Ready %.1f MHz | SF%d | BW %.0f kHz | CR 4/%d | preamble %d | sync 0x%02X | CRC %s\n",
                LORA_FREQUENCY,
                LORA_SPREADING,
                LORA_BANDWIDTH,
                LORA_CODING_RATE,
                LORA_PREAMBLE_LEN,
                LORA_SYNC_WORD,
                LORA_CRC_ENABLED ? "on" : "off");
}

static void handlePacket(uint8_t *buf, size_t rawLen) {
  const size_t len = inferBluepawsLength(buf, rawLen);
  const bool valid = pkt_validate_structure(buf, static_cast<uint8_t>(len));
  const float rssi = radio.getRSSI();
  const float snr = radio.getSNR();

  packetCount++;
  if (valid) {
    validCount++;
  } else {
    errorCount++;
  }

  Serial.println();
  Serial.printf("[RX] #%lu len=%u valid=%s rssi=%.1f snr=%.1f\n",
                packetCount,
                static_cast<unsigned>(len),
                valid ? "yes" : "no",
                rssi,
                snr);
  Serial.print("[RX] Hex: ");
  printHexLine(buf, len);

  if (valid) {
    Serial.printf("[RX] device=%u seq=%u unix=%lu status=%s profile=%s flags=0x%02X reason=%s\n",
                  pkt_device_id(buf),
                  pkt_msg_seq(buf),
                  static_cast<unsigned long>(pkt_time_unix(buf)),
                  bp_status_display(static_cast<bp_status_t>(pkt_status(buf))),
                  bp_profile_name(static_cast<bp_profile_t>(pkt_power_profile(buf))),
                  pkt_flags(buf),
                  bp_tx_reason_display(pkt_tx_reason(buf)));
    Serial.printf("[RX] lat=%.7f lon=%.7f batt=%umV acc=%um sats=%u tlv_len=%u\n",
                  pkt_lat_e7(buf) / 10000000.0,
                  pkt_lon_e7(buf) / 10000000.0,
                  pkt_batt_mV(buf),
                  pkt_acc_m(buf),
                  pkt_sat_count(buf),
                  pkt_tlv_len(buf));
  }

  drawPacket(buf, len, valid, rssi, snr);
}

void setup() {
  pinMode(PIN_HEARTBEAT, OUTPUT);
  pinMode(PIN_USER_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  Serial.begin(115200);
  delay(1200);

  Serial.println();
  Serial.println("════════════════════════════════════════════");
  Serial.println("  Bluepaws V4 — Heltec T190 TLV Sniffer");
  Serial.println("  Receive-only raw LoRa TLV monitor");
  Serial.println("  V3 values are read-only reference; using V4 profile");
  Serial.println("════════════════════════════════════════════");

  configureDisplay();
  configureRadioOrHalt();
}

void loop() {
  if (millis() - lastHeartbeatMs >= 500) {
    lastHeartbeatMs = millis();
    digitalWrite(PIN_HEARTBEAT, !digitalRead(PIN_HEARTBEAT));
  }

  uint8_t buf[BP_MAX_PACKET_SIZE] = {0};
  const int state = radio.receive(buf, sizeof(buf), 3000);

  if (state == RADIOLIB_ERR_NONE) {
    handlePacket(buf, sizeof(buf));
  } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    Serial.print('.');
  } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
    errorCount++;
    Serial.println();
    Serial.println("[RX] CRC mismatch");
  } else {
    errorCount++;
    Serial.println();
    Serial.printf("[RX] receive failed: %d\n", state);
  }
}
