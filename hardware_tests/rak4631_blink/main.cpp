#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

namespace {
constexpr uint32_t kFlashMs = 160;
constexpr uint32_t kBetweenFlashMs = 160;
constexpr uint32_t kLoopPauseMs = 1000;

void setLed(uint8_t pin, bool on) {
  digitalWrite(pin, on ? LED_STATE_ON : !LED_STATE_ON);
}

void setBoth(bool on) {
  setLed(LED_GREEN, on);
  setLed(LED_BLUE, on);
}

void flashTriple() {
  for (uint8_t i = 0; i < 3; ++i) {
    setBoth(true);
    delay(kFlashMs);
    setBoth(false);
    delay(kBetweenFlashMs);
  }
}
}  // namespace

void setup() {
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  setBoth(false);

  Serial.begin(115200);
  delay(500);
  Serial.println("Bluepaws RAK4631 blink confidence test");
  Serial.println("Pattern: blink x3, pause 1s, repeat");
}

void loop() {
  flashTriple();
  delay(kLoopPauseMs);
}
