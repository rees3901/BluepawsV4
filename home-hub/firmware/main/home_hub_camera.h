#pragma once

#include <cstddef>
#include <cstdint>

namespace bluepaws::camera {

constexpr uint32_t kPreviewWidth = 320;
// Match the OV02C10's 1920x1080 aspect ratio so QR modules stay square.
constexpr uint32_t kPreviewHeight = 180;
constexpr std::size_t kPreviewPixelCount = kPreviewWidth * kPreviewHeight;

enum class State : uint8_t {
    Stopped,
    Starting,
    Streaming,
    Failed,
};

struct Status {
    State state = State::Stopped;
    uint32_t captured_frames = 0;
    uint32_t preview_generation = 0;
    uint32_t result_generation = 0;
    char message[96]{};
    char payload[512]{};
};

// Starts the OV02C10 MIPI-CSI worker. Initialization occurs off the LVGL task.
bool start();
void stop();
Status status();

// Adjusts the grayscale image used by the QR decoder. -1 is darker, 0 cycles
// automatically through useful exposure offsets, and +1 is brighter.
void setScanBrightness(int8_t level);
int8_t scanBrightness();

// Copies the latest RGB565 preview into caller-owned memory so LVGL never
// renders from a V4L2 buffer that has already been returned to the camera.
bool copyPreview(uint16_t *destination, std::size_t pixel_capacity, uint32_t &generation);

}  // namespace bluepaws::camera
