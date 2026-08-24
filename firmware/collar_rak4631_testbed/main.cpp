/*
  Bluepaws V4 — RAK4631 collar runtime test-bed

  Hardware:
    WisMesh Board ONE / RAK4631-style nRF52840 + SX1262

  Purpose:
    Exercise the V4 collar runtime decisions before production hardware exists:
      - profile-driven wake cadence
      - simulated BLE Home seen/missed logic
      - spoofed GNSS with gentle drift
      - TLV v1.1 packet generation
      - raw binary TLV LoRa transmission
      - LTE heartbeat/fallback scheduling as serial-visible placeholders

  Deliberately not included yet:
    Real BLE scanning, real GNSS, LTE modem UART control, production deep sleep,
    downlink command parsing, production HMAC generation.
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

static constexpr uint8_t PIN_LORA_NSS  = 42;
static constexpr uint8_t PIN_LORA_SCK  = 43;
static constexpr uint8_t PIN_LORA_MOSI = 44;
static constexpr uint8_t PIN_LORA_MISO = 45;
static constexpr uint8_t PIN_LORA_BUSY = 46;
static constexpr uint8_t PIN_LORA_DIO1 = 47;
static constexpr uint8_t PIN_LORA_RST  = 38;

static constexpr double HOME_LAT = 51.905978580906705;
static constexpr double HOME_LON = -2.239429400113001;
static constexpr double LAT_E7_PER_METRE = 10000000.0 / 111320.0;
static constexpr double LON_E7_PER_METRE = 10000000.0 / 68660.0;

// Bench runtime is time-compressed. Each pass represents one real collar wake.
static constexpr uint32_t TEST_WAKE_STEP_MS = 8000;
static constexpr uint32_t TEST_WAKE_STEP_SECONDS = TEST_WAKE_STEP_MS / 1000UL;
static constexpr uint32_t TEST_RX_WINDOW_MS = 1500;
static constexpr uint32_t FAKE_UNIX_START = 1787486400UL;
static constexpr uint16_t FAKE_BATTERY_MV = 3900;
static constexpr uint16_t FAKE_ACCURACY_M = 8;
static constexpr uint8_t FAKE_SATELLITES = 9;
static constexpr uint16_t FW_VERSION_V0_1 = 0x0001;
static constexpr uint8_t TEST_DEVICE_HMAC_KEY[32] = {
  0xE1, 0x9F, 0x7E, 0x88, 0x71, 0x5A, 0x5D, 0x9D,
  0x37, 0xE6, 0xF8, 0x86, 0x74, 0x90, 0xBB, 0x8A,
  0x59, 0xCF, 0xE6, 0xE5, 0xD0, 0xC0, 0x7F, 0x90,
  0x3E, 0x29, 0x02, 0x83, 0x99, 0x56, 0xC0, 0x52,
};

static constexpr uint16_t STACK_LORA = 4096;
static constexpr uint16_t STACK_RUNTIME = 6144;
static constexpr uint16_t STACK_SERIAL = 4096;
static constexpr UBaseType_t PRIO_LORA = 3;
static constexpr UBaseType_t PRIO_RUNTIME = 2;
static constexpr UBaseType_t PRIO_SERIAL = 1;

struct RuntimeProfile {
  bp_profile_t profile;
  const char *label;
  uint16_t wakeIntervalS;
  uint8_t homeLoraEveryNWakes;
  uint8_t homeGnssEveryNWakes;
  uint32_t lteHeartbeatEveryS;
};

static const RuntimeProfile RUNTIME_PROFILES[] = {
  {PROFILE_POWERSAVE, "Power Save", 1800, 2, 10, 3UL * 3600UL},
  {PROFILE_NORMAL, "Normal", 600, 1, 10, 3600UL},
  {PROFILE_ACTIVE, "Active", 60, 1, 10, 600UL},
  {PROFILE_LOST, "Lost Alert", 30, 1, 1, 120UL},
};

struct TxPacket {
  uint8_t len = 0;
  uint8_t bytes[BP_MAX_PACKET_SIZE] = {0};
  uint16_t seq = 0;
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  bp_status_t status = STATUS_HOME;
  bp_profile_t profile = PROFILE_NORMAL;
  bp_tx_reason_t reason = TX_TELEMETRY;
};

struct CollarRuntimeState {
  bp_profile_t profile = PROFILE_NORMAL;
  bool simulatedHomeBeaconSeen = true;
  bool lastGnssValid = true;
  uint16_t seq = 1;
  uint32_t simulatedUnix = FAKE_UNIX_START;
  uint32_t simulatedProfileSeconds = 0;
  uint32_t lastLteHeartbeatProfileSeconds = 0;
  uint32_t lastSuccessfulCloudSeenUnix = FAKE_UNIX_START;
  uint32_t homeSeenWakeCount = 0;
  uint32_t homeMissedWakeCount = 0;
  uint32_t totalWakeCount = 0;
  int32_t lastLatE7 = 0;
  int32_t lastLonE7 = 0;
};

static QueueHandle_t txQueue = nullptr;
static CollarRuntimeState runtimeState;

SPIClass loraSPI(NRF_SPIM2, PIN_LORA_MISO, PIN_LORA_SCK, PIN_LORA_MOSI);
SX1262 lora = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSPI);

static const RuntimeProfile *runtimeProfile(bp_profile_t profile) {
  for (const RuntimeProfile &candidate : RUNTIME_PROFILES) {
    if (candidate.profile == profile) {
      return &candidate;
    }
  }
  return &RUNTIME_PROFILES[1];
}

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
    {0, 0},       {35, 22},    {72, -18},   {115, 45},
    {168, 96},    {90, 160},   {-25, 125},  {-92, 58},
    {-140, -20},  {-52, -96},  {18, -145},  {95, -68},
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

static TxPacket buildPacket(bp_status_t status,
                            bp_profile_t profile,
                            bp_tx_reason_t reason,
                            bool gnssValid,
                            bool homeBeaconSeen,
                            bool staleFix,
                            bool errorPresent) {
  TxPacket packet;
  packet.seq = runtimeState.seq++;
  packet.status = status;
  packet.profile = profile;
  packet.reason = reason;

  if (gnssValid) {
    fakePosition(packet.seq, &runtimeState.lastLatE7, &runtimeState.lastLonE7);
    runtimeState.lastGnssValid = true;
  }

  packet.latE7 = runtimeState.lastLatE7;
  packet.lonE7 = runtimeState.lastLonE7;

  uint8_t flags = 0;
  if (gnssValid) {
    flags |= FLAG_GNSS_VALID | FLAG_FIX_3D;
  }
  if (homeBeaconSeen) {
    flags |= FLAG_HOME_BEACON_SEEN;
  }
  if (staleFix) {
    flags |= FLAG_STALE_FIX;
  }
  if (errorPresent) {
    flags |= FLAG_ERROR_PRESENT;
  }

  const uint16_t fixAgeS = gnssValid ? 0 : 65535;

  pkt_init(packet.bytes,
           static_cast<uint16_t>(MY_DEVICE_ID),
           packet.seq,
           runtimeState.simulatedUnix,
           status,
           profile,
           flags,
           reason);
  pkt_set_gps(packet.bytes, packet.latE7, packet.lonE7);
  pkt_set_quality(packet.bytes, FAKE_BATTERY_MV, FAKE_ACCURACY_M, fixAgeS);
  pkt_set_sat_count(packet.bytes, gnssValid ? FAKE_SATELLITES : 255);

  pkt_add_tlv_u16(packet.bytes, TLV_FW_VER, FW_VERSION_V0_1);
  pkt_add_tlv_u32(packet.bytes, TLV_UPTIME_S, millis() / 1000UL);

  packet.len = pkt_finalize_hmac_sha256_64(packet.bytes,
                                           TEST_DEVICE_HMAC_KEY,
                                           sizeof(TEST_DEVICE_HMAC_KEY));
  return packet;
}

static bool enqueuePacket(const TxPacket &packet, const char *label) {
  Serial.printf(
    "[TLV] %s seq=%u len=%u device=%u status=%s profile=%s reason=%s flags=0x%02X lat=%.7f lon=%.7f\n",
    label,
    packet.seq,
    packet.len,
    static_cast<unsigned>(MY_DEVICE_ID),
    bp_status_display(static_cast<bp_status_t>(pkt_status(packet.bytes))),
    bp_profile_name(static_cast<bp_profile_t>(pkt_power_profile(packet.bytes))),
    bp_tx_reason_display(pkt_tx_reason(packet.bytes)),
    pkt_flags(packet.bytes),
    packet.latE7 / 10000000.0,
    packet.lonE7 / 10000000.0);
  Serial.print("[TLV] Hex: ");
  printHexLine(packet.bytes, packet.len);

  if (xQueueSend(txQueue, &packet, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("[TLV] TX queue full; packet dropped");
    blinkLed(LED_BLUE, 6, 35, 35);
    return false;
  }

  blinkLed(LED_BLUE, 1, 25, 25);
  return true;
}

static void noteRxWindow(const char *path) {
  Serial.printf("[RX] %s: opening simulated %lu ms command/ACK window (production target: %u ms)\n",
                path,
                static_cast<unsigned long>(TEST_RX_WINDOW_MS),
                CMD_LISTEN_WINDOW_MS);
  vTaskDelay(pdMS_TO_TICKS(TEST_RX_WINDOW_MS));
}

static void noteLteEvent(const char *label, bp_status_t status, bp_tx_reason_t reason) {
  runtimeState.lastLteHeartbeatProfileSeconds = runtimeState.simulatedProfileSeconds;
  runtimeState.lastSuccessfulCloudSeenUnix = runtimeState.simulatedUnix;
  Serial.printf("[LTE] %s due now: would HTTPS-wrap same TLV shape | status=%s reason=%s unix=%lu\n",
                label,
                bp_status_display(status),
                bp_tx_reason_display(reason),
                static_cast<unsigned long>(runtimeState.simulatedUnix));
}

static bool homeGnssShouldFailForIndoorDemo() {
  return (runtimeState.homeSeenWakeCount % 40UL) == 0UL;
}

static void runHomePath(const RuntimeProfile *profile) {
  runtimeState.homeSeenWakeCount++;
  runtimeState.homeMissedWakeCount = 0;

  const bool gnssDue = (profile->homeGnssEveryNWakes > 0) &&
                       ((runtimeState.homeSeenWakeCount % profile->homeGnssEveryNWakes) == 0);
  const bool loraWakeDue = (profile->homeLoraEveryNWakes > 0) &&
                           ((runtimeState.homeSeenWakeCount % profile->homeLoraEveryNWakes) == 0);
  const bool lteDue = (profile->lteHeartbeatEveryS > 0) &&
                      ((runtimeState.simulatedProfileSeconds - runtimeState.lastLteHeartbeatProfileSeconds) >= profile->lteHeartbeatEveryS);

  Serial.printf("[HOME] BLE seen count=%lu | LoRa due=%s | GNSS sanity due=%s | LTE due=%s\n",
                static_cast<unsigned long>(runtimeState.homeSeenWakeCount),
                loraWakeDue ? "yes" : "no",
                gnssDue ? "yes" : "no",
                lteDue ? "yes" : "no");

  if (gnssDue) {
    const bool gnssOk = !homeGnssShouldFailForIndoorDemo();
    const TxPacket packet = buildPacket(STATUS_HOME,
                                        profile->profile,
                                        TX_TELEMETRY,
                                        gnssOk,
                                        true,
                                        !gnssOk,
                                        !gnssOk);
    enqueuePacket(packet, gnssOk ? "Home GNSS sanity telemetry" : "Home GNSS failed; stale HOME telemetry");
    noteRxWindow("home GNSS refresh");
  } else if (loraWakeDue) {
    const TxPacket checkin = buildPacket(STATUS_HOME,
                                         profile->profile,
                                         TX_WAKE_CHECKIN,
                                         false,
                                         true,
                                         false,
                                         false);
    enqueuePacket(checkin, "Home wake check-in");
    noteRxWindow("home wake check-in");
  } else {
    Serial.println("[HOME] No LoRa packet due this wake; returning to profile sleep.");
  }

  if (lteDue) {
    noteLteEvent("Home LTE heartbeat", STATUS_HOME, gnssDue ? TX_TELEMETRY : TX_WAKE_CHECKIN);
  }
}

static void runAwayPath(const RuntimeProfile *profile) {
  runtimeState.homeMissedWakeCount++;
  runtimeState.homeSeenWakeCount = 0;

  Serial.printf("[AWAY] BLE Home missed count=%lu\n",
                static_cast<unsigned long>(runtimeState.homeMissedWakeCount));

  if (runtimeState.homeMissedWakeCount < 2) {
    Serial.println("[AWAY] First miss only; not declaring away yet.");
    return;
  }

  const TxPacket awake = buildPacket(STATUS_OUT_AND_ABOUT,
                                     profile->profile,
                                     TX_PING,
                                     false,
                                     false,
                                     runtimeState.lastGnssValid,
                                     false);
  enqueuePacket(awake, "Away awake/looking check-in");
  noteRxWindow("away awake/looking");

  const bool gnssOk = (runtimeState.totalWakeCount % 6UL) != 0UL;
  const TxPacket packet = buildPacket(STATUS_OUT_AND_ABOUT,
                                      profile->profile,
                                      TX_TELEMETRY,
                                      gnssOk,
                                      false,
                                      !gnssOk,
                                      !gnssOk);
  enqueuePacket(packet, gnssOk ? "Away GNSS telemetry" : "Away GNSS failed; stale telemetry");
  noteRxWindow("away telemetry");

  if (profile->profile == PROFILE_ACTIVE || profile->profile == PROFILE_LOST) {
    noteLteEvent("Away LTE profile send", STATUS_OUT_AND_ABOUT, TX_TELEMETRY);
  } else {
    Serial.println("[LTE] Away LTE fallback not due in this compressed demo cycle.");
  }
}

static void runLostAlertPath(const RuntimeProfile *profile) {
  runtimeState.homeMissedWakeCount = 0;
  Serial.println("[LOST] Lost Alert active: BLE Home gate ignored; advertising BP_FIND beacon placeholder.");

  const TxPacket telemetry = buildPacket(STATUS_LOST,
                                         profile->profile,
                                         TX_ALERT,
                                         true,
                                         false,
                                         false,
                                         false);
  enqueuePacket(telemetry, "Lost Alert telemetry");
  noteLteEvent("Lost Alert LTE emergency send", STATUS_LOST, TX_ALERT);
  noteRxWindow("lost alert");
}

static void runtimeTask(void *param) {
  (void)param;
  fakePosition(0, &runtimeState.lastLatE7, &runtimeState.lastLonE7);

  vTaskDelay(pdMS_TO_TICKS(1500));

  for (;;) {
    const RuntimeProfile *profile = runtimeProfile(runtimeState.profile);
    runtimeState.totalWakeCount++;
    runtimeState.simulatedUnix += TEST_WAKE_STEP_SECONDS;
    runtimeState.simulatedProfileSeconds += profile->wakeIntervalS;

    Serial.println();
    Serial.println("────────────────────────────────────────────");
    Serial.printf("[WAKE] #%lu profile=%s real_interval=%us simulated_unix=%lu logical_profile_time=%lus BLE_HOME=%s\n",
                  static_cast<unsigned long>(runtimeState.totalWakeCount),
                  profile->label,
                  profile->wakeIntervalS,
                  static_cast<unsigned long>(runtimeState.simulatedUnix),
                  static_cast<unsigned long>(runtimeState.simulatedProfileSeconds),
                  runtimeState.simulatedHomeBeaconSeen ? "seen" : "missed");

    if (profile->profile == PROFILE_LOST) {
      runLostAlertPath(profile);
    } else if (runtimeState.simulatedHomeBeaconSeen) {
      runHomePath(profile);
    } else {
      runAwayPath(profile);
    }

    Serial.printf("[SLEEP] compressed bench delay %lu ms before next profile wake\n",
                  static_cast<unsigned long>(TEST_WAKE_STEP_MS));
    vTaskDelay(pdMS_TO_TICKS(TEST_WAKE_STEP_MS));
  }
}

static void printHelp() {
  Serial.println();
  Serial.println("[CMD] Serial commands:");
  Serial.println("      n = Normal profile");
  Serial.println("      p = Power Save profile");
  Serial.println("      a = Active profile");
  Serial.println("      l = Lost Alert profile");
  Serial.println("      h = simulated BLE Home beacon seen");
  Serial.println("      o = simulated BLE Home beacon missed / away");
  Serial.println("      t = toggle simulated Home/Away");
  Serial.println("      ? = print this help");
}

static void serialTask(void *param) {
  (void)param;
  printHelp();

  for (;;) {
    while (Serial.available() > 0) {
      const char ch = static_cast<char>(Serial.read());
      switch (ch) {
        case 'n':
        case 'N':
          runtimeState.profile = PROFILE_NORMAL;
          Serial.println("[CMD] Profile set to NORMAL");
          break;
        case 'p':
        case 'P':
          runtimeState.profile = PROFILE_POWERSAVE;
          Serial.println("[CMD] Profile set to POWER_SAVE");
          break;
        case 'a':
        case 'A':
          runtimeState.profile = PROFILE_ACTIVE;
          Serial.println("[CMD] Profile set to ACTIVE");
          break;
        case 'l':
        case 'L':
          runtimeState.profile = PROFILE_LOST;
          Serial.println("[CMD] Profile set to LOST_ALERT");
          break;
        case 'h':
        case 'H':
          runtimeState.simulatedHomeBeaconSeen = true;
          Serial.println("[CMD] Simulated BLE Home set to SEEN");
          break;
        case 'o':
        case 'O':
          runtimeState.simulatedHomeBeaconSeen = false;
          Serial.println("[CMD] Simulated BLE Home set to MISSED/AWAY");
          break;
        case 't':
        case 'T':
          runtimeState.simulatedHomeBeaconSeen = !runtimeState.simulatedHomeBeaconSeen;
          Serial.printf("[CMD] Simulated BLE Home toggled to %s\n",
                        runtimeState.simulatedHomeBeaconSeen ? "SEEN" : "MISSED/AWAY");
          break;
        case '?':
          printHelp();
          break;
        case '\r':
        case '\n':
        case ' ':
          break;
        default:
          Serial.printf("[CMD] Unknown command '%c'; send ? for help\n", ch);
          break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
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

  Serial.printf("[LORA] Ready %.1f MHz | SF%d | BW %.0f kHz | CR 4/%d | preamble %d | sync 0x%02X | CRC %s | default %d dBm\n",
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

    const int8_t power = bp_profile_config(packet.profile)->tx_power_dBm;
    lora.setOutputPower(power);

    Serial.printf("[LORA] TX start seq=%u len=%u profile=%s tx_power=%d dBm\n",
                  packet.seq,
                  packet.len,
                  bp_profile_name(packet.profile),
                  power);
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
  Serial.println("  Bluepaws V4 — RAK4631 collar runtime test-bed");
  Serial.printf("  Device ID: %u\n", static_cast<unsigned>(MY_DEVICE_ID));
  Serial.printf("  Protocol: TLV v%d | fixed header %dB | max packet %dB\n",
                BP_PROTOCOL_VERSION,
                BP_HEADER_SIZE,
                BP_MAX_PACKET_SIZE);
  Serial.printf("  Fake GPS centre: %.7f, %.7f\n", HOME_LAT, HOME_LON);
  Serial.println("  Radio path: raw TLV over private LoRa, no JSON");
  Serial.println("  Auth tag: HMAC-SHA256-64 using device 1001 bench key");
  Serial.println("  Runtime: FreeRTOS LoRa + profile scheduler + serial command tasks");
  Serial.println("════════════════════════════════════════════");

  txQueue = xQueueCreate(8, sizeof(TxPacket));
  if (txQueue == nullptr) {
    Serial.println("[INIT] FATAL: could not create TX queue");
    while (true) {
      blinkBoth(10, 40, 40);
      delay(1000);
    }
  }

  xTaskCreate(loraTask, "lora", STACK_LORA, nullptr, PRIO_LORA, nullptr);
  xTaskCreate(runtimeTask, "runtime", STACK_RUNTIME, nullptr, PRIO_RUNTIME, nullptr);
  xTaskCreate(serialTask, "serial", STACK_SERIAL, nullptr, PRIO_SERIAL, nullptr);

  Serial.println("[INIT] FreeRTOS tasks started.");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(10000));
}
