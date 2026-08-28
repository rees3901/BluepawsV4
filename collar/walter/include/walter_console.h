#pragma once
#include <Arduino.h>
#include <cstdarg>

// Keep each diagnostic record together across the USB command and worker tasks.
// Arduino ESP32 2.0.17 HWCDC flushes its hardware FIFO on each write; rapid small
// writes lost characters on COM26. Pace short USB chunks under a record lock;
// avoid flush() racing the interrupt-driven FIFO writes in this pinned core.
class WalterConsole {
    SemaphoreHandle_t mutex_ = nullptr;
public:
    bool begin() { mutex_ = xSemaphoreCreateMutex(); return mutex_ != nullptr; }
    void printf(const char* format, ...) {
        char line[512];
        va_list args;
        va_start(args, format);
        const int size = vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (size <= 0 || size >= int(sizeof(line)) || !mutex_ ||
            xSemaphoreTake(mutex_, pdMS_TO_TICKS(1100)) != pdTRUE) return;
        for (int offset = 0; offset < size;) {
            const size_t chunk = size - offset > 32 ? 32 : size - offset;
            if (Serial.write(reinterpret_cast<const uint8_t*>(line + offset), chunk) != chunk) break;
            offset += chunk;
            delay(5); // Low-volume bench diagnostics, not a high-throughput serial link.
        }
        xSemaphoreGive(mutex_);
    }
    void println(const char* line = "") { printf("%s\n", line); }
};
