/*
  Bluepaws V4 — Home Hub Firmware
  Hardware: Seeed XIAO ESP32-S3 + SX1262 LoRa

  FreeRTOS tasks (ESP32 Arduino runs on FreeRTOS natively):
    - LoRa: RX packet handling, command TX queue
    - Web:  HTTP server, WebSocket, mDNS
    - BLE:  Home beacon advertising
    - Main: State coordination, logging, cloud forwarding
*/

#include <Arduino.h>
#include <bp_protocol.h>
#include <bp_config.h>
#include "hub_pins.h"

// ESP32 FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// ── Task Handles ──
static TaskHandle_t loraTaskHandle = NULL;
static TaskHandle_t webTaskHandle  = NULL;
static TaskHandle_t bleTaskHandle  = NULL;

// ── Task Stack Sizes ──
#define STACK_LORA  4096
#define STACK_WEB   8192
#define STACK_BLE   2048

// ── Task Priorities ──
#define PRIO_LORA  3
#define PRIO_WEB   2
#define PRIO_BLE   1

// ── Forward Declarations ──
static void loraTask(void *param);
static void webTask(void *param);
static void bleTask(void *param);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }

    Serial.println("=================================");
    Serial.println("  Bluepaws V4 — Home Hub");
    Serial.printf("  Protocol v%d | Packet max %d B\n",
                  BP_PROTOCOL_VERSION, BP_MAX_PACKET_SIZE);
    Serial.println("=================================");

    // TODO: Initialise LoRa radio (SX1262 via RadioLib)
    // TODO: Initialise WiFi AP + STA
    // TODO: Initialise LittleFS for web GUI
    // TODO: Initialise BLE beacon

    // Create FreeRTOS tasks (pinned to cores for ESP32)
    xTaskCreatePinnedToCore(loraTask, "lora", STACK_LORA, NULL, PRIO_LORA, &loraTaskHandle, 1);
    xTaskCreatePinnedToCore(webTask,  "web",  STACK_WEB,  NULL, PRIO_WEB,  &webTaskHandle,  0);
    xTaskCreatePinnedToCore(bleTask,  "ble",  STACK_BLE,  NULL, PRIO_BLE,  &bleTaskHandle,  0);

    Serial.println("[INIT] FreeRTOS tasks created");
}

void loop() {
    // FreeRTOS scheduler runs the tasks; loop() yields
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// ── Task Implementations (stubs) ──

static void loraTask(void *param) {
    (void)param;
    for (;;) {
        // TODO: Receive packets, validate CRC, dispatch by type
        // TODO: Process command queue with rate limiting
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void webTask(void *param) {
    (void)param;
    for (;;) {
        // TODO: Handle HTTP requests, WebSocket events
        // TODO: Serve Leaflet.js map interface
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void bleTask(void *param) {
    (void)param;
    for (;;) {
        // TODO: Advertise home beacon for collar detection
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
