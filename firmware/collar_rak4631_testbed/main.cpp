/*
  Bluepaws V4 — RAK4631 collar test-bed

  Hardware:
    WisMesh Board ONE / RAK4631-style nRF52840 + SX1262

  Purpose:
    First real collar-side bring-up for the V4 architecture.
    This sketch builds canonical TLV v1.1 packets and transmits the raw binary
    packet over private LoRa. No JSON is sent on the radio path.

  Deliberately not included yet:
    BLE home beacon scanning, GNSS, LTE modem control, sleep scheduling,
    downlink command RX, production HMAC generation.
*/

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

#include <FreeRTOS.h>
#include <queue.h>
#include <task.h>

#include <bp_config.h>
#include <bp_protocol.h>

#ifndef MY_DEVICE_ID
#define MY_DEVICE_ID 1001
#endif

// RAK4630 / RAK4631 internal SX1262 wiring.
static constexpr uint8_t PIN_LORA_NSS  = 42;  // P1.10
static constexpr uint8_t PIN_LORA_SCK  = 43;  // P1.11
static constexpr uint8_t PIN_LORA_MOSI = 44;  // P1.12
static constexpr uint8_t PIN_LORA_MISO = 45;  // P1.13
static constexpr uint8_t PIN_LORA_BUSY = 46;  // P1.14
static constexpr uint8_t PIN_LORA_DIO1 = 47;  // P1.15
static constexpr uint8_t PIN_LORA_RST  = 38;  // P1.06

// Spoofed GPS centre supplied for the first collar test-bed.
static constexpr double HOME_LAT = 51.905978580906705;
static constexpr double HOME_LON = -2.239429400113001;

// Useful local conversion constants for the supplied latitude.
static constexpr double LAT_E7_PER_METRE = 10000000.0 / 111320.0;
static constexpr double LON_E7_PER_METRE = 10000000.0 / 68660.0;

static constexpr uint32_t PACKET_INTERVAL_MS = 15000;
static constexpr uint32_t FAKE_UNIX_START = 1787486400UL;
static constexpr uint16_t FAKE_BATTERY_MV = 3900;
static constexpr uint16_t FAKE_ACCURACY_M = 8;
static constexpr uint8_t FAKE_SATELLITES = 9;
static constexpr uint16_t FW_VERSION_V0_1 = 0x0001;

static constexpr uint16_t STACK_LORA = 4096;
static constexpr uint16_t STACK_TELEMETRY = 4096;
static constexpr UBaseType_t PRIO_LORA = 3;
static constexpr UBaseType_t PRIO_TELEMETRY = 2;

struct TxPacket {
  uint8_t len = 0;
  uint8_t bytes[BP_MAX_PACKET_SIZE] = {0};
  uint16_t seq = 0;
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
};

static QueueHandle_t txQueue = nullptr;

SPIClass loraSPI(NRF_SPIM2, PIN_LORA_MISO, PIN_LORA_SCK, PIN_LORA_MOSI);
SX1262 lora = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);

static void setLed(uint8_t pin, bool on) {
  digitalWrite(pin, on ? LED_STATE_ON : !LED_STATE_ON);
}

static void setBothLeds(bool on) {
  setLed(LED_GREEN, on);
  setLed(LED_BLUE, on);
}

static void blinkBoth(uint8_t count, uint16_t onMs, uint16_t offMs) {
  for (uint8_t i = 0; i < count; i++) {
    setBothLeds(true);
    delay(onMs);
    setBothLeds(false);
    delay(offMs);
  }
}

static void blinkLed(uint8_t pin, uint8_t count, uint16_t onMs, uint16_t offMs) {
  for (uint8_t i = 0; i < count; i++) {
    setLed(pin, true);
    vTaskDelay(pdMS_TO_TICKS(onMs));
    setLed(pin, false);
    vTaskDelay(pdMS_TO_TICKS(offMs));
  }
}

static int32_t degreesToE7(double degrees) {
  return static_cast<int32_t>(degrees * 10000000.0);
}

static void fakePosition(uint16_t seq, int32_t *latE7, int32_t *lonE7) {
  struct OffsetM {
    int16_t north;
    int16_t east;
  };

  static const OffsetM path[] = {
    {0, 0},       {45, 30},    {95, -35},   {155, 65},
    {210, 135},   {115, 220},  {-35, 170},  {-130, 75},
    {-170, -35},  {-65, -120}, {25, -185},  {120, -95},
  };

  const OffsetM &offset = path[seq % (sizeof(path) / sizeof(path[0]))];
  *latE7 = degreesToE7(HOME_LAT) + static_cast<int32_t>(offset.north * LAT_E7_PER_METRE);
  *lonE7 = degreesToE7(HOME_LON) + static_cast<int32_t>(offset.east * LON_E7_PER_METRE);
}

static void printHexLine(const uint8_t *bytes, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
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

static TxPacket buildTelemetryPacket(uint16_t seq) {
  TxPacket packet;
  packet.seq = seq;

  fakePosition(seq, &packet.latE7, &packet.lonE7);

  const uint32_t uptimeS = millis() / 1000UL;
  const uint32_t fakeUnix = FAKE_UNIX_START + uptimeS;
  const uint8_t flags = FLAG_GNSS_VALID | FLAG_FIX_3D;

  pkt_init(packet.bytes,
           static_cast<uint16_t>(MY_DEVICE_ID),
           seq,
           fakeUnix,
           STATUS_OUT_AND_ABOUT,
           PROFILE_NORMAL,
           flags,
           TX_TELEMETRY);
  pkt_set_gps(packet.bytes, packet.latE7, packet.lonE7);
  pkt_set_quality(packet.bytes, FAKE_BATTERY_MV, FAKE_ACCURACY_M, 0);
  pkt_set_sat_count(packet.bytes, FAKE_SATELLITES);

  // Small optional TLV set. Total TLV length = 10 bytes, so packet length = 50B.
  pkt_add_tlv_u16(packet.bytes, TLV_FW_VER, FW_VERSION_V0_1);
  pkt_add_tlv_u32(packet.bytes, TLV_UPTIME_S, uptimeS);

  packet.len = pkt_finalize(packet.bytes);
  return packet;
}

static void telemetryTask(void *param) {
  (void)param;
  uint16_t seq = 1;

  vTaskDelay(pdMS_TO_TICKS(1500));

  for (;;) {
    TxPacket packet = buildTelemetryPacket(seq++);

    Serial.printf(
      "[TLV] Built seq=%u len=%u device=%u lat=%.7f lon=%.7f status=%s profile=%s reason=%s\n",
      packet.seq,
      packet.len,
      static_cast<unsigned>(MY_DEVICE_ID),
      packet.latE7 / 10000000.0,
      packet.lonE7 / 10000000.0,
      bp_status_display(static_cast<bp_status_t>(pkt_status(packet.bytes))),
      bp_profile_name(static_cast<bp_profile_t>(pkt_power_profile(packet.bytes))),
      bp_tx_reason_display(pkt_tx_reason(packet.bytes)));
    Serial.print("[TLV] Hex: ");
    printHexLine(packet.bytes, packet.len);

    if (xQueueSend(txQueue, &packet, pdMS_TO_TICKS(1000)) != pdTRUE) {
      Serial.println("[TLV] TX queue full; packet dropped");
      blinkLed(LED_BLUE, 6, 35, 35);
    } else {
      blinkLed(LED_BLUE, 1, 25, 25);
    }

    vTaskDelay(pdMS_TO_TICKS(PACKET_INTERVAL_MS));
  }
}

static void configureRadioOrHalt() {
  loraSPI.begin();

  Serial.println("[LORA] Initialising SX1262 for Bluepaws V4 locked profile...");
  int state = lora.begin(LORA_FREQUENCY);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] FATAL init failed: %d\n", state);
    while (true) {
      blinkBoth(6, 60, 60);
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  lora.setSpreadingFactor(LORA_SPREADING);
  lora.setBandwidth(LORA_BANDWIDTH);
  lora.setCodingRate(LORA_CODING_RATE);
  lora.setPreambleLength(LORA_PREAMBLE_LEN);
  lora.setSyncWord(LORA_SYNC_WORD);
  lora.setCRC(LORA_CRC_ENABLED);
  lora.setOutputPower(bp_profile_config(PROFILE_NORMAL)->tx_power_dBm);

  Serial.printf("[LORA] Ready %.1f MHz | SF%d | BW %.0f kHz | CR 4/%d | preamble %d | sync 0x%02X | CRC %s | %d dBm\n",
                LORA_FREQUENCY,
                LORA_SPREADING,
                LORA_BANDWIDTH,
                LORA_CODING_RATE,
                LORA_PREAMBLE_LEN,
                LORA_SYNC_WORD,
                LORA_CRC_ENABLED ? "on" : "off",
                bp_profile_config(PROFILE_NORMAL)->tx_power_dBm);
  Serial.println("[LORA] V3 receiver/sniffer values are read-only reference; this TX uses V4 locked parameters.");
}

static void loraTask(void *param) {
  (void)param;
  configureRadioOrHalt();

  TxPacket packet;
  for (;;) {
    if (xQueueReceive(txQueue, &packet, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    Serial.printf("[LORA] TX start seq=%u len=%u\n", packet.seq, packet.len);
    const int state = lora.transmit(packet.bytes, packet.len);
    if (state == RADIOLIB_ERR_NONE) {
      Serial.printf("[LORA] TX OK seq=%u len=%u\n", packet.seq, packet.len);
      blinkLed(LED_GREEN, 2, 60, 60);
    } else {
      Serial.printf("[LORA] TX failed seq=%u code=%d\n", packet.seq, state);
      blinkBoth(6, 45, 45);
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {
    delay(10);
  }

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  setBothLeds(false);
  blinkBoth(3, 80, 120);

  Serial.println();
  Serial.println("════════════════════════════════════════════");
  Serial.println("  Bluepaws V4 — RAK4631 collar test-bed");
  Serial.printf("  Device ID: %u\n", static_cast<unsigned>(MY_DEVICE_ID));
  Serial.printf("  Protocol: TLV v%d | fixed header %dB | max packet %dB\n",
                BP_PROTOCOL_VERSION,
                BP_HEADER_SIZE,
                BP_MAX_PACKET_SIZE);
  Serial.printf("  Fake GPS centre: %.7f, %.7f\n", HOME_LAT, HOME_LON);
  Serial.println("  Radio path: raw TLV over private LoRa, no JSON");
  Serial.println("  Auth tag: placeholder zero tag until embedded HMAC is wired");
  Serial.println("════════════════════════════════════════════");

  txQueue = xQueueCreate(4, sizeof(TxPacket));
  if (txQueue == nullptr) {
    Serial.println("[INIT] FATAL: could not create TX queue");
    while (true) {
      blinkBoth(10, 40, 40);
      delay(1000);
    }
  }

  xTaskCreate(loraTask, "lora", STACK_LORA, nullptr, PRIO_LORA, nullptr);
  xTaskCreate(telemetryTask, "telemetry", STACK_TELEMETRY, nullptr, PRIO_TELEMETRY, nullptr);

  Serial.println("[INIT] FreeRTOS tasks started.");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(10000));
}
