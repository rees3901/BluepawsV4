#include "home_hub_camera.h"

#include "guition_jc4880p443c.h"

#include "esp_cache.h"
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
constexpr uint32_t kTargetCaptureFps = 5;
constexpr uint32_t kDecodeEveryFrames = 1;
constexpr uint32_t kScanWidth = 384;
constexpr uint32_t kScanHeight = 384;

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
uint32_t g_scan_attempts = 0;
volatile int8_t g_scan_brightness = 0;
bool g_qr_found = false;

void set_state(State state, const char *message)
{
    if (g_lock == nullptr || xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    g_status.state = state;
    std::snprintf(g_status.message, sizeof(g_status.message), "%s", message);
    xSemaphoreGive(g_lock);
}

uint8_t rgb565_gray(uint16_t pixel)
{
    // V4L2_PIX_FMT_RGB565 (RGBP) is little-endian. Use Rec. 601-style
    // luminance weighting so coloured phone pixels retain the same contrast
    // that is visible in the preview.
    const uint32_t red = ((pixel >> 11U) & 0x1FU) * 255U / 31U;
    const uint32_t green = ((pixel >> 5U) & 0x3FU) * 255U / 63U;
    const uint32_t blue = (pixel & 0x1FU) * 255U / 31U;
    return static_cast<uint8_t>((red * 77U + green * 150U + blue * 29U) >> 8U);
}

uint8_t adjusted_gray(uint16_t pixel, int brightness_offset)
{
    return static_cast<uint8_t>(std::clamp<int>(
        static_cast<int>(rgb565_gray(pixel)) + brightness_offset, 0, 255));
}

uint16_t gray_rgb565(uint8_t gray)
{
    return static_cast<uint16_t>(
        ((gray >> 3U) << 11U) | ((gray >> 2U) << 5U) | (gray >> 3U));
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
            g_preview[y * kPreviewWidth + x] = gray_rgb565(rgb565_gray(pixel));
        }
    }
    ++g_status.preview_generation;
    captured_frames = ++g_status.captured_frames;
    xSemaphoreGive(g_lock);

    if (captured_frames == 1) {
        uint16_t minimum = UINT16_MAX;
        uint16_t maximum = 0;
        uint32_t nonzero = 0;
        for (uint32_t y = 0; y < height; y += std::max<uint32_t>(1, height / 24U)) {
            for (uint32_t x = 0; x < width; x += std::max<uint32_t>(1, width / 32U)) {
                const uint16_t pixel = source[y * stride_pixels + x];
                minimum = std::min(minimum, pixel);
                maximum = std::max(maximum, pixel);
                nonzero += pixel != 0;
            }
        }
        ESP_LOGI(kTag,
                 "First frame received: %lux%lu stride=%lu sample_min=0x%04x "
                 "sample_max=0x%04x nonzero=%lu",
                 static_cast<unsigned long>(width), static_cast<unsigned long>(height),
                 static_cast<unsigned long>(stride_bytes), minimum, maximum,
                 static_cast<unsigned long>(nonzero));
    }

    if (g_qr_found || captured_frames % kDecodeEveryFrames != 0) return;
    const int8_t selected_brightness = g_scan_brightness;
    // Auto cycles through normal, darker, and brighter grayscale once per
    // scan. This handles phone-screen glare without changing sensor exposure.
    const int brightness_offset = selected_brightness == 0
        ? (g_scan_attempts % 3U == 0 ? 0 : (g_scan_attempts % 3U == 1 ? -40 : 40))
        : selected_brightness * 40;
    uint8_t *gray = quirc_begin(decoder, nullptr, nullptr);
    const uint32_t crop_size = std::min(width, height);
    const uint32_t crop_left = (width - crop_size) / 2U;
    const uint32_t crop_top = (height - crop_size) / 2U;
    for (uint32_t y = 0; y < kScanHeight; ++y) {
        const uint32_t source_y = crop_top + y * crop_size / kScanHeight;
        for (uint32_t x = 0; x < kScanWidth; ++x) {
            const uint32_t source_x = crop_left + x * crop_size / kScanWidth;
            gray[y * kScanWidth + x] = adjusted_gray(
                source[source_y * stride_pixels + source_x], brightness_offset);
        }
    }
    quirc_end(decoder);
    const int count = quirc_count(decoder);
    ++g_scan_attempts;
    if (count > 0) {
        ESP_LOGI(kTag, "QR finder candidates: %d", count);
    } else if (g_scan_attempts % 10U == 0) {
        ESP_LOGI(kTag, "No QR finder candidate after %lu scan attempts",
                 static_cast<unsigned long>(g_scan_attempts));
    }
    for (int index = 0; index < count; ++index) {
        quirc_code code{};
        quirc_data data{};
        quirc_extract(decoder, index, &code);
        quirc_decode_error_t result = quirc_decode(&code, &data);
        if (result != QUIRC_SUCCESS) {
            quirc_flip(&code);
            result = quirc_decode(&code, &data);
        }
        if (result != QUIRC_SUCCESS || data.payload_len == 0) {
            ESP_LOGW(kTag, "QR candidate %d could not be decoded: %s", index,
                     quirc_strerror(result));
            continue;
        }
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
            g_qr_found = true;
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
    ESP_LOGI(kTag, "Starting OV02C10 MIPI-CSI camera");
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
        ESP_LOGI(kTag, "esp_video_init returned %s", esp_err_to_name(result));
    }
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "OV02C10 initialization failed: %s", esp_err_to_name(result));
        set_state(State::Failed, "OV02C10 initialization failed");
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(kTag, "Could not open %s: errno=%d (%s)",
                 ESP_VIDEO_MIPI_CSI_DEVICE_NAME, errno, std::strerror(errno));
        set_state(State::Failed, "Could not open MIPI-CSI video device");
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(kTag, "VIDIOC_G_FMT failed: errno=%d (%s)", errno, std::strerror(errno));
        close(fd);
        set_state(State::Failed, "Could not read camera format");
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(fd, VIDIOC_S_FMT, &format) != 0 || ioctl(fd, VIDIOC_G_FMT, &format) != 0) {
            ESP_LOGE(kTag, "RGB565 format setup failed: errno=%d (%s)",
                     errno, std::strerror(errno));
            close(fd);
            set_state(State::Failed, "Camera RGB565 format is unavailable");
            g_task = nullptr;
            vTaskDelete(nullptr);
            return;
        }
    }
    ESP_LOGI(kTag, "Camera format: %lux%lu fourcc=0x%08lx stride=%lu size=%lu",
             static_cast<unsigned long>(format.fmt.pix.width),
             static_cast<unsigned long>(format.fmt.pix.height),
             static_cast<unsigned long>(format.fmt.pix.pixelformat),
             static_cast<unsigned long>(format.fmt.pix.bytesperline),
             static_cast<unsigned long>(format.fmt.pix.sizeimage));

    // The sensor runs at 30 fps, but a QR scanner does not need to process a
    // 1080p frame that often. The ESP video driver implements this as frame
    // skipping, leaving CPU time for LVGL while retaining the sensor's native
    // resolution and automatic exposure pipeline.
    v4l2_streamparm stream_parameters{};
    stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    stream_parameters.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    stream_parameters.parm.capture.timeperframe.numerator = 1;
    stream_parameters.parm.capture.timeperframe.denominator = kTargetCaptureFps;
    if (ioctl(fd, VIDIOC_S_PARM, &stream_parameters) != 0) {
        ESP_LOGW(kTag, "Could not limit camera to %lu fps: errno=%d (%s)",
                 static_cast<unsigned long>(kTargetCaptureFps), errno, std::strerror(errno));
    } else {
        ESP_LOGI(kTag, "Camera processing rate limited to %lu fps",
                 static_cast<unsigned long>(kTargetCaptureFps));
    }

    v4l2_requestbuffers request{};
    request.count = kBufferCount;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    CaptureBuffer buffers[kBufferCount]{};
    bool buffers_ready = ioctl(fd, VIDIOC_REQBUFS, &request) == 0 && request.count >= kBufferCount;
    if (!buffers_ready) {
        ESP_LOGE(kTag, "VIDIOC_REQBUFS failed or returned %lu buffers: errno=%d (%s)",
                 static_cast<unsigned long>(request.count), errno, std::strerror(errno));
    }
    for (unsigned i = 0; buffers_ready && i < kBufferCount; ++i) {
        v4l2_buffer buffer{};
        buffer.type = request.type;
        buffer.memory = request.memory;
        buffer.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            ESP_LOGE(kTag, "VIDIOC_QUERYBUF[%u] failed: errno=%d (%s)",
                     i, errno, std::strerror(errno));
            buffers_ready = false;
            break;
        }
        buffers[i].length = buffer.length;
        buffers[i].data = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                               buffer.m.offset);
        if (buffers[i].data == kMapFailed || ioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
            ESP_LOGE(kTag, "Camera buffer[%u] map/queue failed: errno=%d (%s)",
                     i, errno, std::strerror(errno));
            buffers_ready = false;
        }
    }

    quirc *decoder = quirc_new();
    if (!buffers_ready || decoder == nullptr ||
        quirc_resize(decoder, kScanWidth, kScanHeight) < 0) {
        ESP_LOGE(kTag, "Camera buffer/QR decoder setup failed (buffers=%d decoder=%p)",
                 buffers_ready, decoder);
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
        ESP_LOGE(kTag, "VIDIOC_STREAMON failed: errno=%d (%s)", errno, std::strerror(errno));
        set_state(State::Failed, "Camera stream did not start");
    } else {
        ESP_LOGI(kTag, "Camera stream started");
        set_state(State::Streaming, "Point the camera at a Wi-Fi QR code");
        uint32_t dequeue_failures = 0;
        while (!g_stop_requested) {
            v4l2_buffer buffer{};
            buffer.type = request.type;
            buffer.memory = request.memory;
            if (ioctl(fd, VIDIOC_DQBUF, &buffer) != 0) {
                ++dequeue_failures;
                if (dequeue_failures == 1 || dequeue_failures % 100U == 0) {
                    ESP_LOGW(kTag, "VIDIOC_DQBUF failure %lu: errno=%d (%s)",
                             static_cast<unsigned long>(dequeue_failures), errno,
                             std::strerror(errno));
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (buffer.index < kBufferCount) {
                esp_cache_msync(buffers[buffer.index].data, buffers[buffer.index].length,
                                ESP_CACHE_MSYNC_FLAG_DIR_M2C);
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
    ESP_LOGI(kTag, "Camera stream stopped");
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
    if (g_task != nullptr && !g_stop_requested) return true;
    // If the page was reopened while the previous stream was still closing,
    // wait for that worker instead of reporting success without starting a
    // replacement task (which left the reopened preview dark).
    for (unsigned attempt = 0; g_task != nullptr && attempt < 100; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (g_task != nullptr) {
        ESP_LOGW(kTag, "Previous camera worker did not stop in time");
        return false;
    }
    g_stop_requested = false;
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_status.captured_frames = 0;
        std::snprintf(g_status.message, sizeof(g_status.message), "Starting OV02C10 camera...");
        g_status.state = State::Starting;
        xSemaphoreGive(g_lock);
    }
    g_scan_attempts = 0;
    g_qr_found = false;
    return xTaskCreatePinnedToCore(
        camera_task, "camera_qr", 32768, nullptr, 3, &g_task, 1) == pdPASS;
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

void setScanBrightness(int8_t level)
{
    g_scan_brightness = std::clamp<int8_t>(level, -1, 1);
    ESP_LOGI(kTag, "QR scan brightness set to %s",
             g_scan_brightness < 0 ? "darker" : (g_scan_brightness > 0 ? "brighter" : "auto"));
}

int8_t scanBrightness()
{
    return g_scan_brightness;
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
