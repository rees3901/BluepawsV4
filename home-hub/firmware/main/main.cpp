#include "bluepaws/cat_simulator.h"
#include "bluepaws/cat_store.h"
#include "bluepaws/map_engine.h"
#include "guition_jc4880p443c.h"

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <sys/stat.h>

namespace {

constexpr bluepaws::map::GeoPoint kTestOrigin{51.8642, -2.2382};
constexpr int32_t kMapWidth = 515;
constexpr int32_t kMapHeight = 350;
constexpr int32_t kMapLeft = 10;
constexpr int32_t kMapTop = 42;
constexpr int32_t kMarkerSize = 28;
constexpr size_t kTilePixelBytes = bluepaws::map::kTileSize * bluepaws::map::kTileSize * 2;
constexpr char kTag[] = "bluepaws_home_hub";
constexpr uint32_t kMarkerColours[] = {
    0x1E88E5, 0xE53935, 0x43A047, 0xFB8C00,
    0x8E24AA, 0x00897B, 0x6D4C41, 0x546E7A,
};

struct UiState {
    bluepaws::CatStore cats;
    bluepaws::CatSimulator simulator{kTestOrigin};
    bluepaws::map::Viewport viewport{kMapWidth, kMapHeight, kTestOrigin, 15};
    std::array<lv_obj_t *, bluepaws::map::kMaximumVisibleTiles> tile_images{};
    std::array<bluepaws::map::TileId, bluepaws::map::kMaximumVisibleTiles> tile_ids{};
    std::array<lv_image_dsc_t, bluepaws::map::kMaximumVisibleTiles> tile_descriptors{};
    std::array<uint8_t *, bluepaws::map::kMaximumVisibleTiles> tile_pixels{};
    std::array<size_t, bluepaws::map::kMaximumVisibleTiles> tile_pixel_capacities{};
    std::array<bool, bluepaws::map::kMaximumVisibleTiles> tile_assigned{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> markers{};
    lv_obj_t *cat_list = nullptr;
    lv_obj_t *status = nullptr;
    guition_jc4880p443c_sd_info_t sd{};
    esp_err_t sd_error = ESP_FAIL;
    jpeg_decoder_handle_t jpeg_decoder = nullptr;
    size_t visible_tile_count = 0;
    size_t loaded_tile_count = 0;
    uint32_t map_decode_ms = 0;
    bool tiles_dirty = true;
};

uint32_t uptime_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool decode_tile(UiState &ui, size_t slot, const char *path)
{
    if (ui.jpeg_decoder == nullptr || slot >= ui.tile_images.size()) {
        return false;
    }

    struct stat tile_stat {};
    if (stat(path, &tile_stat) != 0 || !S_ISREG(tile_stat.st_mode) || tile_stat.st_size <= 0) {
        return false;
    }

    jpeg_decode_memory_alloc_cfg_t input_config{
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t input_capacity = 0;
    auto *input = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(
        static_cast<size_t>(tile_stat.st_size), &input_config, &input_capacity));
    if (input == nullptr || input_capacity < static_cast<size_t>(tile_stat.st_size)) {
        heap_caps_free(input);
        ESP_LOGE(kTag, "Could not allocate JPEG input buffer for %s", path);
        return false;
    }

    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        heap_caps_free(input);
        return false;
    }
    const size_t jpeg_size = std::fread(input, 1, static_cast<size_t>(tile_stat.st_size), file);
    std::fclose(file);
    if (jpeg_size != static_cast<size_t>(tile_stat.st_size)) {
        heap_caps_free(input);
        ESP_LOGE(kTag, "Short read for map tile %s", path);
        return false;
    }

    jpeg_decode_picture_info_t picture{};
    esp_err_t error = jpeg_decoder_get_info(input, jpeg_size, &picture);
    if (error != ESP_OK || picture.width != bluepaws::map::kTileSize ||
        picture.height != bluepaws::map::kTileSize) {
        heap_caps_free(input);
        ESP_LOGE(kTag, "Unexpected JPEG tile dimensions for %s", path);
        return false;
    }

    if (ui.tile_pixels[slot] == nullptr) {
        jpeg_decode_memory_alloc_cfg_t output_config{
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        ui.tile_pixels[slot] = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(
            kTilePixelBytes, &output_config, &ui.tile_pixel_capacities[slot]));
        if (ui.tile_pixels[slot] == nullptr || ui.tile_pixel_capacities[slot] < kTilePixelBytes) {
            heap_caps_free(input);
            ESP_LOGE(kTag, "Could not allocate decoded tile buffer in PSRAM");
            return false;
        }
    }

    jpeg_decode_cfg_t decode_config{
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t output_size = 0;
    const int64_t started_us = esp_timer_get_time();
    error = jpeg_decoder_process(ui.jpeg_decoder,
                                 &decode_config,
                                 input,
                                 jpeg_size,
                                 ui.tile_pixels[slot],
                                 ui.tile_pixel_capacities[slot],
                                 &output_size);
    const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - started_us + 999) / 1000);
    heap_caps_free(input);
    if (error != ESP_OK || output_size < kTilePixelBytes) {
        ESP_LOGE(kTag, "Hardware JPEG decode failed for %s: %s", path, esp_err_to_name(error));
        return false;
    }

    lv_image_dsc_t &descriptor = ui.tile_descriptors[slot];
    descriptor = {};
    descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    descriptor.header.w = bluepaws::map::kTileSize;
    descriptor.header.h = bluepaws::map::kTileSize;
    descriptor.header.stride = bluepaws::map::kTileSize * 2;
    descriptor.data_size = kTilePixelBytes;
    descriptor.data = ui.tile_pixels[slot];
    ui.map_decode_ms += elapsed_ms;
    ESP_LOGI(kTag, "Hardware-decoded map tile in %lu ms: %s",
             static_cast<unsigned long>(elapsed_ms), path);
    return true;
}

void refresh_map_tiles(UiState &ui)
{
    if (!ui.tiles_dirty) {
        return;
    }
    ui.tiles_dirty = false;
    ui.visible_tile_count = 0;
    ui.loaded_tile_count = 0;
    ui.map_decode_ms = 0;

    if (!ui.sd.mounted) {
        for (lv_obj_t *image : ui.tile_images) {
            if (image != nullptr) {
                lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    const bluepaws::map::TileGrid grid = ui.viewport.visibleTiles(0);
    ui.visible_tile_count = grid.count;
    for (size_t i = 0; i < ui.tile_images.size(); ++i) {
        lv_obj_t *image = ui.tile_images[i];
        if (image == nullptr) {
            continue;
        }
        if (i >= grid.count) {
            lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
            ui.tile_assigned[i] = false;
            continue;
        }

        const bluepaws::map::TilePlacement &placement = grid.tiles[i];
        char filesystem_path[128]{};
        std::snprintf(filesystem_path,
                      sizeof(filesystem_path),
                      "/sdcard/bluepaws/maps/tiles/%u/%lu/%lu.jpg",
                      placement.id.zoom,
                      static_cast<unsigned long>(placement.id.x),
                      static_cast<unsigned long>(placement.id.y));
        if (!ui.tile_assigned[i] || !(ui.tile_ids[i] == placement.id)) {
            if (!decode_tile(ui, i, filesystem_path)) {
                lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
                ui.tile_assigned[i] = false;
                continue;
            }
            lv_image_set_src(image, &ui.tile_descriptors[i]);
            ui.tile_ids[i] = placement.id;
            ui.tile_assigned[i] = true;
        }
        lv_obj_set_pos(image,
                       kMapLeft + placement.screen_x,
                       kMapTop + placement.screen_y);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(image);
        ++ui.loaded_tile_count;
    }
}

void update_ui(UiState &ui)
{
    refresh_map_tiles(ui);
    const uint32_t now_ms = uptime_ms();
    ui.simulator.update(now_ms, ui.cats);

    char list_text[640]{};
    size_t used = 0;
    for (size_t i = 0; i < ui.cats.size(); ++i) {
        const bluepaws::CatRecord *cat = ui.cats.at(i);
        if (cat == nullptr) {
            continue;
        }

        const int written = std::snprintf(
            list_text + used,
            sizeof(list_text) - used,
            "%s\n  %u%%   %d dBm%s",
            cat->name,
            cat->latest.battery_percent,
            cat->latest.rssi,
            i + 1 == ui.cats.size() ? "" : "\n");
        if (written > 0) {
            used = std::min(sizeof(list_text) - 1, used + static_cast<size_t>(written));
        }

        lv_obj_t *marker = ui.markers[i];
        if (marker == nullptr || !cat->has_position) {
            continue;
        }
        const bluepaws::map::GeoPoint position{
            static_cast<double>(cat->last_valid_latitude_e7) / 1.0e7,
            static_cast<double>(cat->last_valid_longitude_e7) / 1.0e7,
        };
        const bluepaws::map::ScreenPoint point = ui.viewport.toScreen(position);
        const int32_t x = static_cast<int32_t>(point.x);
        const int32_t y = static_cast<int32_t>(point.y);
        if (x < 0 || x >= kMapWidth || y < 0 || y >= kMapHeight) {
            lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(marker, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(marker,
                           kMapLeft + x - kMarkerSize / 2,
                           kMapTop + y - kMarkerSize / 2);
        }
    }

    lv_label_set_text(ui.cat_list, list_text);
    if (ui.sd.mounted) {
        constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
        const unsigned volume_gib = static_cast<unsigned>((ui.sd.volume_total_bytes + kGiB / 2) / kGiB);
        const unsigned card_gib = static_cast<unsigned>((ui.sd.card_capacity_bytes + kGiB / 2) / kGiB);
        lv_label_set_text_fmt(ui.status,
                              "SIMULATOR | %u cats | SD %u/%u GiB | tiles %u/%u | %lu ms",
                              static_cast<unsigned>(ui.cats.size()),
                              volume_gib,
                              card_gib,
                              static_cast<unsigned>(ui.loaded_tile_count),
                              static_cast<unsigned>(ui.visible_tile_count),
                              static_cast<unsigned long>(ui.map_decode_ms));
    } else {
        lv_label_set_text_fmt(ui.status,
                              "SIMULATOR  |  %u cats  |  SD unavailable  |  touch ready",
                              static_cast<unsigned>(ui.cats.size()));
    }
}

void update_timer(lv_timer_t *timer)
{
    auto *ui = static_cast<UiState *>(lv_timer_get_user_data(timer));
    update_ui(*ui);
}

void fit_all_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    std::array<bluepaws::map::GeoPoint, bluepaws::kMaximumCats> points{};
    size_t count = 0;
    for (size_t i = 0; i < ui->cats.size(); ++i) {
        const bluepaws::CatRecord *cat = ui->cats.at(i);
        if (cat != nullptr && cat->has_position) {
            points[count++] = {
                static_cast<double>(cat->last_valid_latitude_e7) / 1.0e7,
                static_cast<double>(cat->last_valid_longitude_e7) / 1.0e7,
            };
        }
    }
    const auto fit = bluepaws::map::fitPoints(
        points.data(), count, kMapWidth, kMapHeight, 38, 12, 17);
    if (fit.valid) {
        ui->viewport = bluepaws::map::Viewport(
            kMapWidth, kMapHeight, fit.center, fit.zoom);
        ui->tiles_dirty = true;
        update_ui(*ui);
    }
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t colour)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, colour, 0);
    return label;
}

void create_map_grid(lv_obj_t *map_panel)
{
    for (int i = 1; i < 5; ++i) {
        lv_obj_t *line = lv_obj_create(map_panel);
        lv_obj_set_pos(line, kMapLeft + i * kMapWidth / 5, kMapTop);
        lv_obj_set_size(line, 1, kMapHeight);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xC8D8E4), 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }
    for (int i = 1; i < 4; ++i) {
        lv_obj_t *line = lv_obj_create(map_panel);
        lv_obj_set_pos(line, kMapLeft, kMapTop + i * kMapHeight / 4);
        lv_obj_set_size(line, kMapWidth, 1);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xC8D8E4), 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void create_ui(UiState &ui)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xEAF1F5), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_pad_gap(screen, 0, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, LV_PCT(100), 58);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x17324D), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 18, 0);
    lv_obj_set_style_pad_ver(header, 8, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(header, "BluePaws Home Hub", lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    ui.status = make_label(header, "Starting hardware...", lv_color_hex(0x9FD8FF));
    lv_obj_set_style_text_font(ui.status, &lv_font_montserrat_14, 0);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_set_size(content, LV_PCT(100), 422);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_style_pad_gap(content, 8, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *map_panel = lv_obj_create(content);
    lv_obj_set_size(map_panel, 535, LV_PCT(100));
    lv_obj_set_style_bg_color(map_panel, lv_color_hex(0xF7FAFC), 0);
    lv_obj_set_style_border_color(map_panel, lv_color_hex(0xAFC3D1), 0);
    lv_obj_set_style_border_width(map_panel, 1, 0);
    lv_obj_set_style_radius(map_panel, 10, 0);
    lv_obj_set_style_pad_all(map_panel, 0, 0);
    lv_obj_remove_flag(map_panel, LV_OBJ_FLAG_SCROLLABLE);

    for (lv_obj_t *&image : ui.tile_images) {
        image = lv_image_create(map_panel);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *map_title = make_label(map_panel, "OFFLINE GLOUCESTER TILES", lv_color_hex(0x41657A));
    lv_obj_set_pos(map_title, 12, 12);
    lv_obj_set_style_text_font(map_title, &lv_font_montserrat_14, 0);
    create_map_grid(map_panel);

    for (size_t i = 0; i < ui.markers.size(); ++i) {
        lv_obj_t *marker = lv_obj_create(map_panel);
        lv_obj_set_size(marker, kMarkerSize, kMarkerSize);
        lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(marker, lv_color_hex(kMarkerColours[i]), 0);
        lv_obj_set_style_border_color(marker, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(marker, 2, 0);
        lv_obj_set_style_pad_all(marker, 0, 0);
        lv_obj_remove_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *number = make_label(marker, "", lv_color_hex(0xFFFFFF));
        lv_label_set_text_fmt(number, "%u", static_cast<unsigned>(i + 1));
        lv_obj_center(number);
        ui.markers[i] = marker;
    }

    lv_obj_t *sidebar = lv_obj_create(content);
    lv_obj_set_size(sidebar, 241, LV_PCT(100));
    lv_obj_set_style_bg_color(sidebar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(sidebar, lv_color_hex(0xAFC3D1), 0);
    lv_obj_set_style_border_width(sidebar, 1, 0);
    lv_obj_set_style_radius(sidebar, 10, 0);
    lv_obj_set_style_pad_all(sidebar, 12, 0);
    lv_obj_set_style_pad_gap(sidebar, 8, 0);
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cats_title = make_label(sidebar, "Nearby cats", lv_color_hex(0x17324D));
    lv_obj_set_style_text_font(cats_title, &lv_font_montserrat_18, 0);
    ui.cat_list = make_label(sidebar, "Waiting for telemetry...", lv_color_hex(0x29495D));
    lv_obj_set_width(ui.cat_list, LV_PCT(100));
    lv_label_set_long_mode(ui.cat_list, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(ui.cat_list, 1);

    lv_obj_t *fit_button = lv_button_create(sidebar);
    lv_obj_set_size(fit_button, LV_PCT(100), 42);
    lv_obj_set_style_bg_color(fit_button, lv_color_hex(0x1976D2), 0);
    lv_obj_add_event_cb(fit_button, fit_all_clicked, LV_EVENT_CLICKED, &ui);
    lv_obj_t *fit_label = make_label(fit_button, "Fit all", lv_color_hex(0xFFFFFF));
    lv_obj_center(fit_label);

    update_ui(ui);
    lv_timer_create(update_timer, 1000, &ui);
}

}  // namespace

extern "C" void app_main(void)
{
    lv_display_t *display = guition_jc4880p443c_display_start();
    if (display == nullptr) {
        ESP_LOGE(kTag, "JC4880P443C display/touch initialization failed");
        return;
    }

    static UiState ui;
    ui.sd_error = guition_jc4880p443c_sd_mount(&ui.sd);
    if (ui.sd_error != ESP_OK) {
        ESP_LOGE(kTag, "SD card initialization failed: %s", esp_err_to_name(ui.sd_error));
    } else {
        const jpeg_decode_engine_cfg_t jpeg_config{
            .intr_priority = 0,
            .timeout_ms = 250,
        };
        const esp_err_t jpeg_error = jpeg_new_decoder_engine(&jpeg_config, &ui.jpeg_decoder);
        if (jpeg_error != ESP_OK) {
            ESP_LOGE(kTag, "Hardware JPEG decoder initialization failed: %s", esp_err_to_name(jpeg_error));
        }
    }
    ui.simulator.reset(kTestOrigin, uptime_ms());
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(kTag, "Could not acquire LVGL lock");
        return;
    }
    create_ui(ui);
    lvgl_port_unlock();
    ESP_LOGI(kTag, "BluePaws Home Hub testbed UI started");
}
