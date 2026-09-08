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
constexpr uint32_t kTargetCaptureFps = 15;
constexpr uint32_t kScanWidth = 512;
constexpr uint32_t kScanHeight = 512;
constexpr std::size_t kScanPixelCount = kScanWidth * kScanHeight;

struct CaptureBuffer {
    void *data = nullptr;
    std::size_t length = 0;
};

void *const kMapFailed = reinterpret_cast<void *>(-1);

SemaphoreHandle_t g_lock = nullptr;
SemaphoreHandle_t g_scan_lock = nullptr;
TaskHandle_t g_task = nullptr;
TaskHandle_t g_decode_task = nullptr;
Status g_status{};
uint16_t *g_preview = nullptr;
uint8_t *g_scan_frame = nullptr;
volatile bool g_stop_requested = false;
bool g_video_initialized = false;
uint32_t g_scan_attempts = 0;
uint32_t g_scan_generation = 0;
TickType_t g_capture_started_at = 0;
volatile int16_t g_scan_brightness = 0;
volatile uint16_t g_scan_contrast = 100;
volatile uint16_t g_scan_zoom = 125;
volatile uint8_t g_auto_black = 0;
volatile uint8_t g_auto_white = 255;
volatile bool g_qr_found = false;

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

uint8_t adjusted_gray(uint8_t gray, int brightness_offset, uint16_t contrast_percent)
{
    const int contrasted =
        (static_cast<int>(gray) - 128) * static_cast<int>(contrast_percent) / 100 + 128;
    return static_cast<uint8_t>(std::clamp<int>(
        contrasted + brightness_offset, 0, 255));
}

uint16_t gray_rgb565(uint8_t gray)
{
    return static_cast<uint16_t>(
        ((gray >> 3U) << 11U) | ((gray >> 2U) << 5U) | (gray >> 3U));
}

uint8_t level_gray(uint8_t gray, uint8_t black, uint8_t white)
{
    if (white <= black + 24U) return gray;
    if (gray <= black) return 0;
    if (gray >= white) return 255;
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(gray - black) * 255U + (white - black) / 2U) /
        (white - black));
}

void publish_frame(const uint16_t *source, uint32_t width, uint32_t height, uint32_t stride_bytes)
{
    if (source == nullptr || width == 0 || height == 0 || g_preview == nullptr) return;
    const uint32_t stride_pixels = stride_bytes >= width * 2U ? stride_bytes / 2U : width;
    const uint16_t requested_zoom = g_scan_zoom;
    const uint16_t zoom_percent = std::clamp<uint16_t>(requested_zoom, 100, 200);
    const int brightness_offset = g_scan_brightness;
    const uint16_t contrast_percent = g_scan_contrast;
    const uint8_t auto_black = g_auto_black;
    const uint8_t auto_white = g_auto_white;
    const uint32_t crop_size = std::min(width, height) * 100U / zoom_percent;
    const uint32_t crop_left = (width - crop_size) / 2U;
    const uint32_t crop_top = (height - crop_size) / 2U;
    const uint32_t crop_right = crop_left + crop_size - 1U;
    const uint32_t crop_bottom = crop_top + crop_size - 1U;
    uint32_t captured_frames = 0;
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    for (uint32_t y = 0; y < kPreviewHeight; ++y) {
        const uint32_t source_y = crop_top + y * crop_size / kPreviewHeight;
        for (uint32_t x = 0; x < kPreviewWidth; ++x) {
            const uint32_t source_x = crop_left + x * crop_size / kPreviewWidth;
            const uint16_t pixel = source[source_y * stride_pixels + source_x];
            const uint8_t processed = adjusted_gray(
                level_gray(rgb565_gray(pixel), auto_black, auto_white),
                brightness_offset, contrast_percent);
            // Keep the full grayscale range in the preview. Hard black/white
            // thresholding made sensor noise prominent and could visually merge
            // the small modules and quiet zone around a QR code.
            g_preview[y * kPreviewWidth + x] = gray_rgb565(processed);
        }
    }
    ++g_status.preview_generation;
    captured_frames = ++g_status.captured_frames;
    xSemaphoreGive(g_lock);

    if (captured_frames == 1) {
        g_capture_started_at = xTaskGetTickCount();
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
    } else if (captured_frames % 50U == 0 && g_capture_started_at != 0) {
        const uint32_t elapsed_ms = static_cast<uint32_t>(
            (xTaskGetTickCount() - g_capture_started_at) * portTICK_PERIOD_MS);
        if (elapsed_ms > 0) {
            ESP_LOGI(kTag, "Preview throughput: %.1f fps (%lu frames)",
                     static_cast<double>((captured_frames - 1U) * 1000U) / elapsed_ms,
                     static_cast<unsigned long>(captured_frames));
        }
    }

    // Preserve the full 15 fps user-facing preview. Preparing a 512x512 scan
    // image is comparatively expensive, while the independent decoder cannot
    // consume every camera frame anyway, so stage alternate captures only.
    if ((captured_frames % 2U) == 0U) return;

    // Publish a fresh centre crop to the independent decoder worker. Task
    // notifications coalesce, so a busy decoder always receives the newest
    // image instead of accumulating a stale frame queue.
    if (g_qr_found || g_scan_frame == nullptr || g_scan_lock == nullptr ||
        xSemaphoreTake(g_scan_lock, 0) != pdTRUE) return;
    for (uint32_t y = 0; y < kScanHeight; ++y) {
        const uint32_t source_y = std::min<uint32_t>(
            crop_top + (2U * y + 1U) * crop_size / (2U * kScanHeight), height - 1U);
        const uint32_t source_y_next = std::min<uint32_t>(source_y + 1U, crop_bottom);
        for (uint32_t x = 0; x < kScanWidth; ++x) {
            const uint32_t source_x = std::min<uint32_t>(
                crop_left + (2U * x + 1U) * crop_size / (2U * kScanWidth), width - 1U);
            const uint32_t source_x_next = std::min<uint32_t>(source_x + 1U, crop_right);
            const uint32_t gray_sum =
                rgb565_gray(source[source_y * stride_pixels + source_x]) +
                rgb565_gray(source[source_y * stride_pixels + source_x_next]) +
                rgb565_gray(source[source_y_next * stride_pixels + source_x]) +
                rgb565_gray(source[source_y_next * stride_pixels + source_x_next]);
            g_scan_frame[y * kScanWidth + x] = static_cast<uint8_t>((gray_sum + 2U) / 4U);
        }
    }
    ++g_scan_generation;
    xSemaphoreGive(g_scan_lock);
    if (g_decode_task != nullptr) xTaskNotifyGive(g_decode_task);
}

void decoder_task(void *)
{
    quirc *decoder = quirc_new();
    if (decoder == nullptr || quirc_resize(decoder, kScanWidth, kScanHeight) < 0) {
        ESP_LOGE(kTag, "Could not allocate QR decoder");
        if (decoder != nullptr) quirc_destroy(decoder);
        g_decode_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    uint32_t consumed_generation = 0;
    while (!g_stop_requested) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        if (g_stop_requested || g_qr_found || g_scan_lock == nullptr) continue;
        if (xSemaphoreTake(g_scan_lock, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        if (consumed_generation == g_scan_generation) {
            xSemaphoreGive(g_scan_lock);
            continue;
        }

        // Meter the centre of the guide instead of the surrounding room. A
        // bright phone can otherwise occupy too little of the full camera view
        // to influence exposure and contrast decisions.
        uint32_t histogram[256]{};
        constexpr uint32_t kMeterMargin = kScanWidth / 6U;
        uint32_t metered_pixels = 0;
        for (uint32_t y = kMeterMargin; y < kScanHeight - kMeterMargin; y += 2U) {
            for (uint32_t x = kMeterMargin; x < kScanWidth - kMeterMargin; x += 2U) {
                ++histogram[g_scan_frame[y * kScanWidth + x]];
                ++metered_pixels;
            }
        }
        const uint32_t low_target = metered_pixels / 20U;
        const uint32_t high_target = metered_pixels - low_target;
        uint32_t cumulative = 0;
        uint8_t black = 0;
        uint8_t white = 255;
        bool black_found = false;
        for (unsigned level = 0; level < 256U; ++level) {
            cumulative += histogram[level];
            if (!black_found && cumulative >= low_target) {
                black = static_cast<uint8_t>(level);
                black_found = true;
            }
            if (cumulative >= high_target) {
                white = static_cast<uint8_t>(level);
                break;
            }
        }
        // Preserve headroom so AUTO improves separation without producing the
        // harsh, grainy binary appearance used in the earlier experiment.
        black = black > 8U ? static_cast<uint8_t>(black - 8U) : 0;
        white = white < 247U ? static_cast<uint8_t>(white + 8U) : 255;
        g_auto_black = black;
        g_auto_white = white;

        // Try raw, automatically levelled, and levelled plus mild sharpening
        // in rotation. Manual brightness remains preview-only.
        const unsigned variant = g_scan_attempts % 3U;
        const bool auto_levels = variant != 0;
        const bool sharpen = variant == 2U;
        const uint16_t contrast_percent = g_scan_contrast;
        uint8_t *gray = quirc_begin(decoder, nullptr, nullptr);
        for (uint32_t y = 0; y < kScanHeight; ++y) {
            for (uint32_t x = 0; x < kScanWidth; ++x) {
                const std::size_t index = y * kScanWidth + x;
                const uint8_t centre = auto_levels
                    ? adjusted_gray(level_gray(g_scan_frame[index], black, white),
                                    0, contrast_percent)
                    : g_scan_frame[index];
                if (!sharpen || x == 0 || y == 0 ||
                    x + 1U == kScanWidth || y + 1U == kScanHeight) {
                    gray[index] = centre;
                    continue;
                }
                const int neighbours =
                    adjusted_gray(level_gray(g_scan_frame[index - 1U], black, white),
                                  0, contrast_percent) +
                    adjusted_gray(level_gray(g_scan_frame[index + 1U], black, white),
                                  0, contrast_percent) +
                    adjusted_gray(level_gray(g_scan_frame[index - kScanWidth], black, white),
                                  0, contrast_percent) +
                    adjusted_gray(level_gray(g_scan_frame[index + kScanWidth], black, white),
                                  0, contrast_percent);
                gray[index] = static_cast<uint8_t>(std::clamp<int>(
                    static_cast<int>(centre) * 2 - neighbours / 4, 0, 255));
            }
        }
        consumed_generation = g_scan_generation;
        xSemaphoreGive(g_scan_lock);

        quirc_end(decoder);
        const int count = quirc_count(decoder);
        ++g_scan_attempts;
        if (count > 0) {
            ESP_LOGI(kTag, "QR finder candidates: %d", count);
        } else if (g_scan_attempts % 10U == 0) {
            ESP_LOGI(kTag,
                     "No QR finder candidate after %lu scan attempts (AUTO levels %u..%u)",
                     static_cast<unsigned long>(g_scan_attempts), black, white);
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

    quirc_destroy(decoder);
    g_decode_task = nullptr;
    vTaskDelete(nullptr);
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

    if (!buffers_ready || xTaskCreatePinnedToCore(
            decoder_task, "qr_decode", 32768, nullptr, 2, &g_decode_task, 0) != pdPASS) {
        ESP_LOGE(kTag, "Camera buffer/QR decoder setup failed (buffers=%d)", buffers_ready);
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
        set_state(State::Streaming, "Local QR processing • no frames saved or uploaded");
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
                              format.fmt.pix.bytesperline);
            }
            ioctl(fd, VIDIOC_QBUF, &buffer);
        }
        ioctl(fd, VIDIOC_STREAMOFF, &type);
    }

    g_stop_requested = true;
    if (g_decode_task != nullptr) xTaskNotifyGive(g_decode_task);
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
    if (g_scan_lock == nullptr) g_scan_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr || g_scan_lock == nullptr) return false;
    if (g_preview == nullptr) {
        g_preview = static_cast<uint16_t *>(heap_caps_calloc(
            kPreviewPixelCount, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (g_preview == nullptr) return false;
    if (g_scan_frame == nullptr) {
        g_scan_frame = static_cast<uint8_t *>(heap_caps_calloc(
            kScanPixelCount, sizeof(uint8_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (g_scan_frame == nullptr) return false;
    if (g_task != nullptr && !g_stop_requested) return true;
    // If the page was reopened while the previous stream was still closing,
    // wait for that worker instead of reporting success without starting a
    // replacement task (which left the reopened preview dark).
    for (unsigned attempt = 0;
         (g_task != nullptr || g_decode_task != nullptr) && attempt < 100; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (g_task != nullptr || g_decode_task != nullptr) {
        ESP_LOGW(kTag, "Previous camera workers did not stop in time");
        return false;
    }
    g_stop_requested = false;
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_status.captured_frames = 0;
        g_status.payload[0] = '\0';
        std::snprintf(g_status.message, sizeof(g_status.message), "Starting OV02C10 camera...");
        g_status.state = State::Starting;
        xSemaphoreGive(g_lock);
    }
    g_scan_attempts = 0;
    g_scan_generation = 0;
    g_capture_started_at = 0;
    g_auto_black = 0;
    g_auto_white = 255;
    g_qr_found = false;
    return xTaskCreatePinnedToCore(
        camera_task, "camera_qr", 32768, nullptr, 3, &g_task, 1) == pdPASS;
}

void stop()
{
    g_stop_requested = true;
    if (g_decode_task != nullptr) xTaskNotifyGive(g_decode_task);
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

void setScanBrightness(int16_t offset)
{
    g_scan_brightness = std::clamp<int16_t>(offset, -80, 80);
    ESP_LOGI(kTag, "QR scan brightness set to %+d", g_scan_brightness);
}

int16_t scanBrightness()
{
    return g_scan_brightness;
}

void setScanContrast(uint16_t percent)
{
    g_scan_contrast = std::clamp<uint16_t>(percent, 70, 160);
    ESP_LOGI(kTag, "QR scan contrast set to %u%%", g_scan_contrast);
}

uint16_t scanContrast()
{
    return g_scan_contrast;
}

void setScanZoom(uint16_t percent)
{
    g_scan_zoom = std::clamp<uint16_t>(percent, 100, 200);
    ESP_LOGI(kTag, "QR scan zoom set to %u%%", g_scan_zoom);
}

uint16_t scanZoom()
{
    return g_scan_zoom;
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
