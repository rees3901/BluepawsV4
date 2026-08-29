#include "bluepaws/cat_simulator.h"
#include "bluepaws/cat_store.h"
#include "bluepaws/map_engine.h"
#include "guition_jc4880p443c.h"
#include "app_shell.h"
#include "ui_icons.h"

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
constexpr int32_t kMapLeft = 0;
constexpr int32_t kMapTop = 0;
constexpr int32_t kMarkerSize = 28;
constexpr size_t kTilePixelBytes = bluepaws::map::kTileSize * bluepaws::map::kTileSize * 2;
constexpr char kTag[] = "bluepaws_home_hub";
constexpr uint32_t kMarkerColours[] = {
    0x1E88E5, 0xE53935, 0x43A047, 0xFB8C00,
    0x8E24AA, 0x00897B, 0x6D4C41, 0x546E7A,
};

enum class AppPage : uint8_t {
    Launcher,
    Map,
    Summary,
    Settings,
    Diagnostics,
};

struct UiLayout {
    int32_t map_width;
    int32_t map_height;
    int32_t map_panel_width;
    int32_t map_panel_height;
};

constexpr UiLayout kLandscapeLayout{784, 406, 784, 406};
constexpr UiLayout kPortraitLayout{464, 726, 464, 726};

struct TileCacheEntry {
    bluepaws::map::TileId id{};
    lv_image_dsc_t descriptor{};
    uint8_t *pixels = nullptr;
    size_t pixel_capacity = 0;
    uint32_t generation = 0;
    bool valid = false;
};

struct UiState {
    bluepaws::CatStore cats;
    bluepaws::CatSimulator simulator{kTestOrigin};
    bluepaws::map::Viewport viewport{
        kLandscapeLayout.map_width, kLandscapeLayout.map_height, kTestOrigin, 15};
    lv_display_t *display = nullptr;
    lv_obj_t *map_view = nullptr;
    std::array<lv_obj_t *, bluepaws::map::kMaximumVisibleTiles> tile_images{};
    std::array<TileCacheEntry, bluepaws::map::kMaximumVisibleTiles> tile_cache{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> markers{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> summary_rows{};
    lv_obj_t *cat_list = nullptr;
    lv_obj_t *diagnostics_text = nullptr;
    lv_obj_t *map_drawer = nullptr;
    lv_obj_t *status = nullptr;
    lv_timer_t *update_timer = nullptr;
    guition_jc4880p443c_sd_info_t sd{};
    esp_err_t sd_error = ESP_FAIL;
    jpeg_decoder_handle_t jpeg_decoder = nullptr;
    size_t visible_tile_count = 0;
    size_t prepared_tile_count = 0;
    uint32_t map_refresh_ms = 0;
    uint32_t cache_generation = 0;
    AppPage active_page = AppPage::Launcher;
    bool dark_mode = true;
    bool drawer_open = false;
    bool portrait = false;
    bool tiles_dirty = true;
};

const UiLayout &current_layout(const UiState &ui)
{
    return ui.portrait ? kPortraitLayout : kLandscapeLayout;
}

uint32_t uptime_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool decode_tile(UiState &ui, TileCacheEntry &entry, const char *path)
{
    if (ui.jpeg_decoder == nullptr) {
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

    if (entry.pixels == nullptr) {
        jpeg_decode_memory_alloc_cfg_t output_config{
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        entry.pixels = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(
            kTilePixelBytes, &output_config, &entry.pixel_capacity));
        if (entry.pixels == nullptr || entry.pixel_capacity < kTilePixelBytes) {
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
                                 entry.pixels,
                                 entry.pixel_capacity,
                                 &output_size);
    const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - started_us + 999) / 1000);
    heap_caps_free(input);
    if (error != ESP_OK || output_size < kTilePixelBytes) {
        ESP_LOGE(kTag, "Hardware JPEG decode failed for %s: %s", path, esp_err_to_name(error));
        return false;
    }

    lv_image_dsc_t &descriptor = entry.descriptor;
    descriptor = {};
    descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    descriptor.header.w = bluepaws::map::kTileSize;
    descriptor.header.h = bluepaws::map::kTileSize;
    descriptor.header.stride = bluepaws::map::kTileSize * 2;
    descriptor.data_size = kTilePixelBytes;
    descriptor.data = entry.pixels;
    ESP_LOGI(kTag, "Hardware-decoded map tile in %lu ms: %s",
             static_cast<unsigned long>(elapsed_ms), path);
    return true;
}

TileCacheEntry *acquire_tile(UiState &ui,
                             const bluepaws::map::TileId &id,
                             const char *path)
{
    for (TileCacheEntry &entry : ui.tile_cache) {
        if (entry.valid && entry.id == id) {
            entry.generation = ui.cache_generation;
            return &entry;
        }
    }

    TileCacheEntry *candidate = nullptr;
    for (TileCacheEntry &entry : ui.tile_cache) {
        if (!entry.valid) {
            candidate = &entry;
            break;
        }
        if (entry.generation != ui.cache_generation &&
            (candidate == nullptr || entry.generation < candidate->generation)) {
            candidate = &entry;
        }
    }
    if (candidate == nullptr) {
        ESP_LOGE(kTag, "No unpinned map tile cache entry is available");
        return nullptr;
    }

    candidate->valid = false;
    if (!decode_tile(ui, *candidate, path)) {
        return nullptr;
    }
    candidate->id = id;
    candidate->generation = ui.cache_generation;
    candidate->valid = true;
    return candidate;
}

void refresh_map_tiles(UiState &ui)
{
    if (!ui.tiles_dirty) {
        return;
    }
    ui.tiles_dirty = false;
    ui.visible_tile_count = 0;
    ui.prepared_tile_count = 0;
    ui.map_refresh_ms = 0;
    const int64_t refresh_started_us = esp_timer_get_time();

    if (!ui.sd.mounted) {
        for (lv_obj_t *image : ui.tile_images) {
            if (image != nullptr) {
                lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
            }
        }
        ui.map_refresh_ms = static_cast<uint32_t>(
            (esp_timer_get_time() - refresh_started_us + 999) / 1000);
        return;
    }

    const bluepaws::map::TileGrid visible_grid = ui.viewport.visibleTiles(0);
    const bluepaws::map::TileGrid grid = ui.viewport.visibleTiles(1);
    ui.visible_tile_count = visible_grid.count;
    ++ui.cache_generation;
    if (ui.cache_generation == 0) {
        ++ui.cache_generation;
        for (TileCacheEntry &entry : ui.tile_cache) {
            entry.generation = 0;
        }
    }
    for (size_t i = 0; i < ui.tile_images.size(); ++i) {
        lv_obj_t *image = ui.tile_images[i];
        if (image == nullptr) {
            continue;
        }
        if (i >= grid.count) {
            lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
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
        TileCacheEntry *entry = acquire_tile(ui, placement.id, filesystem_path);
        if (entry == nullptr) {
            lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_image_set_src(image, &entry->descriptor);
        lv_obj_set_pos(image, placement.screen_x, placement.screen_y);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(image);
        ++ui.prepared_tile_count;
    }
    ui.map_refresh_ms = static_cast<uint32_t>(
        (esp_timer_get_time() - refresh_started_us + 999) / 1000);
}

void update_ui(UiState &ui)
{
    const uint32_t now_ms = uptime_ms();
    ui.simulator.update(now_ms, ui.cats);

    if (ui.active_page == AppPage::Map && ui.map_view != nullptr) {
        refresh_map_tiles(ui);
    }

    char list_text[640]{};
    size_t used = 0;
    for (size_t i = 0; i < ui.cats.size(); ++i) {
        const bluepaws::CatRecord *cat = ui.cats.at(i);
        if (cat == nullptr) {
            continue;
        }

        const int written = ui.portrait
            ? std::snprintf(list_text + used,
                            sizeof(list_text) - used,
                            "%s %u%% %d dBm%s",
                            cat->name,
                            cat->latest.battery_percent,
                            cat->latest.rssi,
                            i + 1 == ui.cats.size() ? "" : "\n")
            : std::snprintf(list_text + used,
                            sizeof(list_text) - used,
                            "%s\n  %u%%   %d dBm%s",
                            cat->name,
                            cat->latest.battery_percent,
                            cat->latest.rssi,
                            i + 1 == ui.cats.size() ? "" : "\n");
        if (written > 0) {
            used = std::min(sizeof(list_text) - 1, used + static_cast<size_t>(written));
        }

        if (ui.summary_rows[i] != nullptr) {
            const uint32_t age_seconds = (now_ms - cat->latest.received_at_ms) / 1000U;
            lv_label_set_text_fmt(ui.summary_rows[i],
                                  "%s\nID %u  |  %u%%  |  %d dBm  |  %lus ago",
                                  cat->name,
                                  static_cast<unsigned>(cat->device_id),
                                  static_cast<unsigned>(cat->latest.battery_percent),
                                  static_cast<int>(cat->latest.rssi),
                                  static_cast<unsigned long>(age_seconds));
        }

        lv_obj_t *marker = ui.markers[i];
        if (marker != nullptr && cat->has_position) {
            const bluepaws::map::GeoPoint position{
                static_cast<double>(cat->last_valid_latitude_e7) / 1.0e7,
                static_cast<double>(cat->last_valid_longitude_e7) / 1.0e7,
            };
            const bluepaws::map::ScreenPoint point = ui.viewport.toScreen(position);
            const int32_t x = static_cast<int32_t>(point.x);
            const int32_t y = static_cast<int32_t>(point.y);
            if (x < 0 || x >= ui.viewport.width() || y < 0 || y >= ui.viewport.height()) {
                lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(marker, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(marker, x - kMarkerSize / 2, y - kMarkerSize / 2);
            }
        }
    }

    if (ui.cat_list != nullptr) {
        lv_label_set_text(ui.cat_list, list_text);
    }

    constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
    const unsigned volume_gib = static_cast<unsigned>((ui.sd.volume_total_bytes + kGiB / 2) / kGiB);
    const unsigned card_gib = static_cast<unsigned>((ui.sd.card_capacity_bytes + kGiB / 2) / kGiB);

    if (ui.diagnostics_text != nullptr) {
        const unsigned internal_kib = static_cast<unsigned>(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U);
        const unsigned psram_kib = static_cast<unsigned>(
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U);
        size_t cached_tiles = 0;
        for (const TileCacheEntry &entry : ui.tile_cache) {
            cached_tiles += entry.valid ? 1U : 0U;
        }
        lv_label_set_text_fmt(ui.diagnostics_text,
                              "BOARD\nESP32-P4 rev 1.3  |  800 x 480 RGB display\n\n"
                              "RUNTIME\nUptime: %lu s\nOrientation: %s\nTouch contacts: %u\n\n"
                              "STORAGE + MAP\nSD: %s  |  FAT volume %u GiB  |  card %u GiB\n"
                              "Map zoom: %u  |  decoded cache: %u/%u tiles\n\n"
                              "MEMORY\nInternal free: %u KiB\nPSRAM free: %u KiB\n\n"
                              "SERVICES\nCat simulator: %u active\nC6 networking: not started\nLoRa receiver: not started",
                              static_cast<unsigned long>(now_ms / 1000U),
                              ui.portrait ? "portrait" : "landscape",
                              static_cast<unsigned>(guition_jc4880p443c_touch_count()),
                              ui.sd.mounted ? "mounted" : "unavailable",
                              volume_gib,
                              card_gib,
                              static_cast<unsigned>(ui.viewport.zoom()),
                              static_cast<unsigned>(cached_tiles),
                              static_cast<unsigned>(ui.tile_cache.size()),
                              internal_kib,
                              psram_kib,
                              static_cast<unsigned>(ui.cats.size()));
    }

    if (ui.status == nullptr) {
        return;
    }
    switch (ui.active_page) {
    case AppPage::Launcher:
        lv_label_set_text_fmt(ui.status,
                              "%u cats online  |  SD %s  |  touch ready",
                              static_cast<unsigned>(ui.cats.size()),
                              ui.sd.mounted ? "ready" : "unavailable");
        break;
    case AppPage::Map:
        if (ui.sd.mounted) {
            lv_label_set_text_fmt(ui.status,
                                  ui.portrait
                                      ? "%u cats | z%u | SD %u/%u | view %u | %lu ms"
                                      : "SIM | %u cats | z%u | SD %u/%u GiB | view %u | %lu ms",
                                  static_cast<unsigned>(ui.cats.size()),
                                  static_cast<unsigned>(ui.viewport.zoom()),
                                  volume_gib,
                                  card_gib,
                                  static_cast<unsigned>(ui.visible_tile_count),
                                  static_cast<unsigned long>(ui.map_refresh_ms));
        } else {
            lv_label_set_text(ui.status, "SIMULATOR | SD unavailable | touch ready");
        }
        break;
    case AppPage::Summary:
        lv_label_set_text_fmt(ui.status,
                              "%u reporting  |  live simulator data  |  updates every second",
                              static_cast<unsigned>(ui.cats.size()));
        break;
    case AppPage::Settings:
        lv_label_set_text(ui.status, "Configuration preview | values are not persisted yet");
        break;
    case AppPage::Diagnostics:
        lv_label_set_text_fmt(ui.status,
                              "Uptime %lu s | SD %s | %u cats",
                              static_cast<unsigned long>(now_ms / 1000U),
                              ui.sd.mounted ? "mounted" : "unavailable",
                              static_cast<unsigned>(ui.cats.size()));
        break;
    }
}

void update_timer(lv_timer_t *timer)
{
    auto *ui = static_cast<UiState *>(lv_timer_get_user_data(timer));
    update_ui(*ui);
}

void change_zoom(UiState &ui, int delta);

void map_pressing(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    lv_indev_t *indev = lv_indev_active();
    if (ui == nullptr || indev == nullptr) {
        return;
    }
    const int8_t pinch_steps = guition_jc4880p443c_take_pinch_steps();
    if (pinch_steps != 0) {
        change_zoom(*ui, pinch_steps);
    }
    if (guition_jc4880p443c_touch_count() > 1) {
        return;
    }
    lv_point_t vector{};
    lv_indev_get_vect(indev, &vector);
    if (vector.x == 0 && vector.y == 0) {
        return;
    }
    ui->viewport.panBy(vector.x, vector.y);
    ui->tiles_dirty = true;
    update_ui(*ui);
}

void change_zoom(UiState &ui, int delta)
{
    constexpr int kMinimumPackZoom = 12;
    constexpr int kMaximumPackZoom = 17;
    const int next_zoom = std::clamp(
        static_cast<int>(ui.viewport.zoom()) + delta, kMinimumPackZoom, kMaximumPackZoom);
    if (next_zoom == ui.viewport.zoom()) {
        return;
    }
    ui.viewport.setZoom(static_cast<uint8_t>(next_zoom));
    ui.tiles_dirty = true;
    update_ui(ui);
}

void zoom_in_clicked(lv_event_t *event)
{
    change_zoom(*static_cast<UiState *>(lv_event_get_user_data(event)), 1);
}

void zoom_out_clicked(lv_event_t *event)
{
    change_zoom(*static_cast<UiState *>(lv_event_get_user_data(event)), -1);
}

void home_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    ui->viewport.setCenter(kTestOrigin);
    ui->tiles_dirty = true;
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
        points.data(), count, ui->viewport.width(), ui->viewport.height(), 38, 12, 17);
    if (fit.valid) {
        ui->viewport = bluepaws::map::Viewport(
            ui->viewport.width(), ui->viewport.height(), fit.center, fit.zoom);
        ui->tiles_dirty = true;
        update_ui(*ui);
    }
}

void create_ui(UiState &ui);

void rebuild_current_page(void *user_data)
{
    auto *ui = static_cast<UiState *>(user_data);
    lv_obj_clean(lv_screen_active());
    create_ui(*ui);
}

void rebuild_for_orientation(void *user_data)
{
    auto *ui = static_cast<UiState *>(user_data);
    ui->portrait = !ui->portrait;
    lv_display_set_rotation(
        ui->display, ui->portrait ? LV_DISPLAY_ROTATION_0 : LV_DISPLAY_ROTATION_90);
    const UiLayout &layout = current_layout(*ui);
    ui->viewport.resize(layout.map_width, layout.map_height);
    ui->tiles_dirty = true;
    ESP_LOGI(kTag, "UI orientation changed to %s", ui->portrait ? "portrait" : "landscape");
    rebuild_current_page(ui);
}

void orientation_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    lv_async_call(rebuild_for_orientation, ui);
}

void rebuild_for_theme(void *user_data)
{
    auto *ui = static_cast<UiState *>(user_data);
    ui->dark_mode = !ui->dark_mode;
    ui->tiles_dirty = true;
    ESP_LOGI(kTag, "UI theme changed to %s", ui->dark_mode ? "dark" : "light");
    rebuild_current_page(ui);
}

void theme_clicked(lv_event_t *event)
{
    lv_async_call(rebuild_for_theme, lv_event_get_user_data(event));
}

void navigate_to(UiState &ui, AppPage page)
{
    ui.active_page = page;
    ui.drawer_open = false;
    ESP_LOGI(kTag, "Opening app page %u", static_cast<unsigned>(page));
    lv_async_call(rebuild_current_page, &ui);
}

void launcher_clicked(lv_event_t *event)
{
    navigate_to(*static_cast<UiState *>(lv_event_get_user_data(event)), AppPage::Launcher);
}

void map_app_clicked(lv_event_t *event)
{
    navigate_to(*static_cast<UiState *>(lv_event_get_user_data(event)), AppPage::Map);
}

void summary_app_clicked(lv_event_t *event)
{
    navigate_to(*static_cast<UiState *>(lv_event_get_user_data(event)), AppPage::Summary);
}

void settings_app_clicked(lv_event_t *event)
{
    navigate_to(*static_cast<UiState *>(lv_event_get_user_data(event)), AppPage::Settings);
}

void diagnostics_app_clicked(lv_event_t *event)
{
    navigate_to(*static_cast<UiState *>(lv_event_get_user_data(event)), AppPage::Diagnostics);
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t colour)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, colour, 0);
    return label;
}

lv_obj_t *make_map_control(lv_obj_t *parent,
                           int32_t x,
                           int32_t y,
                           const char *text,
                           lv_event_cb_t callback,
                           UiState &ui)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, 44, 38);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x17324D), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_90, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, &ui);
    lv_obj_t *label = make_label(button, text, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
    return button;
}

lv_obj_t *make_map_icon_control(lv_obj_t *parent,
                                int32_t x,
                                int32_t y,
                                const lv_image_dsc_t &icon,
                                lv_event_cb_t callback,
                                UiState &ui)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, 44, 44);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x10202E), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_80, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_pad_all(button, 7, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, &ui);
    lv_obj_t *image = lv_image_create(button);
    lv_image_set_src(image, &icon);
    lv_obj_set_style_image_recolor(image, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    lv_obj_center(image);
    return button;
}

lv_obj_t *make_hamburger_control(lv_obj_t *parent,
                                 int32_t x,
                                 int32_t y,
                                 lv_event_cb_t callback,
                                 UiState &ui)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, 44, 44);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x10202E), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_80, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, &ui);
    for (int32_t offset : {-8, 0, 8}) {
        lv_obj_t *bar = lv_obj_create(button);
        lv_obj_set_size(bar, 24, 3);
        lv_obj_align(bar, LV_ALIGN_CENTER, 0, offset);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
    return button;
}

void drawer_open_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->map_drawer == nullptr) {
        return;
    }
    ui->drawer_open = true;
    ESP_LOGI(kTag, "Map cat drawer opened");
    lv_obj_remove_flag(ui->map_drawer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->map_drawer);
    const int32_t width = lv_obj_get_width(ui->map_drawer);
    lv_obj_set_x(ui->map_drawer, -width);
    lv_anim_t animation{};
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, ui->map_drawer);
    lv_anim_set_values(&animation, -width, 0);
    lv_anim_set_duration(&animation, 180);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&animation, [](void *object, int32_t value) {
        lv_obj_set_x(static_cast<lv_obj_t *>(object), value);
    });
    lv_anim_start(&animation);
}

void drawer_close_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->map_drawer == nullptr) {
        return;
    }
    ui->drawer_open = false;
    ESP_LOGI(kTag, "Map cat drawer closed");
    lv_obj_add_flag(ui->map_drawer, LV_OBJ_FLAG_HIDDEN);
}

void create_map_grid(lv_obj_t *map_view, int32_t map_width, int32_t map_height)
{
    for (int i = 1; i < 5; ++i) {
        lv_obj_t *line = lv_obj_create(map_view);
        lv_obj_set_pos(line, i * map_width / 5, 0);
        lv_obj_set_size(line, 1, map_height);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xC8D8E4), 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 1; i < 4; ++i) {
        lv_obj_t *line = lv_obj_create(map_view);
        lv_obj_set_pos(line, 0, i * map_height / 4);
        lv_obj_set_size(line, map_width, 1);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xC8D8E4), 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
}

bluepaws::ui::PageActions page_actions(UiState &ui, bool show_home)
{
    return {
        .home = show_home ? launcher_clicked : nullptr,
        .rotate = orientation_clicked,
        .theme = theme_clicked,
        .user_data = &ui,
    };
}

void create_launcher(UiState &ui)
{
    lv_obj_t *content = bluepaws::ui::create_page_frame(
        lv_screen_active(),
        "BluePaws Home Hub",
        "Starting services...",
        ui.dark_mode,
        page_actions(ui, false),
        &ui.status);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(content,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER);

    const int32_t tile_width = ui.portrait ? 210 : 180;
    const int32_t tile_height = ui.portrait ? 170 : 160;
    bluepaws::ui::create_app_tile(content,
                                  tile_width,
                                  tile_height,
                                  "Live Map",
                                  &bluepaws::ui::icon_map,
                                  nullptr,
                                  false,
                                  0x1976A3,
                                  ui.dark_mode,
                                  map_app_clicked,
                                  &ui);
    bluepaws::ui::create_app_tile(content,
                                  tile_width,
                                  tile_height,
                                  "Cat Summary",
                                  nullptr,
                                  LV_SYMBOL_LIST,
                                  false,
                                  0x2E7D5B,
                                  ui.dark_mode,
                                  summary_app_clicked,
                                  &ui);
    bluepaws::ui::create_app_tile(content,
                                  tile_width,
                                  tile_height,
                                  "Settings",
                                  &bluepaws::ui::icon_settings,
                                  nullptr,
                                  true,
                                  0x7A5A9E,
                                  ui.dark_mode,
                                  settings_app_clicked,
                                  &ui);
    bluepaws::ui::create_app_tile(content,
                                  tile_width,
                                  tile_height,
                                  "Diagnostics",
                                  &bluepaws::ui::icon_diagnostic,
                                  nullptr,
                                  true,
                                  0xB65E36,
                                  ui.dark_mode,
                                  diagnostics_app_clicked,
                                  &ui);
}

void create_map_page(UiState &ui)
{
    const UiLayout &layout = current_layout(ui);
    ui.tiles_dirty = true;
    lv_obj_t *content = bluepaws::ui::create_page_frame(
        lv_screen_active(),
        "BluePaws | Live Map",
        "Loading offline map...",
        ui.dark_mode,
        page_actions(ui, true),
        &ui.status);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *map_panel = lv_obj_create(content);
    lv_obj_set_size(map_panel, layout.map_panel_width, layout.map_panel_height);
    lv_obj_set_style_bg_color(map_panel, lv_color_hex(0xDDE8EE), 0);
    lv_obj_set_style_border_width(map_panel, 0, 0);
    lv_obj_set_style_radius(map_panel, 0, 0);
    lv_obj_set_style_pad_all(map_panel, 0, 0);
    lv_obj_remove_flag(map_panel, LV_OBJ_FLAG_SCROLLABLE);

    ui.map_view = lv_obj_create(map_panel);
    lv_obj_set_pos(ui.map_view, kMapLeft, kMapTop);
    lv_obj_set_size(ui.map_view, layout.map_width, layout.map_height);
    lv_obj_set_style_bg_color(ui.map_view, lv_color_hex(0xE5EFF4), 0);
    lv_obj_set_style_border_width(ui.map_view, 0, 0);
    lv_obj_set_style_radius(ui.map_view, 0, 0);
    lv_obj_set_style_pad_all(ui.map_view, 0, 0);
    lv_obj_remove_flag(ui.map_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui.map_view, map_pressing, LV_EVENT_PRESSING, &ui);

    for (lv_obj_t *&image : ui.tile_images) {
        image = lv_image_create(ui.map_view);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    }

    create_map_grid(ui.map_view, layout.map_width, layout.map_height);

    for (size_t i = 0; i < ui.markers.size(); ++i) {
        lv_obj_t *marker = lv_obj_create(ui.map_view);
        lv_obj_set_size(marker, kMarkerSize, kMarkerSize);
        lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(marker, lv_color_hex(kMarkerColours[i]), 0);
        lv_obj_set_style_border_color(marker, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(marker, 2, 0);
        lv_obj_set_style_pad_all(marker, 0, 0);
        lv_obj_remove_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *number = make_label(marker, "", lv_color_hex(0xFFFFFF));
        lv_label_set_text_fmt(number, "%u", static_cast<unsigned>(i + 1));
        lv_obj_center(number);
        ui.markers[i] = marker;
    }

    make_hamburger_control(ui.map_view, 8, 8, drawer_open_clicked, ui);
    make_map_control(ui.map_view, 8, 58, "HOME", home_clicked, ui);
    make_map_control(ui.map_view, 8, 100, "FIT", fit_all_clicked, ui);
    make_map_icon_control(ui.map_view,
                          8,
                          layout.map_height - 94,
                          bluepaws::ui::icon_zoom_in,
                          zoom_in_clicked,
                          ui);
    make_map_control(ui.map_view, 8, layout.map_height - 46, "-", zoom_out_clicked, ui);

    const int32_t drawer_width = ui.portrait ? 360 : 280;
    ui.map_drawer = lv_obj_create(map_panel);
    lv_obj_set_pos(ui.map_drawer, 0, 0);
    lv_obj_set_size(ui.map_drawer, drawer_width, layout.map_height);
    lv_obj_set_style_bg_color(ui.map_drawer,
                              ui.dark_mode ? lv_color_hex(0x101B25) : lv_color_hex(0xF7FAFC),
                              0);
    lv_obj_set_style_bg_opa(ui.map_drawer, LV_OPA_90, 0);
    lv_obj_set_style_border_color(ui.map_drawer,
                                  ui.dark_mode ? lv_color_hex(0x486274) : lv_color_hex(0xAFC3D1),
                                  0);
    lv_obj_set_style_border_width(ui.map_drawer, 1, 0);
    lv_obj_set_style_border_side(ui.map_drawer, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_radius(ui.map_drawer, 0, 0);
    lv_obj_set_style_pad_all(ui.map_drawer, 14, 0);
    lv_obj_set_style_pad_gap(ui.map_drawer, 8, 0);
    lv_obj_set_flex_flow(ui.map_drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(ui.map_drawer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *drawer_header = lv_obj_create(ui.map_drawer);
    lv_obj_set_size(drawer_header, LV_PCT(100), 44);
    lv_obj_set_style_bg_opa(drawer_header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drawer_header, 0, 0);
    lv_obj_set_style_pad_all(drawer_header, 0, 0);
    lv_obj_remove_flag(drawer_header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cats_title = make_label(
        drawer_header,
        "Nearby cats",
        ui.dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x17324D));
    lv_obj_set_pos(cats_title, 0, 7);
    lv_obj_set_style_text_font(cats_title, &lv_font_montserrat_18, 0);
    lv_obj_t *close_button = lv_button_create(drawer_header);
    lv_obj_set_size(close_button, 38, 38);
    lv_obj_align(close_button, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(close_button, lv_color_hex(0x2B5878), 0);
    lv_obj_set_style_bg_opa(close_button, LV_OPA_70, 0);
    lv_obj_set_style_radius(close_button, 9, 0);
    lv_obj_set_style_pad_all(close_button, 0, 0);
    lv_obj_add_event_cb(close_button, drawer_close_clicked, LV_EVENT_CLICKED, &ui);
    lv_obj_t *close_label = make_label(close_button, "X", lv_color_hex(0xFFFFFF));
    lv_obj_center(close_label);

    ui.cat_list = make_label(
        ui.map_drawer,
        "Waiting for telemetry...",
        ui.dark_mode ? lv_color_hex(0xCFE2EC) : lv_color_hex(0x29495D));
    lv_obj_set_width(ui.cat_list, LV_PCT(100));
    lv_label_set_long_mode(ui.cat_list, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(ui.cat_list, 1);
    if (!ui.drawer_open) {
        lv_obj_add_flag(ui.map_drawer, LV_OBJ_FLAG_HIDDEN);
    }
}

void style_card(lv_obj_t *card, bool dark_mode)
{
    lv_obj_set_style_bg_color(card,
                              dark_mode ? lv_color_hex(0x15232E) : lv_color_hex(0xFFFFFF),
                              0);
    lv_obj_set_style_border_color(card,
                                  dark_mode ? lv_color_hex(0x486274) : lv_color_hex(0xAFC3D1),
                                  0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
}

void create_summary_page(UiState &ui)
{
    lv_obj_t *content = bluepaws::ui::create_page_frame(
        lv_screen_active(),
        "BluePaws | Cat Summary",
        "Waiting for collar reports...",
        ui.dark_mode,
        page_actions(ui, true),
        &ui.status);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(content,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER);

    const int32_t card_width = ui.portrait ? 448 : 374;
    const int32_t card_height = ui.portrait ? 76 : 88;
    for (size_t i = 0; i < ui.summary_rows.size(); ++i) {
        lv_obj_t *card = lv_obj_create(content);
        lv_obj_set_size(card, card_width, card_height);
        style_card(card, ui.dark_mode);
        lv_obj_set_style_border_color(card, lv_color_hex(kMarkerColours[i]), 0);
        lv_obj_set_style_border_width(card, 3, 0);
        ui.summary_rows[i] = make_label(
            card,
            "Waiting for report...",
            ui.dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D));
        lv_obj_set_width(ui.summary_rows[i], LV_PCT(100));
        lv_obj_set_style_text_font(ui.summary_rows[i], &lv_font_montserrat_14, 0);
    }
}

lv_obj_t *create_setting_card(lv_obj_t *parent,
                              const char *title_text,
                              const char *value,
                              lv_color_t accent,
                              bool dark_mode)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), 78);
    style_card(card, dark_mode);
    lv_obj_set_style_border_color(card, accent, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_t *title = make_label(
        card,
        title_text,
        dark_mode ? lv_color_hex(0x8DCDEC) : lv_color_hex(0x41657A));
    lv_obj_set_pos(title, 2, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_t *value_label = make_label(
        card,
        value,
        dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D));
    lv_obj_set_pos(value_label, 2, 28);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_18, 0);
    return card;
}

void create_settings_page(UiState &ui)
{
    lv_obj_t *content = bluepaws::ui::create_page_frame(
        lv_screen_active(),
        "BluePaws | Settings",
        "Configuration preview",
        ui.dark_mode,
        page_actions(ui, true),
        &ui.status);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(content, ui.portrait ? 12 : 80, 0);

    create_setting_card(
        content, "PRIMARY WI-FI", "Not configured", lv_color_hex(0x1976A3), ui.dark_mode);
    create_setting_card(
        content, "SECONDARY WI-FI", "Not configured", lv_color_hex(0x2E7D5B), ui.dark_mode);
    create_setting_card(
        content, "FALLBACK ACCESS POINT", "BluePaws-Hub", lv_color_hex(0x7A5A9E), ui.dark_mode);

    lv_obj_t *notice = lv_obj_create(content);
    lv_obj_set_size(notice, LV_PCT(100), 92);
    style_card(notice, ui.dark_mode);
    lv_obj_set_style_bg_color(
        notice, ui.dark_mode ? lv_color_hex(0x3B3014) : lv_color_hex(0xFFF4D6), 0);
    lv_obj_set_style_border_color(notice, lv_color_hex(0xD29B29), 0);
    lv_obj_t *notice_text = make_label(
        notice,
        "TESTBED ONLY\nEditing and persistent storage arrive with the C6 networking/settings service.",
        ui.dark_mode ? lv_color_hex(0xFFE29A) : lv_color_hex(0x654A10));
    lv_obj_set_width(notice_text, LV_PCT(100));
    lv_label_set_long_mode(notice_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(notice_text, &lv_font_montserrat_14, 0);
}

void create_diagnostics_page(UiState &ui)
{
    lv_obj_t *content = bluepaws::ui::create_page_frame(
        lv_screen_active(),
        "BluePaws | Diagnostics",
        "Reading hardware state...",
        ui.dark_mode,
        page_actions(ui, true),
        &ui.status);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *panel = lv_obj_create(content);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    style_card(panel, ui.dark_mode);
    lv_obj_set_style_pad_all(panel, 16, 0);
    ui.diagnostics_text = make_label(
        panel,
        "Collecting diagnostics...",
        ui.dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D));
    lv_obj_set_width(ui.diagnostics_text, LV_PCT(100));
    lv_label_set_long_mode(ui.diagnostics_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(ui.diagnostics_text, &lv_font_montserrat_14, 0);
}

void create_ui(UiState &ui)
{
    ui.tile_images.fill(nullptr);
    ui.markers.fill(nullptr);
    ui.summary_rows.fill(nullptr);
    ui.map_view = nullptr;
    ui.cat_list = nullptr;
    ui.diagnostics_text = nullptr;
    ui.map_drawer = nullptr;
    ui.status = nullptr;

    switch (ui.active_page) {
    case AppPage::Launcher:
        create_launcher(ui);
        break;
    case AppPage::Map:
        create_map_page(ui);
        break;
    case AppPage::Summary:
        create_summary_page(ui);
        break;
    case AppPage::Settings:
        create_settings_page(ui);
        break;
    case AppPage::Diagnostics:
        create_diagnostics_page(ui);
        break;
    }

    update_ui(ui);
    if (ui.update_timer == nullptr) {
        ui.update_timer = lv_timer_create(update_timer, 1000, &ui);
    }
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
    ui.display = display;
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
