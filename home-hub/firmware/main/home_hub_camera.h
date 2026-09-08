#pragma once

#include <cstddef>
#include <cstdint>

namespace bluepaws::camera {

constexpr uint32_t kPreviewWidth = 320;
// Show the square centre crop consumed by the QR decoder. The user-facing
// preview remains colour while the decoder maintains a separate grayscale copy.
constexpr uint32_t kPreviewHeight = 320;
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

// Software processing controls for QR framing and decoder tuning. The default
// colour preview is copied directly; non-zero brightness also gives immediate
// visual feedback and tunes the enhanced decoder attempts.
void setScanBrightness(int16_t offset);
int16_t scanBrightness();
void setScanContrast(uint16_t percent);
uint16_t scanContrast();
void setScanZoom(uint16_t percent);
uint16_t scanZoom();

// Copies the latest RGB565 preview into caller-owned memory so LVGL never
// renders from a V4L2 buffer that has already been returned to the camera.
bool copyPreview(uint16_t *destination, std::size_t pixel_capacity, uint32_t &generation);

}  // namespace bluepaws::camera
