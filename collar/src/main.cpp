/*
  Bluepaws V4 — Collar Firmware
  Hardware: Seeed XIAO nRF52840 + SX1262 + L76K GNSS + BG77 NB-IoT

  FreeRTOS tasks:
    - LoRa:     TX telemetry, RX command window
    - GPS:      Acquisition, fix monitoring
    - Cellular: Periodic NB-IoT REST POST
    - BLE:      Home beacon scanning
    - Main:     State machine, sleep scheduling
*/

#include <Arduino.h>
#include <bp_protocol.h>
#include <bp_config.h>
#include "collar_pins.h"

// FreeRTOS is built into the nRF52 Arduino BSP
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>

// ── Task Handles ──
static TaskHandle_t loraTaskHandle   = NULL;
static TaskHandle_t gpsTaskHandle    = NULL;
static TaskHandle_t cellTaskHandle   = NULL;
static TaskHandle_t bleTaskHandle    = NULL;

// ── Task Stack Sizes ──
#define STACK_LORA  2048
#define STACK_GPS   2048
#define STACK_CELL  2048
#define STACK_BLE   1536

// ── Task Priorities (higher = more urgent) ──
#define PRIO_LORA   3
#define PRIO_GPS    2
#define PRIO_CELL   1
#define PRIO_BLE    2

// ── Forward Declarations ──
static void loraTask(void *param);
static void gpsTask(void *param);
static void cellularTask(void *param);
static void bleTask(void *param);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }

    Serial.println("=================================");
    Serial.println("  Bluepaws V4 — Collar");
    Serial.printf("  Protocol v%d | Packet max %d B\n",
                  BP_PROTOCOL_VERSION, BP_MAX_PACKET_SIZE);
    Serial.println("=================================");

    // TODO: Initialise LoRa radio (SX1262 via RadioLib)
    // TODO: Initialise GPS (TinyGPS++ on UART)
    // TODO: Initialise NB-IoT modem (AT commands on UART)
    // TODO: Initialise BLE for beacon scanning

    // Create FreeRTOS tasks
    xTaskCreate(loraTask,     "lora", STACK_LORA, NULL, PRIO_LORA, &loraTaskHandle);
    xTaskCreate(gpsTask,      "gps",  STACK_GPS,  NULL, PRIO_GPS,  &gpsTaskHandle);
    xTaskCreate(cellularTask, "cell", STACK_CELL, NULL, PRIO_CELL, &cellTaskHandle);
    xTaskCreate(bleTask,      "ble",  STACK_BLE,  NULL, PRIO_BLE,  &bleTaskHandle);

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
        // TODO: Build telemetry packet, transmit, listen for commands
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void gpsTask(void *param) {
    (void)param;
    for (;;) {
        // TODO: Feed TinyGPS++, manage cold/warm start, report fix
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void cellularTask(void *param) {
    (void)param;
    for (;;) {
        // TODO: Every N LoRa cycles, POST TLV payload via NB-IoT
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void bleTask(void *param) {
    (void)param;
    for (;;) {
        // TODO: Scan for home beacon, update home cycle count
        vTaskDelay(pdMS_TO_TICKS(BLE_SCAN_DURATION_S * 1000));
    }
}
