#include "home_hub_camera.h"

#include "guition_jc4880p443c.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "quirc.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace bluepaws::camera {
namespace {

constexpr char kTag[] = "home_hub_camera";
constexpr unsigned kBufferCount = 2;
constexpr uint32_t kDecodeEveryFrames = 5;

struct CaptureBuffer {
    void *data = nullptr;
    std::size_t length = 0;
};

void *const kMapFailed = reinterpret_cast<void *>(-1);

SemaphoreHandle_t g_lock = nullptr;
TaskHandle_t g_task = nullptr;
Status g_status{};
uint16_t *g_preview = nullptr;
volatile bool g_stop_requested = false;
bool g_video_initialized = false;

void set_state(State state, const char *message)
{
    if (g_lock == nullptr || xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    g_status.state = state;
    std::snprintf(g_status.message, sizeof(g_status.message), "%s", message);
    xSemaphoreGive(g_lock);
}

uint8_t rgb565_gray(uint16_t pixel)
{
    // OV02C10/ISP buffers follow the vendor demo's big-endian RGB565 layout.
    const uint16_t value = __builtin_bswap16(pixel);
    const uint32_t red = (value >> 11U) & 0x1FU;
    const uint32_t green = (value >> 5U) & 0x3FU;
    const uint32_t blue = value & 0x1FU;
    return static_cast<uint8_t>(std::min<uint32_t>(255, (red * 8U + green * 4U + blue * 8U) / 3U));
}

void publish_frame(const uint16_t *source, uint32_t width, uint32_t height, uint32_t stride_bytes,
                   quirc *decoder)
{
    if (source == nullptr || width == 0 || height == 0 || g_preview == nullptr) return;
    const uint32_t stride_pixels = stride_bytes >= width * 2U ? stride_bytes / 2U : width;
    uint32_t captured_frames = 0;
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    for (uint32_t y = 0; y < kPreviewHeight; ++y) {
        const uint32_t source_y = y * height / kPreviewHeight;
        for (uint32_t x = 0; x < kPreviewWidth; ++x) {
            const uint32_t source_x = x * width / kPreviewWidth;
            const uint16_t pixel = source[source_y * stride_pixels + source_x];
            g_preview[y * kPreviewWidth + x] = pixel;
        }
    }
    ++g_status.preview_generation;
    captured_frames = ++g_status.captured_frames;
    xSemaphoreGive(g_lock);

    if (captured_frames % kDecodeEveryFrames != 0) return;
    uint8_t *gray = quirc_begin(decoder, nullptr, nullptr);
    for (uint32_t y = 0; y < kPreviewHeight; ++y) {
        const uint32_t source_y = y * height / kPreviewHeight;
        for (uint32_t x = 0; x < kPreviewWidth; ++x) {
            const uint32_t source_x = x * width / kPreviewWidth;
            gray[y * kPreviewWidth + x] =
                rgb565_gray(source[source_y * stride_pixels + source_x]);
        }
    }
    quirc_end(decoder);
    const int count = quirc_count(decoder);
    for (int index = 0; index < count; ++index) {
        quirc_code code{};
        quirc_data data{};
        quirc_extract(decoder, index, &code);
        quirc_decode_error_t result = quirc_decode(&code, &data);
        if (result != QUIRC_SUCCESS) {
            quirc_flip(&code);
            result = quirc_decode(&code, &data);
        }
        if (result != QUIRC_SUCCESS || data.payload_len == 0) continue;
        if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            const std::size_t length = std::min<std::size_t>(
                data.payload_len, sizeof(g_status.payload) - 1);
            const bool changed = std::strlen(g_status.payload) != length ||
                std::memcmp(g_status.payload, data.payload, length) != 0;
            if (changed) {
                std::memcpy(g_status.payload, data.payload, length);
                g_status.payload[length] = '\0';
                ++g_status.result_generation;
            }
            std::snprintf(g_status.message, sizeof(g_status.message), "QR code detected");
            xSemaphoreGive(g_lock);
        }
        ESP_LOGI(kTag, "QR payload detected (%u bytes)", data.payload_len);
        break;
    }
}

void camera_task(void *)
{
    set_state(State::Starting, "Starting OV02C10 camera...");
    esp_err_t result = ESP_OK;
    if (!g_video_initialized) {
        esp_video_init_csi_config_t csi[] = {{
            .sccb_config = {
                .init_sccb = true,
                .i2c_config = {
                    .port = 1,
                    .scl_pin = GUITION_JC4880P443C_TOUCH_SCL_GPIO,
                    .sda_pin = GUITION_JC4880P443C_TOUCH_SDA_GPIO,
                },
                .freq = 100000,
            },
            .reset_pin = -1,
            .pwdn_pin = -1,
        }};
        csi[0].sccb_config.init_sccb = false;
        csi[0].sccb_config.i2c_handle = guition_jc4880p443c_i2c_bus();
        esp_video_init_config_t config{};
        config.csi = csi;
        result = esp_video_init(&config);
        g_video_initialized = result == ESP_OK;
    }
    if (result != ESP_OK) {
        set_state(State::Failed, "OV02C10 initialization failed");
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (fd < 0) {
        set_state(State::Failed, "Could not open MIPI-CSI video device");
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
        close(fd);
        set_state(State::Failed, "Could not read camera format");
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(fd, VIDIOC_S_FMT, &format) != 0 || ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
            close(fd);
            set_state(State::Failed, "Camera RGB565 format is unavailable");
            g_task = nullptr;
            vTaskDelete(nullptr);
            return;
        }
    }

    v4l2_requestbuffers request{};
    request.count = kBufferCount;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    CaptureBuffer buffers[kBufferCount]{};
    bool buffers_ready = ioctl(fd, VIDIOC_REQBUFS, &request) == 0 && request.count >= kBufferCount;
    for (unsigned i = 0; buffers_ready && i < kBufferCount; ++i) {
        v4l2_buffer buffer{};
        buffer.type = request.type;
        buffer.memory = request.memory;
        buffer.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            buffers_ready = false;
            break;
        }
        buffers[i].length = buffer.length;
        buffers[i].data = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                               buffer.m.offset);
        if (buffers[i].data == kMapFailed || ioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
            buffers_ready = false;
        }
    }

    quirc *decoder = quirc_new();
    if (!buffers_ready || decoder == nullptr ||
        quirc_resize(decoder, kPreviewWidth, kPreviewHeight) < 0) {
        if (decoder != nullptr) quirc_destroy(decoder);
        for (auto &buffer : buffers) {
            if (buffer.data != nullptr && buffer.data != kMapFailed) munmap(buffer.data, buffer.length);
        }
        close(fd);
        set_state(State::Failed, "Could not allocate camera buffers");
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        set_state(State::Failed, "Camera stream did not start");
    } else {
        set_state(State::Streaming, "Point the camera at a Wi-Fi QR code");
        while (!g_stop_requested) {
            v4l2_buffer buffer{};
            buffer.type = request.type;
            buffer.memory = request.memory;
            if (ioctl(fd, VIDIOC_DQBUF, &buffer) != 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (buffer.index < kBufferCount) {
                publish_frame(static_cast<const uint16_t *>(buffers[buffer.index].data),
                              format.fmt.pix.width, format.fmt.pix.height,
                              format.fmt.pix.bytesperline, decoder);
            }
            ioctl(fd, VIDIOC_QBUF, &buffer);
        }
        ioctl(fd, VIDIOC_STREAMOFF, &type);
    }

    quirc_destroy(decoder);
    for (auto &buffer : buffers) {
        if (buffer.data != nullptr && buffer.data != kMapFailed) munmap(buffer.data, buffer.length);
    }
    close(fd);
    set_state(State::Stopped, "Camera stopped");
    g_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool start()
{
    if (g_lock == nullptr) g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) return false;
    if (g_preview == nullptr) {
        g_preview = static_cast<uint16_t *>(heap_caps_calloc(
            kPreviewPixelCount, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (g_preview == nullptr) return false;
    g_stop_requested = false;
    if (g_task != nullptr) return true;
    return xTaskCreatePinnedToCore(camera_task, "camera_qr", 32768, nullptr, 3, &g_task, 1) == pdPASS;
}

void stop()
{
    g_stop_requested = true;
}

Status status()
{
    Status copy{};
    if (g_lock != nullptr && xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        copy = g_status;
        xSemaphoreGive(g_lock);
    }
    return copy;
}

bool copyPreview(uint16_t *destination, std::size_t pixel_capacity, uint32_t &generation)
{
    if (destination == nullptr || pixel_capacity < kPreviewPixelCount || g_preview == nullptr ||
        g_lock == nullptr || xSemaphoreTake(g_lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    if (generation == g_status.preview_generation) {
        xSemaphoreGive(g_lock);
        return false;
    }
    std::memcpy(destination, g_preview, kPreviewPixelCount * sizeof(uint16_t));
    generation = g_status.preview_generation;
    xSemaphoreGive(g_lock);
    return true;
}

}  // namespace bluepaws::camera
