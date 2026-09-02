#include "bluepaws/cat_simulator.h"
#include "bluepaws/cat_store.h"
#include "bluepaws/hub_settings.h"
#include "bluepaws/map_engine.h"
#include "guition_jc4880p443c.h"
#include "app_shell.h"
#include "ui_icons.h"
#include "home_hub_cloud.h"
#include "home_hub_config.h"
#include "home_hub_settings_store.h"

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

// ESP-IDF's default static-allocation hook obtains this memory from the heap
// before app_main. On the P4 build, the two SMP idle tasks and ESP-Hosted leave
// insufficient contiguous stack-capable heap for that timer allocation.
// Keep the timer service deterministic and independent of early heap state.
extern "C" void __wrap_vApplicationGetTimerTaskMemory(
    StaticTask_t **tcb_buffer,
    StackType_t **stack_buffer,
    uint32_t *stack_size)
{
    static StaticTask_t timer_tcb;
    alignas(StackType_t) static uint8_t timer_stack[CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH];

    *tcb_buffer = &timer_tcb;
    *stack_buffer = reinterpret_cast<StackType_t *>(timer_stack);
    *stack_size = CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH;
}

namespace {

constexpr bluepaws::map::GeoPoint kTestOrigin{51.8642, -2.2382};
constexpr int32_t kMapLeft = 0;
constexpr int32_t kMapTop = 0;
constexpr int32_t kMarkerSize = 28;
constexpr size_t kTilePixelBytes = bluepaws::map::kTileSize * bluepaws::map::kTileSize * 2;
constexpr uint32_t kBrightnessTimeoutMs = 1000;
constexpr uint32_t kBrightnessFadeMs = 3000;
constexpr uint32_t kGesturePollMs = 30;
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
    Overview,
};

enum class SettingsField : uint8_t {
    PrimarySsid,
    PrimaryPassword,
    SecondarySsid,
    SecondaryPassword,
    AccessPointSsid,
    AccessPointPassword,
    OverviewTimeout,
    DimTimeout,
    ScreenOffTimeout,
    DimBrightness,
};

enum class MapLayer : uint8_t {
    Street,
    OrdnanceSurvey,
    Satellite,
    Aerial,
};

struct MapLayerInfo {
    const char *name;
    const char *description;
    const char *tile_root;
    uint8_t minimum_zoom;
    uint8_t maximum_zoom;
};

constexpr std::array<MapLayerInfo, 4> kMapLayers{{
    {"OpenStreetMap", "GB overview; 100 km Gloucester detail", "/sdcard/bluepaws/maps/layers/osm-road-100km/tiles", 5, 17},
    {"Ordnance Survey", "Official OS mapping; GB overview and regional detail", "/sdcard/bluepaws/maps/layers/ordnance-survey-100km/tiles", 5, 17},
    {"Satellite", "EA 20 cm Gloucester aerial imagery", "/sdcard/bluepaws/maps/layers/satellite-v2/tiles", 14, 17},
    {"Aerial", "Single-source Gloucester aerial imagery", "/sdcard/bluepaws/maps/layers/aerial-consistent/tiles", 12, 17},
}};

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
    MapLayer layer = MapLayer::Street;
    lv_image_dsc_t descriptor{};
    uint8_t *pixels = nullptr;
    size_t pixel_capacity = 0;
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
    std::array<lv_obj_t *, bluepaws::kMaximumCats> overview_markers{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> overview_labels{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_cards{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_summary_labels{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_name_labels{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_status_images{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_profile_images{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_fault_images{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_battery_images{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_battery_labels{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_signal_images{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_signal_labels{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_radio_images{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_distance_labels{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_age_labels{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_detail_labels{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_expanded_panels{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_message_labels{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_jump_buttons{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_follow_buttons{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_trail_buttons{};
    std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_command_buttons{};
    std::array<bool, bluepaws::kMaximumCats> drawer_card_expanded{};
    std::array<bool, bluepaws::kMaximumCats> trail_enabled{};
    lv_obj_t *cat_list = nullptr;
    lv_obj_t *diagnostics_text = nullptr;
    lv_obj_t *map_drawer = nullptr;
    lv_obj_t *layer_drawer = nullptr;
    lv_obj_t *brightness_popup = nullptr;
    lv_obj_t *brightness_slider = nullptr;
    lv_obj_t *brightness_label = nullptr;
    lv_obj_t *volume_popup = nullptr;
    lv_obj_t *volume_slider = nullptr;
    lv_obj_t *volume_label = nullptr;
    lv_obj_t *quick_settings_tray = nullptr;
    lv_obj_t *quick_settings_handle = nullptr;
    lv_timer_t *brightness_hide_timer = nullptr;
    lv_timer_t *volume_hide_timer = nullptr;
    lv_obj_t *settings_modal = nullptr;
    lv_obj_t *settings_input = nullptr;
    lv_obj_t *settings_keyboard = nullptr;
    lv_obj_t *settings_error = nullptr;
    lv_timer_t *gesture_timer = nullptr;
    lv_obj_t *status = nullptr;
    lv_timer_t *update_timer = nullptr;
    guition_jc4880p443c_sd_info_t sd{};
    esp_err_t sd_error = ESP_FAIL;
    jpeg_decoder_handle_t jpeg_decoder = nullptr;
    size_t visible_tile_count = 0;
    size_t prepared_tile_count = 0;
    size_t loaded_tile_count = 0;
    uint32_t map_refresh_ms = 0;
    AppPage active_page = AppPage::Launcher;
    MapLayer active_map_layer = MapLayer::Street;
    bool dark_mode = true;
    bool drawer_open = false;
    bool layer_drawer_open = false;
    bool quick_settings_open = false;
    bool screensaver_pending = false;
    bool screen_dimmed = false;
    bool screen_off = false;
    bool portrait = false;
    bool tiles_dirty = true;
    bool tile_images_bound = false;
    int brightness_percent = 80;
    int volume_percent = 60;
    int followed_cat = -1;
    bool cloud_enabled = false;
    SettingsField editing_field = SettingsField::PrimarySsid;
    bluepaws::hub::Settings settings = bluepaws::hub::defaultSettings();
};

void rebuild_current_page(void *user_data);
void navigate_to(UiState &ui, AppPage page);

const UiLayout &current_layout(const UiState &ui)
{
    return ui.portrait ? kPortraitLayout : kLandscapeLayout;
}

const MapLayerInfo &map_layer_info(MapLayer layer)
{
    return kMapLayers[static_cast<size_t>(layer)];
}

bool map_layer_available(const UiState &ui, MapLayer layer)
{
    if (!ui.sd.mounted) {
        return false;
    }
    const char *root = map_layer_info(layer).tile_root;
    struct stat directory_stat {};
    if (stat(root, &directory_stat) == 0 && S_ISDIR(directory_stat.st_mode)) {
        return true;
    }
    const int stat_error = errno;
    DIR *directory = opendir(root);
    if (directory != nullptr) {
        closedir(directory);
        return true;
    }
    // Some FatFs/VFS combinations have returned an error for directory
    // metadata while files beneath the same path remain readable. A known
    // centre tile is therefore a final positive probe for the installed
    // satellite pack, preventing a valid layer from being greyed out.
    if (layer == MapLayer::Satellite) {
        char probe_path[160]{};
        std::snprintf(probe_path, sizeof(probe_path), "%s/14/8090/5421.jpg", root);
        struct stat probe_stat {};
        if (stat(probe_path, &probe_stat) == 0 && S_ISREG(probe_stat.st_mode) &&
            probe_stat.st_size > 4) {
            return true;
        }
    }
    ESP_LOGW(kTag,
             "Map layer probe failed: %s path=%s stat_errno=%d opendir_errno=%d",
             map_layer_info(layer).name,
             root,
             stat_error,
             errno);
    return false;
}

void log_map_storage_probe(const UiState &ui)
{
    for (size_t i = 0; i < kMapLayers.size(); ++i) {
        const auto layer = static_cast<MapLayer>(i);
        ESP_LOGI(kTag,
                 "Startup map layer probe: %s=%s path=%s",
                 kMapLayers[i].name,
                 map_layer_available(ui, layer) ? "available" : "unavailable",
                 kMapLayers[i].tile_root);
    }

    constexpr char satellite_probe[] =
        "/sdcard/bluepaws/maps/layers/satellite-v2/tiles/14/8090/5421.jpg";
    struct stat tile_stat {};
    if (stat(satellite_probe, &tile_stat) != 0) {
        ESP_LOGE(kTag, "Satellite centre tile missing: %s errno=%d", satellite_probe, errno);
        return;
    }
    FILE *tile = std::fopen(satellite_probe, "rb");
    uint8_t magic[2]{};
    const size_t read = tile == nullptr ? 0 : std::fread(magic, 1, sizeof(magic), tile);
    if (tile != nullptr) {
        std::fclose(tile);
    }
    ESP_LOGI(kTag,
             "Satellite centre tile: bytes=%ld jpeg=%s path=%s",
             static_cast<long>(tile_stat.st_size),
             read == 2 && magic[0] == 0xFF && magic[1] == 0xD8 ? "yes" : "no",
             satellite_probe);
}

void invalidate_tile_cache(UiState &ui)
{
    for (TileCacheEntry &entry : ui.tile_cache) {
        if (entry.descriptor.data != nullptr) {
            lv_image_cache_drop(&entry.descriptor);
        }
        entry.valid = false;
    }
    ui.tiles_dirty = true;
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

void refresh_map_tiles(UiState &ui)
{
    if (!ui.tiles_dirty) {
        return;
    }
    ui.tiles_dirty = false;
    ui.visible_tile_count = 0;
    ui.prepared_tile_count = 0;
    ui.loaded_tile_count = 0;
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

    bool sources_changed = !ui.tile_images_bound;
    for (size_t i = 0; i < grid.count && !sources_changed; ++i) {
        const TileCacheEntry &entry = ui.tile_cache[i];
        sources_changed = !entry.valid || entry.layer != ui.active_map_layer ||
                          !(entry.id == grid.tiles[i].id);
    }

    if (sources_changed) {
        // Keep LVGL descriptor addresses fixed by screen slot, but reorder the
        // decoded buffer identities before rebinding them. Most of a shifted
        // grid therefore survives and only the newly exposed row/column needs
        // an SD read. This is the fixed-grid strategy used by 0015/map_tiles,
        // extended here with XYZ-aware reuse for continuous panning.
        for (lv_obj_t *image : ui.tile_images) {
            if (image != nullptr) {
                lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
                lv_image_set_src(image, nullptr);
            }
        }
        ui.tile_images_bound = false;

        const auto previous_cache = ui.tile_cache;
        std::array<int, bluepaws::map::kMaximumVisibleTiles> source_indices{};
        std::array<bool, bluepaws::map::kMaximumVisibleTiles> source_used{};
        source_indices.fill(-1);

        for (size_t i = 0; i < grid.count; ++i) {
            for (size_t j = 0; j < previous_cache.size(); ++j) {
                const TileCacheEntry &candidate = previous_cache[j];
                if (!source_used[j] && candidate.valid &&
                    candidate.layer == ui.active_map_layer &&
                    candidate.id == grid.tiles[i].id) {
                    source_indices[i] = static_cast<int>(j);
                    source_used[j] = true;
                    break;
                }
            }
        }

        size_t next_unused = 0;
        for (size_t i = 0; i < source_indices.size(); ++i) {
            if (source_indices[i] >= 0) {
                continue;
            }
            while (next_unused < source_used.size() && source_used[next_unused]) {
                ++next_unused;
            }
            source_indices[i] = static_cast<int>(next_unused);
            source_used[next_unused] = true;
        }
        for (size_t i = 0; i < ui.tile_cache.size(); ++i) {
            ui.tile_cache[i] = previous_cache[static_cast<size_t>(source_indices[i])];
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
        const MapLayerInfo &layer = map_layer_info(ui.active_map_layer);
        char filesystem_path[160]{};
        std::snprintf(filesystem_path,
                      sizeof(filesystem_path),
                      "%s/%u/%lu/%lu.jpg",
                      layer.tile_root,
                      placement.id.zoom,
                      static_cast<unsigned long>(placement.id.x),
                      static_cast<unsigned long>(placement.id.y));
        TileCacheEntry &entry = ui.tile_cache[i];
        if (!entry.valid || entry.layer != ui.active_map_layer || !(entry.id == placement.id)) {
            if (entry.descriptor.data != nullptr) {
                lv_image_cache_drop(&entry.descriptor);
            }
            entry.valid = false;
            if (!decode_tile(ui, entry, filesystem_path)) {
                lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            entry.layer = ui.active_map_layer;
            entry.id = placement.id;
            entry.valid = true;
            ++ui.loaded_tile_count;
        }
        if (sources_changed) {
            lv_image_set_src(image, &entry.descriptor);
            lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_pos(image, placement.screen_x, placement.screen_y);
        ++ui.prepared_tile_count;
    }
    if (sources_changed) {
        ui.tile_images_bound = true;
    }
    ui.map_refresh_ms = static_cast<uint32_t>(
        (esp_timer_get_time() - refresh_started_us + 999) / 1000);
}

void update_ui(UiState &ui)
{
    const uint32_t now_ms = uptime_ms();
    const uint32_t inactive_ms = ui.display == nullptr
        ? 0 : lv_display_get_inactive_time(ui.display);
    const uint32_t overview_ms = static_cast<uint32_t>(
        ui.settings.overview_timeout_seconds) * 1000U;
    const uint32_t dim_ms = static_cast<uint32_t>(ui.settings.dim_timeout_seconds) * 1000U;
    const uint32_t off_ms = static_cast<uint32_t>(ui.settings.screen_off_timeout_seconds) * 1000U;
    if (inactive_ms < 1000U && (ui.screen_dimmed || ui.screen_off)) {
        ui.screen_dimmed = false;
        ui.screen_off = false;
        guition_jc4880p443c_backlight_set(ui.brightness_percent);
    } else if (inactive_ms >= off_ms && !ui.screen_off) {
        ui.screen_off = true;
        ui.screen_dimmed = true;
        guition_jc4880p443c_backlight_set(0);
    } else if (inactive_ms >= dim_ms && !ui.screen_dimmed) {
        ui.screen_dimmed = true;
        guition_jc4880p443c_backlight_set(ui.settings.dim_brightness_percent);
    }
    if (inactive_ms >= overview_ms && ui.active_page != AppPage::Overview &&
        ui.settings_modal == nullptr && !ui.screensaver_pending) {
        ui.screensaver_pending = true;
        ui.active_page = AppPage::Overview;
        lv_async_call(rebuild_current_page, &ui);
    }
    const size_t cloud_updates = bluepaws::cloud::drain(ui.cats);
    if (!ui.cloud_enabled) {
        ui.simulator.update(now_ms, ui.cats);
    } else if (cloud_updates > 0) {
        ui.tiles_dirty = true;
    }
    const bluepaws::cloud::Status cloud_status = bluepaws::cloud::status();
    const char *sync_name = !ui.cloud_enabled ? "simulator"
        : (cloud_status.state == bluepaws::cloud::ConnectionState::Online ? "online"
        : (cloud_status.state == bluepaws::cloud::ConnectionState::Connecting ? "connecting"
        : "degraded"));

    if (ui.followed_cat >= 0 && static_cast<size_t>(ui.followed_cat) < ui.cats.size()) {
        const bluepaws::CatRecord *followed = ui.cats.at(static_cast<size_t>(ui.followed_cat));
        if (followed != nullptr && followed->has_position) {
            ui.viewport.setCenter({
                static_cast<double>(followed->last_valid_latitude_e7) / 1.0e7,
                static_cast<double>(followed->last_valid_longitude_e7) / 1.0e7,
            });
            ui.tiles_dirty = true;
        }
    }

    if (ui.active_page == AppPage::Map && ui.map_view != nullptr) {
        refresh_map_tiles(ui);
    }

    char list_text[640]{};
    size_t used = 0;
    double overview_scale_metres = 250.0;
    for (size_t i = 0; i < ui.cats.size(); ++i) {
        const bluepaws::CatRecord *cat = ui.cats.at(i);
        if (cat == nullptr || !cat->has_position) continue;
        const auto relative = bluepaws::hub::relativePosition(
            kTestOrigin,
            {static_cast<double>(cat->last_valid_latitude_e7) / 1.0e7,
             static_cast<double>(cat->last_valid_longitude_e7) / 1.0e7});
        if (relative.valid) overview_scale_metres = std::max(
            overview_scale_metres, relative.distance_metres * 1.15);
    }
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

        const uint32_t age_seconds = (now_ms - cat->latest.received_at_ms) / 1000U;
        const auto relative = cat->has_position
            ? bluepaws::hub::relativePosition(
                kTestOrigin,
                {static_cast<double>(cat->last_valid_latitude_e7) / 1.0e7,
                 static_cast<double>(cat->last_valid_longitude_e7) / 1.0e7})
            : bluepaws::hub::RelativePosition{};
        const unsigned long distance_metres = relative.valid
            ? static_cast<unsigned long>(std::lround(relative.distance_metres)) : 0UL;
        if (ui.drawer_name_labels[i] != nullptr) {
            lv_label_set_text(ui.drawer_name_labels[i], cat->name);
        }
        if (ui.drawer_status_images[i] != nullptr) {
            lv_image_set_src(ui.drawer_status_images[i],
                             cat->latest.status_code == 0 ? &bluepaws::ui::icon_status_at_home
                                    : &bluepaws::ui::icon_status_out);
        }
        if (ui.drawer_profile_images[i] != nullptr) {
            lv_image_set_src(ui.drawer_profile_images[i],
                             cat->latest.power_profile_code == 0
                                 ? &bluepaws::ui::icon_profile_powersave
                                          : &bluepaws::ui::icon_status_normal);
        }
        if (ui.drawer_fault_images[i] != nullptr) {
            if (cat->latest.status_code == 3 || (cat->latest.flags & 0x80U) != 0) {
                lv_image_set_src(ui.drawer_fault_images[i], &bluepaws::ui::icon_status_error);
                lv_obj_remove_flag(ui.drawer_fault_images[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(ui.drawer_fault_images[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (ui.drawer_battery_images[i] != nullptr) {
            const lv_image_dsc_t *battery_icon = cat->latest.battery_percent >= 85
                ? &bluepaws::ui::icon_battery_full
                : (cat->latest.battery_percent >= 60
                       ? &bluepaws::ui::icon_battery_medium
                       : (cat->latest.battery_percent >= 20
                              ? &bluepaws::ui::icon_battery_low
                              : &bluepaws::ui::icon_battery_error));
            lv_image_set_src(ui.drawer_battery_images[i], battery_icon);
        }
        if (ui.drawer_battery_labels[i] != nullptr) {
            lv_label_set_text_fmt(ui.drawer_battery_labels[i], "%u%%",
                                  static_cast<unsigned>(cat->latest.battery_percent));
        }
        if (ui.drawer_signal_images[i] != nullptr) {
            const lv_image_dsc_t *signal_icon = cat->latest.rssi > -78
                ? &bluepaws::ui::icon_signal_full
                : (cat->latest.rssi > -86
                       ? &bluepaws::ui::icon_signal_high
                       : (cat->latest.rssi > -94
                              ? &bluepaws::ui::icon_signal_medium
                              : (cat->latest.rssi > -102
                                     ? &bluepaws::ui::icon_signal_low
                                     : &bluepaws::ui::icon_signal_mobile)));
            lv_image_set_src(ui.drawer_signal_images[i], signal_icon);
        }
        if (ui.drawer_signal_labels[i] != nullptr) {
            lv_label_set_text(ui.drawer_signal_labels[i],
                              cat->latest.rssi > -80 ? "Excellent" :
                              (cat->latest.rssi > -95 ? "Good" : "Low"));
        }
        if (ui.drawer_radio_images[i] != nullptr) {
            const lv_image_dsc_t *radio_icon = i % 3U == 0U
                ? &bluepaws::ui::icon_radio_rf
                : (i % 3U == 1U ? &bluepaws::ui::icon_radio_wifi
                                 : &bluepaws::ui::icon_radio_4g);
            lv_image_set_src(ui.drawer_radio_images[i], radio_icon);
        }
        if (ui.drawer_distance_labels[i] != nullptr) {
            if (relative.valid) {
                lv_label_set_text_fmt(ui.drawer_distance_labels[i], "%lum", distance_metres);
            } else {
                lv_label_set_text(ui.drawer_distance_labels[i], "--m");
            }
        }
        if (ui.drawer_age_labels[i] != nullptr) {
            lv_label_set_text_fmt(ui.drawer_age_labels[i], "%lus",
                                  static_cast<unsigned long>(age_seconds));
        }
        if (ui.drawer_detail_labels[i] != nullptr) {
            lv_label_set_text_fmt(
                ui.drawer_detail_labels[i],
                "Coordinates        %.5f, %.5f\n"
                "Device ID                          %u\n"
                "Power Profile              %s\n"
                "Distance from hub              %lum\n"
                "Signal                 %d dBm / %.1f dB\n"
                "Battery                       %u mV\n"
                "Last seen                         %lus",
                static_cast<double>(cat->last_valid_latitude_e7) / 1.0e7,
                static_cast<double>(cat->last_valid_longitude_e7) / 1.0e7,
                static_cast<unsigned>(cat->device_id),
                cat->latest.power_profile_code == 0 ? "PowerSave"
                    : (cat->latest.power_profile_code == 1 ? "Normal"
                    : (cat->latest.power_profile_code == 2 ? "Active"
                    : (cat->latest.power_profile_code == 3 ? "Emergency" : "Debug"))),
                distance_metres,
                static_cast<int>(cat->latest.rssi),
                static_cast<double>(cat->latest.snr),
                static_cast<unsigned>(cat->latest.battery_mv),
                static_cast<unsigned long>(age_seconds));
        }

        if (ui.overview_labels[i] != nullptr) {
            if (relative.valid) {
                lv_label_set_text_fmt(ui.overview_labels[i],
                                      "%s\n%lum | %u o'clock %s | seen %lus ago",
                                      cat->name,
                                      distance_metres,
                                      static_cast<unsigned>(relative.clock_hour),
                                      relative.cardinal,
                                      static_cast<unsigned long>(age_seconds));
            } else {
                lv_label_set_text_fmt(ui.overview_labels[i],
                                      "%s\nPosition unavailable | seen %lus ago",
                                      cat->name,
                                      static_cast<unsigned long>(age_seconds));
            }
        }
        if (ui.overview_markers[i] != nullptr) {
            if (!relative.valid) {
                lv_obj_add_flag(ui.overview_markers[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(ui.overview_markers[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_t *radar = lv_obj_get_parent(ui.overview_markers[i]);
                const double radius = std::max(20.0,
                    std::min(lv_obj_get_width(radar), lv_obj_get_height(radar)) / 2.0 - 36.0);
                const double plotted_radius = std::min(
                    radius, relative.distance_metres / overview_scale_metres * radius);
                const double bearing = relative.bearing_degrees * 3.14159265358979323846 / 180.0;
                const int32_t x = static_cast<int32_t>(
                    lv_obj_get_width(radar) / 2.0 + std::sin(bearing) * plotted_radius - 17.0);
                const int32_t y = static_cast<int32_t>(
                    lv_obj_get_height(radar) / 2.0 - std::cos(bearing) * plotted_radius - 17.0);
                lv_obj_set_pos(ui.overview_markers[i], x, y);
            }
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
                              "Map layer: %s  |  zoom: %u  |  decoded cache: %u/%u tiles\n\n"
                              "MEMORY\nInternal free: %u KiB\nPSRAM free: %u KiB\n\n"
                              "SERVICES\nState source: %s\nCloud snapshots: %lu ok / %lu failed (HTTP %lu)\n"
                              "LoRa receiver: UART adapter pending",
                              static_cast<unsigned long>(now_ms / 1000U),
                              ui.portrait ? "portrait" : "landscape",
                              static_cast<unsigned>(guition_jc4880p443c_touch_count()),
                              ui.sd.mounted ? "mounted" : "unavailable",
                              volume_gib,
                              card_gib,
                              map_layer_info(ui.active_map_layer).name,
                              static_cast<unsigned>(ui.viewport.zoom()),
                              static_cast<unsigned>(cached_tiles),
                              static_cast<unsigned>(ui.tile_cache.size()),
                              internal_kib,
                              psram_kib,
                              sync_name,
                              static_cast<unsigned long>(cloud_status.successful_snapshots),
                              static_cast<unsigned long>(cloud_status.failed_snapshots),
                              static_cast<unsigned long>(cloud_status.last_http_status));
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
                                      ? "%s | %u cats | %s | z%u | SD %u/%u | view %u | new %u | %lu ms"
                                      : "%s | %u cats | %s | z%u | SD %u/%u GiB | view %u | new %u | %lu ms",
                                  sync_name,
                                  static_cast<unsigned>(ui.cats.size()),
                                  map_layer_info(ui.active_map_layer).name,
                                  static_cast<unsigned>(ui.viewport.zoom()),
                                  volume_gib,
                                  card_gib,
                                  static_cast<unsigned>(ui.visible_tile_count),
                                  static_cast<unsigned>(ui.loaded_tile_count),
                                  static_cast<unsigned long>(ui.map_refresh_ms));
        } else {
            lv_label_set_text(ui.status, "SIMULATOR | SD unavailable | touch ready");
        }
        break;
    case AppPage::Summary:
        lv_label_set_text_fmt(ui.status,
                              "%u reporting  |  %s state  |  snapshots every 5 seconds",
                              static_cast<unsigned>(ui.cats.size()), sync_name);
        break;
    case AppPage::Settings:
        lv_label_set_text(ui.status, "Saved locally | Wi-Fi changes apply automatically");
        break;
    case AppPage::Diagnostics:
        lv_label_set_text_fmt(ui.status,
                              "Uptime %lu s | SD %s | %u cats",
                              static_cast<unsigned long>(now_ms / 1000U),
                              ui.sd.mounted ? "mounted" : "unavailable",
                              static_cast<unsigned>(ui.cats.size()));
        break;
    case AppPage::Overview:
        lv_label_set_text_fmt(ui.status,
                              "%u cats | %s | tap anywhere to open",
                              static_cast<unsigned>(ui.cats.size()),
                              sync_name);
        break;
    }
}

void update_timer(lv_timer_t *timer)
{
    auto *ui = static_cast<UiState *>(lv_timer_get_user_data(timer));
    update_ui(*ui);
}

void change_zoom(UiState &ui, int delta);
lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t colour);
void open_quick_settings(UiState &ui);
void close_quick_settings(UiState &ui, bool animate);

void gesture_timer(lv_timer_t *timer)
{
    auto *ui = static_cast<UiState *>(lv_timer_get_user_data(timer));
    if (ui != nullptr && guition_jc4880p443c_take_quick_settings_swipe()) {
        open_quick_settings(*ui);
    }
}

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
    const MapLayerInfo &layer = map_layer_info(ui.active_map_layer);
    const int next_zoom = std::clamp(
        static_cast<int>(ui.viewport.zoom()) + delta,
        static_cast<int>(layer.minimum_zoom),
        static_cast<int>(layer.maximum_zoom));
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
    const MapLayerInfo &layer = map_layer_info(ui->active_map_layer);
    const auto fit = bluepaws::map::fitPoints(points.data(),
                                              count,
                                              ui->viewport.width(),
                                              ui->viewport.height(),
                                              38,
                                              layer.minimum_zoom,
                                              layer.maximum_zoom);
    if (fit.valid) {
        ui->viewport = bluepaws::map::Viewport(
            ui->viewport.width(), ui->viewport.height(), fit.center, fit.zoom);
        ui->tiles_dirty = true;
        update_ui(*ui);
    }
}

void create_ui(UiState &ui);
void volume_fade_exec(void *object, int32_t opacity);

void brightness_fade_exec(void *object, int32_t opacity)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), static_cast<lv_opa_t>(opacity), 0);
}

void brightness_fade_completed(lv_anim_t *animation)
{
    auto *ui = static_cast<UiState *>(lv_anim_get_user_data(animation));
    auto *popup = static_cast<lv_obj_t *>(animation->var);
    if (ui == nullptr || popup == nullptr || ui->brightness_popup != popup) {
        return;
    }
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(popup, LV_OPA_COVER, 0);
}

void brightness_timeout(lv_timer_t *timer)
{
    auto *ui = static_cast<UiState *>(lv_timer_get_user_data(timer));
    lv_timer_pause(timer);
    if (ui == nullptr || ui->brightness_popup == nullptr ||
        lv_obj_has_flag(ui->brightness_popup, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    lv_anim_delete(ui->brightness_popup, brightness_fade_exec);
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, ui->brightness_popup);
    lv_anim_set_user_data(&fade, ui);
    lv_anim_set_exec_cb(&fade, brightness_fade_exec);
    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade, kBrightnessFadeMs);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&fade, brightness_fade_completed);
    lv_anim_start(&fade);
}

void reset_brightness_timeout(UiState &ui)
{
    if (ui.brightness_popup == nullptr) {
        return;
    }
    lv_anim_delete(ui.brightness_popup, brightness_fade_exec);
    lv_obj_set_style_opa(ui.brightness_popup, LV_OPA_COVER, 0);
    if (ui.brightness_hide_timer == nullptr) {
        ui.brightness_hide_timer =
            lv_timer_create(brightness_timeout, kBrightnessTimeoutMs, &ui);
    } else {
        lv_timer_set_period(ui.brightness_hide_timer, kBrightnessTimeoutMs);
        lv_timer_reset(ui.brightness_hide_timer);
        lv_timer_resume(ui.brightness_hide_timer);
    }
}

void hide_brightness_popup(UiState &ui)
{
    if (ui.brightness_hide_timer != nullptr) {
        lv_timer_pause(ui.brightness_hide_timer);
    }
    if (ui.brightness_popup != nullptr) {
        lv_anim_delete(ui.brightness_popup, brightness_fade_exec);
        lv_obj_set_style_opa(ui.brightness_popup, LV_OPA_COVER, 0);
        lv_obj_add_flag(ui.brightness_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

void brightness_activity(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        reset_brightness_timeout(*ui);
    }
}

void rebuild_current_page(void *user_data)
{
    auto *ui = static_cast<UiState *>(user_data);
    if (ui->brightness_hide_timer != nullptr) {
        lv_timer_delete(ui->brightness_hide_timer);
        ui->brightness_hide_timer = nullptr;
    }
    if (ui->brightness_popup != nullptr) {
        lv_anim_delete(ui->brightness_popup, brightness_fade_exec);
    }
    if (ui->volume_hide_timer != nullptr) {
        lv_timer_delete(ui->volume_hide_timer);
        ui->volume_hide_timer = nullptr;
    }
    if (ui->volume_popup != nullptr) {
        lv_anim_delete(ui->volume_popup, volume_fade_exec);
    }
    lv_obj_clean(lv_screen_active());
    create_ui(*ui);
    ui->screensaver_pending = false;
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

void brightness_changed(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (ui == nullptr || slider == nullptr) {
        return;
    }
    ui->brightness_percent = lv_slider_get_value(slider);
    ui->settings.brightness_percent = static_cast<uint8_t>(ui->brightness_percent);
    if (ui->brightness_label != nullptr) {
        lv_label_set_text_fmt(ui->brightness_label, "%d%%", ui->brightness_percent);
    }
    const esp_err_t error = guition_jc4880p443c_backlight_set(ui->brightness_percent);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Backlight update failed: %s", esp_err_to_name(error));
    }
    reset_brightness_timeout(*ui);
}

void brightness_released(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        bluepaws::settings_store::save(ui->settings);
        reset_brightness_timeout(*ui);
    }
}

void create_brightness_popup(UiState &ui)
{
    lv_obj_t *popup = lv_obj_create(lv_screen_active());
    lv_obj_set_size(popup, 78, 270);
    lv_obj_align(popup, LV_ALIGN_TOP_RIGHT, -12, 64);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(popup,
                              ui.dark_mode ? lv_color_hex(0x15232E) : lv_color_hex(0xE7E2D8),
                              0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_90, 0);
    lv_obj_set_style_border_color(popup,
                                  ui.dark_mode ? lv_color_hex(0x60788C) : lv_color_hex(0xAAA69E),
                                  0);
    lv_obj_set_style_border_width(popup, 1, 0);
    lv_obj_set_style_radius(popup, 12, 0);
    lv_obj_set_style_pad_all(popup, 10, 0);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(popup, brightness_activity, LV_EVENT_PRESSED, &ui);
    lv_obj_add_event_cb(popup, brightness_activity, LV_EVENT_PRESSING, &ui);

    lv_obj_t *icon = lv_image_create(popup);
    lv_image_set_src(icon, &bluepaws::ui::icon_brightness);
    lv_obj_set_style_image_recolor(
        icon, ui.dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x17324D), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);

    ui.brightness_slider = lv_slider_create(popup);
    lv_obj_set_size(ui.brightness_slider, 20, 172);
    lv_obj_align(ui.brightness_slider, LV_ALIGN_CENTER, 0, 2);
    lv_slider_set_range(ui.brightness_slider, 10, 100);
    lv_slider_set_value(ui.brightness_slider, ui.brightness_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui.brightness_slider, lv_color_hex(0x586873), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.brightness_slider, lv_color_hex(0x1E88D2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui.brightness_slider, lv_color_hex(0xF3F8FB), LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui.brightness_slider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(
        ui.brightness_slider, brightness_changed, LV_EVENT_VALUE_CHANGED, &ui);
    lv_obj_add_event_cb(
        ui.brightness_slider, brightness_activity, LV_EVENT_PRESSING, &ui);
    lv_obj_add_event_cb(
        ui.brightness_slider, brightness_released, LV_EVENT_RELEASED, &ui);

    ui.brightness_label = make_label(
        popup,
        "",
        ui.dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D));
    lv_label_set_text_fmt(ui.brightness_label, "%d%%", ui.brightness_percent);
    lv_obj_set_style_text_font(ui.brightness_label, &lv_font_montserrat_14, 0);
    lv_obj_align(ui.brightness_label, LV_ALIGN_BOTTOM_MID, 0, 0);

    ui.brightness_popup = popup;
    lv_obj_move_foreground(popup);
    reset_brightness_timeout(ui);
}

void brightness_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    close_quick_settings(*ui, true);
    if (ui->volume_popup != nullptr) {
        lv_obj_add_flag(ui->volume_popup, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui->brightness_popup == nullptr) {
        create_brightness_popup(*ui);
        return;
    }
    if (lv_obj_has_flag(ui->brightness_popup, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(ui->brightness_popup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui->brightness_popup);
        reset_brightness_timeout(*ui);
    } else {
        hide_brightness_popup(*ui);
    }
}

void volume_fade_exec(void *object, int32_t opacity)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), static_cast<lv_opa_t>(opacity), 0);
}

void volume_fade_completed(lv_anim_t *animation)
{
    auto *ui = static_cast<UiState *>(lv_anim_get_user_data(animation));
    auto *popup = static_cast<lv_obj_t *>(animation->var);
    if (ui == nullptr || popup == nullptr || ui->volume_popup != popup) return;
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(popup, LV_OPA_COVER, 0);
}

void volume_timeout(lv_timer_t *timer)
{
    auto *ui = static_cast<UiState *>(lv_timer_get_user_data(timer));
    lv_timer_pause(timer);
    if (ui == nullptr || ui->volume_popup == nullptr ||
        lv_obj_has_flag(ui->volume_popup, LV_OBJ_FLAG_HIDDEN)) return;
    lv_anim_delete(ui->volume_popup, volume_fade_exec);
    lv_anim_t fade{};
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, ui->volume_popup);
    lv_anim_set_user_data(&fade, ui);
    lv_anim_set_exec_cb(&fade, volume_fade_exec);
    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade, kBrightnessFadeMs);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&fade, volume_fade_completed);
    lv_anim_start(&fade);
}

void reset_volume_timeout(UiState &ui)
{
    if (ui.volume_popup == nullptr) return;
    lv_anim_delete(ui.volume_popup, volume_fade_exec);
    lv_obj_set_style_opa(ui.volume_popup, LV_OPA_COVER, 0);
    if (ui.volume_hide_timer == nullptr) {
        ui.volume_hide_timer = lv_timer_create(volume_timeout, kBrightnessTimeoutMs, &ui);
    } else {
        lv_timer_set_period(ui.volume_hide_timer, kBrightnessTimeoutMs);
        lv_timer_reset(ui.volume_hide_timer);
        lv_timer_resume(ui.volume_hide_timer);
    }
}

void hide_volume_popup(UiState &ui)
{
    if (ui.volume_hide_timer != nullptr) lv_timer_pause(ui.volume_hide_timer);
    if (ui.volume_popup != nullptr) {
        lv_anim_delete(ui.volume_popup, volume_fade_exec);
        lv_obj_set_style_opa(ui.volume_popup, LV_OPA_COVER, 0);
        lv_obj_add_flag(ui.volume_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

void volume_activity(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui != nullptr) reset_volume_timeout(*ui);
}

void volume_changed(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (ui == nullptr || slider == nullptr) return;
    ui->volume_percent = lv_slider_get_value(slider);
    ui->settings.volume_percent = static_cast<uint8_t>(ui->volume_percent);
    if (ui->volume_label != nullptr) {
        lv_label_set_text_fmt(ui->volume_label, "%d%%", ui->volume_percent);
    }
    reset_volume_timeout(*ui);
}

void volume_released(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        bluepaws::settings_store::save(ui->settings);
        reset_volume_timeout(*ui);
    }
}

void create_volume_popup(UiState &ui)
{
    lv_obj_t *popup = lv_obj_create(lv_screen_active());
    lv_obj_set_size(popup, 78, 270);
    lv_obj_align(popup, LV_ALIGN_TOP_RIGHT, -12, 64);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(popup,
                              ui.dark_mode ? lv_color_hex(0x15232E) : lv_color_hex(0xE7E2D8),
                              0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_90, 0);
    lv_obj_set_style_border_color(popup,
                                  ui.dark_mode ? lv_color_hex(0x60788C) : lv_color_hex(0xAAA69E),
                                  0);
    lv_obj_set_style_border_width(popup, 1, 0);
    lv_obj_set_style_radius(popup, 12, 0);
    lv_obj_set_style_pad_all(popup, 10, 0);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(popup, volume_activity, LV_EVENT_PRESSED, &ui);
    lv_obj_add_event_cb(popup, volume_activity, LV_EVENT_PRESSING, &ui);

    lv_obj_t *icon = make_label(
        popup, LV_SYMBOL_VOLUME_MAX,
        ui.dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x17324D));
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_22, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);

    ui.volume_slider = lv_slider_create(popup);
    lv_obj_set_size(ui.volume_slider, 20, 172);
    lv_obj_align(ui.volume_slider, LV_ALIGN_CENTER, 0, 2);
    lv_slider_set_range(ui.volume_slider, 0, 100);
    lv_slider_set_value(ui.volume_slider, ui.volume_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui.volume_slider, lv_color_hex(0x586873), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.volume_slider, lv_color_hex(0x1E88D2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui.volume_slider, lv_color_hex(0xF3F8FB), LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui.volume_slider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(ui.volume_slider, volume_changed, LV_EVENT_VALUE_CHANGED, &ui);
    lv_obj_add_event_cb(ui.volume_slider, volume_activity, LV_EVENT_PRESSING, &ui);
    lv_obj_add_event_cb(ui.volume_slider, volume_released, LV_EVENT_RELEASED, &ui);

    ui.volume_label = make_label(
        popup, "", ui.dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D));
    lv_label_set_text_fmt(ui.volume_label, "%d%%", ui.volume_percent);
    lv_obj_set_style_text_font(ui.volume_label, &lv_font_montserrat_14, 0);
    lv_obj_align(ui.volume_label, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui.volume_popup = popup;
    lv_obj_move_foreground(popup);
    reset_volume_timeout(ui);
}

void volume_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui == nullptr) return;
    close_quick_settings(*ui, true);
    hide_brightness_popup(*ui);
    if (ui->volume_popup == nullptr) {
        create_volume_popup(*ui);
    } else if (lv_obj_has_flag(ui->volume_popup, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(ui->volume_popup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui->volume_popup);
        reset_volume_timeout(*ui);
    } else {
        hide_volume_popup(*ui);
    }
}

void navigate_to(UiState &ui, AppPage page)
{
    ui.active_page = page;
    ui.screensaver_pending = false;
    ui.drawer_open = false;
    ui.layer_drawer_open = false;
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
    lv_obj_set_size(button, 54, 54);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x17324D), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_90, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 10, 0);
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
    lv_obj_set_size(button, 54, 54);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x10202E), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_80, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_pad_all(button, 10, 0);
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
    lv_obj_set_size(button, 54, 54);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x10202E), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_80, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, &ui);
    for (int32_t offset : {-10, 0, 10}) {
        lv_obj_t *bar = lv_obj_create(button);
        lv_obj_set_size(bar, 30, 4);
        lv_obj_align(bar, LV_ALIGN_CENTER, 0, offset);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
    return button;
}

void close_layer_drawer(UiState &ui)
{
    ui.layer_drawer_open = false;
    if (ui.layer_drawer != nullptr) {
        lv_anim_delete(ui.layer_drawer, nullptr);
        lv_obj_add_flag(ui.layer_drawer, LV_OBJ_FLAG_HIDDEN);
    }
}

void select_map_layer(UiState &ui, MapLayer layer)
{
    if (!map_layer_available(ui, layer)) {
        ESP_LOGW(kTag, "Map layer is not installed: %s", map_layer_info(layer).name);
        return;
    }

    const MapLayerInfo &info = map_layer_info(layer);
    ui.active_map_layer = layer;
    const uint8_t zoom = static_cast<uint8_t>(std::clamp(
        static_cast<int>(ui.viewport.zoom()),
        static_cast<int>(info.minimum_zoom),
        static_cast<int>(info.maximum_zoom)));
    ui.viewport.setZoom(zoom);
    invalidate_tile_cache(ui);
    close_layer_drawer(ui);
    ESP_LOGI(kTag, "Map layer changed to %s at z%u", info.name, static_cast<unsigned>(zoom));
    update_ui(ui);
}

void street_layer_clicked(lv_event_t *event)
{
    select_map_layer(*static_cast<UiState *>(lv_event_get_user_data(event)), MapLayer::Street);
}

void satellite_layer_clicked(lv_event_t *event)
{
    select_map_layer(*static_cast<UiState *>(lv_event_get_user_data(event)), MapLayer::Satellite);
}

void ordnance_survey_layer_clicked(lv_event_t *event)
{
    select_map_layer(*static_cast<UiState *>(lv_event_get_user_data(event)), MapLayer::OrdnanceSurvey);
}

void aerial_layer_clicked(lv_event_t *event)
{
    select_map_layer(*static_cast<UiState *>(lv_event_get_user_data(event)), MapLayer::Aerial);
}

lv_obj_t *make_layer_option(lv_obj_t *parent,
                            MapLayer layer,
                            lv_event_cb_t callback,
                            UiState &ui)
{
    const MapLayerInfo &info = map_layer_info(layer);
    const bool selected = ui.active_map_layer == layer;
    const bool available = map_layer_available(ui, layer);
    ESP_LOGI(kTag,
             "Map layer %s: %s (%s)",
             info.name,
             available ? "available" : "unavailable",
             info.tile_root);
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(100), 78);
    lv_obj_set_style_bg_color(
        button,
        selected ? lv_color_hex(0x176FA3)
                 : (ui.dark_mode ? lv_color_hex(0x1B2C39) : lv_color_hex(0xD8D3C9)),
        0);
    lv_obj_set_style_bg_opa(button, available ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_border_color(
        button, selected ? lv_color_hex(0x69C6F0) : lv_color_hex(0x60788C), 0);
    lv_obj_set_style_border_width(button, selected ? 2 : 1, 0);
    lv_obj_set_style_radius(button, 9, 0);
    lv_obj_set_style_pad_all(button, 10, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, &ui);
    if (!available) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }

    lv_obj_t *title = make_label(
        button, info.name, ui.dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x17324D));
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_t *description = make_label(
        button,
        available ? info.description
                  : (layer == MapLayer::Aerial
                         ? "Mixed pack rejected; one imagery source required"
                         : "Not installed on SD card"),
        ui.dark_mode ? lv_color_hex(0xC2D4DE) : lv_color_hex(0x41657A));
    lv_obj_set_pos(description, 0, 31);
    lv_obj_set_width(description, LV_PCT(100));
    lv_label_set_long_mode(description, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(description, &lv_font_montserrat_14, 0);
    if (selected) {
        lv_obj_t *active = make_label(button, "ACTIVE", lv_color_hex(0xFFFFFF));
        lv_obj_align(active, LV_ALIGN_TOP_RIGHT, 0, 2);
        lv_obj_set_style_text_font(active, &lv_font_montserrat_14, 0);
    }
    return button;
}

void drawer_open_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->map_drawer == nullptr) {
        return;
    }
    close_layer_drawer(*ui);
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

void layer_drawer_open_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->layer_drawer == nullptr) {
        return;
    }
    if (ui->map_drawer != nullptr) {
        ui->drawer_open = false;
        lv_obj_add_flag(ui->map_drawer, LV_OBJ_FLAG_HIDDEN);
    }
    ui->layer_drawer_open = true;
    lv_obj_remove_flag(ui->layer_drawer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->layer_drawer);
    const int32_t panel_width = current_layout(*ui).map_panel_width;
    const int32_t drawer_width = lv_obj_get_width(ui->layer_drawer);
    lv_obj_set_x(ui->layer_drawer, panel_width);
    lv_anim_t animation{};
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, ui->layer_drawer);
    lv_anim_set_values(&animation, panel_width, panel_width - drawer_width);
    lv_anim_set_duration(&animation, 200);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&animation, [](void *object, int32_t value) {
        lv_obj_set_x(static_cast<lv_obj_t *>(object), value);
    });
    lv_anim_start(&animation);
    ESP_LOGI(kTag, "Map layer drawer opened");
}

void layer_drawer_close_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        close_layer_drawer(*ui);
        ESP_LOGI(kTag, "Map layer drawer closed");
    }
}

bluepaws::ui::PageActions page_actions(UiState &ui, bool show_home)
{
    return {
        .home = show_home ? launcher_clicked : nullptr,
        .rotate = orientation_clicked,
        .theme = theme_clicked,
        .brightness = brightness_clicked,
        .user_data = &ui,
    };
}

void quick_settings_y_exec(void *object, int32_t y)
{
    lv_obj_set_y(static_cast<lv_obj_t *>(object), y);
}

void quick_settings_close_completed(lv_anim_t *animation)
{
    auto *ui = static_cast<UiState *>(lv_anim_get_user_data(animation));
    auto *tray = static_cast<lv_obj_t *>(animation->var);
    if (ui == nullptr || tray == nullptr || ui->quick_settings_tray != tray ||
        ui->quick_settings_open) {
        return;
    }
    lv_obj_add_flag(tray, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *make_quick_setting(lv_obj_t *parent,
                             const lv_image_dsc_t &icon,
                             const char *label_text,
                             UiState &ui,
                             lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, ui.portrait ? 84 : 108, 78);
    lv_obj_set_style_bg_color(button,
                              ui.dark_mode ? lv_color_hex(0x243746) : lv_color_hex(0xDDD8CF),
                              0);
    lv_obj_set_style_bg_opa(button, LV_OPA_80, 0);
    lv_obj_set_style_border_color(button,
                                  ui.dark_mode ? lv_color_hex(0x60788C) : lv_color_hex(0xAAA69E),
                                  0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_pad_all(button, 5, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, &ui);

    lv_obj_t *image = lv_image_create(button);
    lv_image_set_src(image, &icon);
    lv_obj_set_style_image_recolor(
        image, ui.dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x17324D), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *label = make_label(
        button,
        label_text,
        ui.dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

lv_obj_t *make_quick_setting_symbol(lv_obj_t *parent,
                                    const char *symbol,
                                    const char *label_text,
                                    UiState &ui,
                                    lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, ui.portrait ? 84 : 108, 78);
    lv_obj_set_style_bg_color(button,
                              ui.dark_mode ? lv_color_hex(0x243746) : lv_color_hex(0xDDD8CF),
                              0);
    lv_obj_set_style_bg_opa(button, LV_OPA_80, 0);
    lv_obj_set_style_border_color(button,
                                  ui.dark_mode ? lv_color_hex(0x60788C) : lv_color_hex(0xAAA69E),
                                  0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_pad_all(button, 5, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, &ui);
    lv_obj_t *icon = make_label(
        button, symbol, ui.dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x17324D));
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_22, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *label = make_label(
        button, label_text,
        ui.dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

void quick_settings_handle_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    if (ui->quick_settings_open) {
        close_quick_settings(*ui, true);
    } else {
        open_quick_settings(*ui);
    }
}

void quick_settings_close_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        close_quick_settings(*ui, true);
    }
}

void create_quick_settings_tray(UiState &ui)
{
    lv_obj_t *screen = lv_screen_active();
    const int32_t tray_height = ui.portrait ? 164 : 132;
    lv_obj_t *tray = lv_obj_create(screen);
    lv_obj_set_size(tray, LV_PCT(100), tray_height);
    lv_obj_set_pos(tray, 0, -tray_height);
    lv_obj_add_flag(tray, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(tray, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(
        tray, ui.dark_mode ? lv_color_hex(0x101B25) : lv_color_hex(0xD8D3C9), 0);
    lv_obj_set_style_bg_opa(tray, LV_OPA_90, 0);
    lv_obj_set_style_border_color(
        tray, ui.dark_mode ? lv_color_hex(0x4E7187) : lv_color_hex(0xAAA69E), 0);
    lv_obj_set_style_border_width(tray, 1, 0);
    lv_obj_set_style_border_side(tray, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(tray, 0, 0);
    lv_obj_set_style_pad_all(tray, 8, 0);
    lv_obj_remove_flag(tray, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(
        tray,
        "Quick controls",
        ui.dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 0);

    lv_obj_t *close = lv_button_create(tray);
    lv_obj_set_size(close, 46, 28);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -3);
    lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(close, 0, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_add_event_cb(close, quick_settings_close_clicked, LV_EVENT_CLICKED, &ui);
    lv_obj_t *chevron = make_label(
        close,
        LV_SYMBOL_UP,
        ui.dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D));
    lv_obj_center(chevron);

    lv_obj_t *controls = lv_obj_create(tray);
    lv_obj_set_size(controls, LV_PCT(100), 84);
    lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    lv_obj_remove_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

    make_quick_setting(controls, bluepaws::ui::icon_home, "Home", ui, launcher_clicked);
    make_quick_setting(
        controls, bluepaws::ui::icon_brightness, "Brightness", ui, brightness_clicked);
    make_quick_setting_symbol(controls, LV_SYMBOL_VOLUME_MAX, "Volume", ui, volume_clicked);
    make_quick_setting(
        controls, bluepaws::ui::icon_rotate, "Orientation", ui, orientation_clicked);
    make_quick_setting(controls,
                       bluepaws::ui::icon_night_mode,
                       ui.dark_mode ? "Light mode" : "Dark mode",
                       ui,
                       theme_clicked);

    lv_obj_t *handle = lv_button_create(screen);
    lv_obj_set_size(handle, 72, 10);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 1);
    lv_obj_add_flag(handle, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(
        handle, ui.dark_mode ? lv_color_hex(0x80C9F2) : lv_color_hex(0x28709A), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_60, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_set_style_radius(handle, 5, 0);
    lv_obj_set_style_pad_all(handle, 0, 0);
    lv_obj_set_style_shadow_width(handle, 0, 0);
    lv_obj_add_event_cb(handle, quick_settings_handle_clicked, LV_EVENT_CLICKED, &ui);

    ui.quick_settings_tray = tray;
    ui.quick_settings_handle = handle;
    ui.quick_settings_open = false;
    lv_obj_move_foreground(handle);
}

void open_quick_settings(UiState &ui)
{
    if (ui.quick_settings_tray == nullptr || ui.quick_settings_open) {
        return;
    }
    hide_brightness_popup(ui);
    hide_volume_popup(ui);
    ui.quick_settings_open = true;
    lv_obj_remove_flag(ui.quick_settings_tray, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui.quick_settings_tray);
    const int32_t height = lv_obj_get_height(ui.quick_settings_tray);
    lv_anim_delete(ui.quick_settings_tray, quick_settings_y_exec);
    lv_obj_set_y(ui.quick_settings_tray, -height);
    lv_anim_t animation{};
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, ui.quick_settings_tray);
    lv_anim_set_values(&animation, -height, 0);
    lv_anim_set_duration(&animation, 220);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&animation, quick_settings_y_exec);
    lv_anim_start(&animation);
    ESP_LOGI(kTag, "Quick settings opened");
}

void close_quick_settings(UiState &ui, bool animate)
{
    if (ui.quick_settings_tray == nullptr || !ui.quick_settings_open) {
        return;
    }
    ui.quick_settings_open = false;
    lv_anim_delete(ui.quick_settings_tray, quick_settings_y_exec);
    if (!animate) {
        lv_obj_add_flag(ui.quick_settings_tray, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const int32_t height = lv_obj_get_height(ui.quick_settings_tray);
    lv_anim_t animation{};
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, ui.quick_settings_tray);
    lv_anim_set_user_data(&animation, &ui);
    lv_anim_set_values(&animation, lv_obj_get_y(ui.quick_settings_tray), -height);
    lv_anim_set_duration(&animation, 180);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&animation, quick_settings_y_exec);
    lv_anim_set_completed_cb(&animation, quick_settings_close_completed);
    lv_anim_start(&animation);
    ESP_LOGI(kTag, "Quick settings closed");
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

void drawer_cat_card_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    auto *target = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    if (ui == nullptr || target == nullptr) {
        return;
    }
    for (size_t i = 0; i < ui->drawer_cards.size(); ++i) {
        const bool was_expanded = ui->drawer_card_expanded[i];
        const bool expand = ui->drawer_cards[i] == target && !was_expanded;
        ui->drawer_card_expanded[i] = expand;
        if (ui->drawer_cards[i] != nullptr) {
            lv_obj_set_height(ui->drawer_cards[i], expand ? 402 : 140);
        }
        if (ui->drawer_expanded_panels[i] != nullptr) {
            if (expand) {
                lv_obj_remove_flag(ui->drawer_expanded_panels[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(ui->drawer_expanded_panels[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

size_t drawer_action_index(const std::array<lv_obj_t *, bluepaws::kMaximumCats> &buttons,
                           lv_obj_t *target)
{
    for (size_t i = 0; i < buttons.size(); ++i) {
        if (buttons[i] == target) {
            return i;
        }
    }
    return buttons.size();
}

void drawer_jump_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    auto *target = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    if (ui == nullptr) return;
    const size_t index = drawer_action_index(ui->drawer_jump_buttons, target);
    const bluepaws::CatRecord *cat = index < ui->cats.size() ? ui->cats.at(index) : nullptr;
    if (cat == nullptr || !cat->has_position) return;
    ui->viewport.setCenter({static_cast<double>(cat->last_valid_latitude_e7) / 1.0e7,
                            static_cast<double>(cat->last_valid_longitude_e7) / 1.0e7});
    ui->tiles_dirty = true;
    ui->drawer_open = false;
    if (ui->map_drawer != nullptr) lv_obj_add_flag(ui->map_drawer, LV_OBJ_FLAG_HIDDEN);
    update_ui(*ui);
}

void drawer_follow_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    auto *target = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    if (ui == nullptr) return;
    const size_t index = drawer_action_index(ui->drawer_follow_buttons, target);
    if (index >= ui->drawer_follow_buttons.size()) return;
    ui->followed_cat = ui->followed_cat == static_cast<int>(index) ? -1 : static_cast<int>(index);
    for (size_t i = 0; i < ui->drawer_follow_buttons.size(); ++i) {
        if (ui->drawer_follow_buttons[i] == nullptr) continue;
        lv_obj_t *label = lv_obj_get_child(ui->drawer_follow_buttons[i], 0);
        lv_label_set_text(label, ui->followed_cat == static_cast<int>(i) ? "FOLLOWING" : "FOLLOW");
    }
    if (ui->drawer_message_labels[index] != nullptr) {
        lv_label_set_text(ui->drawer_message_labels[index],
                          ui->followed_cat == static_cast<int>(index)
                              ? "Live map now follows this collar."
                              : "Live following stopped.");
    }
}

void drawer_trail_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    auto *target = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    if (ui == nullptr) return;
    const size_t index = drawer_action_index(ui->drawer_trail_buttons, target);
    if (index >= ui->trail_enabled.size()) return;
    ui->trail_enabled[index] = !ui->trail_enabled[index];
    lv_obj_t *label = lv_obj_get_child(target, 0);
    lv_label_set_text(label, ui->trail_enabled[index] ? "TRAIL ON" : "TRAIL");
    if (ui->drawer_message_labels[index] != nullptr) {
        lv_label_set_text(ui->drawer_message_labels[index],
                          ui->trail_enabled[index]
                              ? "Trail capture armed; history rendering follows telemetry storage."
                              : "Trail capture disabled.");
    }
}

void drawer_command_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    auto *target = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    if (ui == nullptr) return;
    const size_t index = drawer_action_index(ui->drawer_command_buttons, target);
    if (index < ui->drawer_message_labels.size() && ui->drawer_message_labels[index] != nullptr) {
        lv_label_set_text(ui->drawer_message_labels[index],
                          "Command panel ready; C6/LoRa transport is not connected yet.");
    }
}

lv_obj_t *make_drawer_action(lv_obj_t *parent, const char *text, lv_event_cb_t callback, UiState &ui)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 70, 38);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x173B52), 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x3C718D), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 7, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, &ui);
    lv_obj_t *label = make_label(button, text, lv_color_hex(0xD8E6ED));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
    return button;
}

lv_obj_t *make_drawer_row(lv_obj_t *parent, int32_t height, int32_t gap)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, gap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

lv_obj_t *make_drawer_image(lv_obj_t *parent, const lv_image_dsc_t &source)
{
    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, &source);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    return image;
}

void create_drawer_cat_card(lv_obj_t *parent, size_t index, UiState &ui)
{
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, 140);
    lv_obj_set_style_bg_color(
        card, ui.dark_mode ? lv_color_hex(0x172733) : lv_color_hex(0xD9D5CC), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(
        card, ui.dark_mode ? lv_color_hex(0x405B6D) : lv_color_hex(0xA8B7C0), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 9, 0);
    lv_obj_set_style_pad_gap(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(card, drawer_cat_card_clicked, LV_EVENT_CLICKED, &ui);

    const lv_color_t primary_text =
        ui.dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D);
    const lv_color_t secondary_text =
        ui.dark_mode ? lv_color_hex(0xAFC3CE) : lv_color_hex(0x456578);

    lv_obj_t *header = make_drawer_row(card, 44, 4);
    lv_obj_t *avatar = lv_obj_create(header);
    lv_obj_set_size(avatar, 42, 42);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(kMarkerColours[index]), 0);
    lv_obj_set_style_border_color(avatar, lv_color_hex(0xD3E5ED), 0);
    lv_obj_set_style_border_width(avatar, 2, 0);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(avatar, 0, 0);
    lv_obj_remove_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(avatar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *avatar_number = make_label(avatar, "", lv_color_hex(0xFFFFFF));
    lv_label_set_text_fmt(avatar_number, "%u", static_cast<unsigned>(index + 1));
    lv_obj_set_style_text_font(avatar_number, &lv_font_montserrat_14, 0);
    lv_obj_center(avatar_number);

    lv_obj_t *name = make_label(header, "Waiting", primary_text);
    lv_obj_set_width(name, 100);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *status = make_drawer_image(header, bluepaws::ui::icon_status_out);
    lv_obj_t *profile = make_drawer_image(header, bluepaws::ui::icon_profile_powersave);
    lv_image_set_scale(status, 307);
    lv_image_set_scale(profile, 307);

    lv_obj_t *fault_row = make_drawer_row(card, 20, 4);
    lv_obj_t *fault = make_drawer_image(fault_row, bluepaws::ui::icon_status_error);

    lv_obj_t *telemetry = make_drawer_row(card, 22, 4);
    lv_obj_t *battery = make_drawer_image(telemetry, bluepaws::ui::icon_battery_full);
    lv_obj_t *battery_text = make_label(telemetry, "--%", secondary_text);
    lv_obj_set_width(battery_text, 34);
    lv_obj_set_style_text_font(battery_text, &lv_font_montserrat_14, 0);
    make_drawer_image(telemetry, bluepaws::ui::icon_radio_antenna);
    lv_obj_t *signal = make_drawer_image(telemetry, bluepaws::ui::icon_signal_full);
    lv_obj_t *signal_text = make_label(telemetry, "--", lv_color_hex(0x31B988));
    lv_obj_set_width(signal_text, 82);
    lv_label_set_long_mode(signal_text, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(signal_text, &lv_font_montserrat_14, 0);
    lv_obj_t *radio = make_drawer_image(telemetry, bluepaws::ui::icon_radio_rf);

    lv_obj_t *meta = make_drawer_row(card, 18, 5);
    make_drawer_image(meta, bluepaws::ui::icon_status_home_small);
    lv_obj_t *distance = make_label(meta, "--m", secondary_text);
    lv_obj_set_width(distance, 54);
    lv_obj_set_style_text_font(distance, &lv_font_montserrat_14, 0);
    make_drawer_image(meta, bluepaws::ui::icon_status_stopwatch);
    lv_obj_t *age = make_label(meta, "--s", secondary_text);
    lv_obj_set_width(age, 52);
    lv_obj_set_style_text_font(age, &lv_font_montserrat_14, 0);

    lv_obj_t *expanded = lv_obj_create(card);
    lv_obj_set_width(expanded, LV_PCT(100));
    lv_obj_set_height(expanded, 230);
    lv_obj_set_style_bg_opa(expanded, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(expanded, 0, 0);
    lv_obj_set_style_pad_all(expanded, 0, 0);
    lv_obj_set_style_pad_gap(expanded, 6, 0);
    lv_obj_set_flex_flow(expanded, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(expanded, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(expanded, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *detail = make_label(
        expanded,
        "",
        ui.dark_mode ? lv_color_hex(0xB9D0DC) : lv_color_hex(0x36596D));
    lv_obj_set_width(detail, LV_PCT(100));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_14, 0);
    lv_obj_remove_flag(detail, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *actions = lv_obj_create(expanded);
    lv_obj_set_size(actions, LV_PCT(100), 42);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_gap(actions, 5, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    ui.drawer_jump_buttons[index] = make_drawer_action(actions, "JUMP", drawer_jump_clicked, ui);
    ui.drawer_follow_buttons[index] = make_drawer_action(actions, "FOLLOW", drawer_follow_clicked, ui);
    ui.drawer_trail_buttons[index] = make_drawer_action(actions, "TRAIL", drawer_trail_clicked, ui);
    ui.drawer_command_buttons[index] = make_drawer_action(actions, "CMD", drawer_command_clicked, ui);

    lv_obj_t *message = make_label(
        expanded,
        "MESSAGE LOG  No messages",
        ui.dark_mode ? lv_color_hex(0x8EABB9) : lv_color_hex(0x496B7C));
    lv_obj_set_width(message, LV_PCT(100));
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(message, &lv_font_montserrat_14, 0);

    ui.drawer_cards[index] = card;
    ui.drawer_summary_labels[index] = nullptr;
    ui.drawer_name_labels[index] = name;
    ui.drawer_status_images[index] = status;
    ui.drawer_profile_images[index] = profile;
    ui.drawer_fault_images[index] = fault;
    ui.drawer_battery_images[index] = battery;
    ui.drawer_battery_labels[index] = battery_text;
    ui.drawer_signal_images[index] = signal;
    ui.drawer_signal_labels[index] = signal_text;
    ui.drawer_radio_images[index] = radio;
    ui.drawer_distance_labels[index] = distance;
    ui.drawer_age_labels[index] = age;
    ui.drawer_detail_labels[index] = detail;
    ui.drawer_expanded_panels[index] = expanded;
    ui.drawer_message_labels[index] = message;
    ui.drawer_card_expanded[index] = false;
}

void create_map_page(UiState &ui)
{
    const UiLayout &layout = current_layout(ui);
    ui.tiles_dirty = true;
    ui.tile_images_bound = false;
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
        if (i == 7) {
            lv_obj_t *home = lv_image_create(marker);
            lv_image_set_src(home, &bluepaws::ui::icon_status_home_small);
            lv_obj_remove_flag(home, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_center(home);
        } else {
            lv_obj_t *number = make_label(marker, "", lv_color_hex(0xFFFFFF));
            lv_label_set_text_fmt(number, "%u", static_cast<unsigned>(i + 1));
            lv_obj_center(number);
        }
        ui.markers[i] = marker;
    }

    constexpr int32_t control_inset = 14;
    constexpr int32_t control_size = 54;
    constexpr int32_t control_gap = 10;
    make_hamburger_control(
        ui.map_view, control_inset, control_inset, drawer_open_clicked, ui);
    make_map_control(
        ui.map_view,
        layout.map_width - control_inset - control_size,
        control_inset,
        "MAP",
        layer_drawer_open_clicked,
        ui);
    make_map_control(ui.map_view,
                     control_inset,
                     control_inset + control_size + control_gap,
                     "HOME",
                     home_clicked,
                     ui);
    make_map_control(ui.map_view,
                     control_inset,
                     control_inset + (control_size + control_gap) * 2,
                     "FIT",
                     fit_all_clicked,
                     ui);
    make_map_icon_control(ui.map_view,
                          control_inset,
                          layout.map_height - control_inset - control_size * 2 - control_gap,
                          bluepaws::ui::icon_zoom_in,
                          zoom_in_clicked,
                          ui);
    make_map_icon_control(ui.map_view,
                          control_inset,
                          layout.map_height - control_inset - control_size,
                          bluepaws::ui::icon_zoom_out,
                          zoom_out_clicked,
                          ui);

    const int32_t drawer_width = ui.portrait ? 440 : 430;
    ui.map_drawer = lv_obj_create(map_panel);
    lv_obj_set_pos(ui.map_drawer, 0, 0);
    lv_obj_set_size(ui.map_drawer, drawer_width, layout.map_height);
    lv_obj_set_style_bg_color(ui.map_drawer,
                              ui.dark_mode ? lv_color_hex(0x101B25) : lv_color_hex(0xE7E2D8),
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
    lv_obj_set_size(drawer_header, LV_PCT(100), 48);
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
    lv_obj_set_size(close_button, 46, 46);
    lv_obj_align(close_button, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(close_button, lv_color_hex(0x2B5878), 0);
    lv_obj_set_style_bg_opa(close_button, LV_OPA_70, 0);
    lv_obj_set_style_radius(close_button, 9, 0);
    lv_obj_set_style_pad_all(close_button, 0, 0);
    lv_obj_add_event_cb(close_button, drawer_close_clicked, LV_EVENT_CLICKED, &ui);
    lv_obj_t *close_label = make_label(close_button, "X", lv_color_hex(0xFFFFFF));
    lv_obj_center(close_label);

    lv_obj_t *drawer_scroll = lv_obj_create(ui.map_drawer);
    lv_obj_set_width(drawer_scroll, LV_PCT(100));
    lv_obj_set_height(drawer_scroll, 0);
    lv_obj_set_flex_grow(drawer_scroll, 1);
    lv_obj_set_flex_flow(drawer_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(drawer_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drawer_scroll, 0, 0);
    lv_obj_set_style_radius(drawer_scroll, 0, 0);
    lv_obj_set_style_pad_all(drawer_scroll, 0, 0);
    lv_obj_set_style_pad_gap(drawer_scroll, 8, 0);
    lv_obj_set_scroll_dir(drawer_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(drawer_scroll, LV_SCROLLBAR_MODE_AUTO);
    for (size_t i = 0; i < bluepaws::kMaximumCats; ++i) {
        create_drawer_cat_card(drawer_scroll, i, ui);
    }
    if (!ui.drawer_open) {
        lv_obj_add_flag(ui.map_drawer, LV_OBJ_FLAG_HIDDEN);
    }

    const int32_t layer_drawer_width = ui.portrait ? 400 : 340;
    ui.layer_drawer = lv_obj_create(map_panel);
    lv_obj_set_pos(ui.layer_drawer, layout.map_panel_width - layer_drawer_width, 0);
    lv_obj_set_size(ui.layer_drawer, layer_drawer_width, layout.map_height);
    lv_obj_set_style_bg_color(ui.layer_drawer,
                              ui.dark_mode ? lv_color_hex(0x101B25) : lv_color_hex(0xE7E2D8),
                              0);
    lv_obj_set_style_bg_opa(ui.layer_drawer, LV_OPA_90, 0);
    lv_obj_set_style_border_color(ui.layer_drawer,
                                  ui.dark_mode ? lv_color_hex(0x486274)
                                               : lv_color_hex(0xAFC3D1),
                                  0);
    lv_obj_set_style_border_width(ui.layer_drawer, 1, 0);
    lv_obj_set_style_border_side(ui.layer_drawer, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(ui.layer_drawer, 0, 0);
    lv_obj_set_style_pad_all(ui.layer_drawer, 14, 0);
    lv_obj_set_style_pad_gap(ui.layer_drawer, 8, 0);
    lv_obj_set_flex_flow(ui.layer_drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(ui.layer_drawer, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.layer_drawer, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *layer_header = lv_obj_create(ui.layer_drawer);
    lv_obj_set_size(layer_header, LV_PCT(100), 44);
    lv_obj_set_style_bg_opa(layer_header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layer_header, 0, 0);
    lv_obj_set_style_pad_all(layer_header, 0, 0);
    lv_obj_remove_flag(layer_header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *layer_title = make_label(
        layer_header,
        "Map layer",
        ui.dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x17324D));
    lv_obj_set_pos(layer_title, 0, 7);
    lv_obj_set_style_text_font(layer_title, &lv_font_montserrat_18, 0);
    lv_obj_t *layer_close = lv_button_create(layer_header);
    lv_obj_set_size(layer_close, 46, 46);
    lv_obj_align(layer_close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(layer_close, lv_color_hex(0x2B5878), 0);
    lv_obj_set_style_bg_opa(layer_close, LV_OPA_70, 0);
    lv_obj_set_style_radius(layer_close, 9, 0);
    lv_obj_set_style_pad_all(layer_close, 0, 0);
    lv_obj_add_event_cb(layer_close, layer_drawer_close_clicked, LV_EVENT_CLICKED, &ui);
    lv_obj_t *layer_close_label = make_label(layer_close, "X", lv_color_hex(0xFFFFFF));
    lv_obj_center(layer_close_label);

    make_layer_option(ui.layer_drawer, MapLayer::Street, street_layer_clicked, ui);
    make_layer_option(
        ui.layer_drawer, MapLayer::OrdnanceSurvey, ordnance_survey_layer_clicked, ui);
    make_layer_option(ui.layer_drawer, MapLayer::Satellite, satellite_layer_clicked, ui);
    make_layer_option(ui.layer_drawer, MapLayer::Aerial, aerial_layer_clicked, ui);
    lv_obj_t *layer_note = make_label(
        ui.layer_drawer,
        "Mixed-source aerial imagery is disabled. Install a consistent aerial pack to enable it.",
        ui.dark_mode ? lv_color_hex(0x9DB3C0) : lv_color_hex(0x41657A));
    lv_obj_set_width(layer_note, LV_PCT(100));
    lv_label_set_long_mode(layer_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(layer_note, &lv_font_montserrat_14, 0);
    if (!ui.layer_drawer_open) {
        lv_obj_add_flag(ui.layer_drawer, LV_OBJ_FLAG_HIDDEN);
    }
}

void style_card(lv_obj_t *card, bool dark_mode)
{
    lv_obj_set_style_bg_color(card,
                              dark_mode ? lv_color_hex(0x15232E) : lv_color_hex(0xE7E2D8),
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

void overview_wake_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui == nullptr) return;
    lv_display_trigger_activity(ui->display);
    ui->screen_off = false;
    ui->screen_dimmed = false;
    guition_jc4880p443c_backlight_set(ui->brightness_percent);
    navigate_to(*ui, AppPage::Launcher);
}

void create_overview_page(UiState &ui)
{
    lv_obj_t *content = bluepaws::ui::create_page_frame(
        lv_screen_active(),
        "Home Hub overview",
        "Tap anywhere to open BluePaws",
        true,
        {},
        &ui.status);
    lv_obj_set_flex_flow(content, ui.portrait ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x050A0F), 0);
    lv_obj_add_event_cb(lv_screen_active(), overview_wake_clicked, LV_EVENT_PRESSED, &ui);
    lv_obj_add_event_cb(content, overview_wake_clicked, LV_EVENT_PRESSED, &ui);

    const int32_t radar_width = ui.portrait ? 440 : 430;
    const int32_t radar_height = ui.portrait ? 440 : 390;
    lv_obj_t *radar = lv_obj_create(content);
    lv_obj_set_size(radar, radar_width, radar_height);
    lv_obj_set_style_bg_color(radar, lv_color_hex(0x07131A), 0);
    lv_obj_set_style_bg_opa(radar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(radar, lv_color_hex(0x17465D), 0);
    lv_obj_set_style_border_width(radar, 2, 0);
    lv_obj_set_style_radius(radar, 18, 0);
    lv_obj_set_style_pad_all(radar, 0, 0);
    lv_obj_remove_flag(radar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(radar, LV_OBJ_FLAG_CLICKABLE);
    const int32_t ring_sizes[] = {330, 240, 150};
    for (int32_t size : ring_sizes) {
        lv_obj_t *ring = lv_obj_create(radar);
        lv_obj_set_size(ring, size, size);
        lv_obj_center(ring);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(0x1C6C83), 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_50, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(ring, 0, 0);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *cross_h = lv_obj_create(radar);
    lv_obj_set_size(cross_h, 330, 1);
    lv_obj_center(cross_h);
    lv_obj_set_style_bg_color(cross_h, lv_color_hex(0x1C6C83), 0);
    lv_obj_set_style_border_width(cross_h, 0, 0);
    lv_obj_t *cross_v = lv_obj_create(radar);
    lv_obj_set_size(cross_v, 1, 330);
    lv_obj_center(cross_v);
    lv_obj_set_style_bg_color(cross_v, lv_color_hex(0x1C6C83), 0);
    lv_obj_set_style_border_width(cross_v, 0, 0);

    lv_obj_t *hub = lv_obj_create(radar);
    lv_obj_set_size(hub, 54, 54);
    lv_obj_center(hub);
    lv_obj_set_style_bg_color(hub, lv_color_hex(0x1E88D2), 0);
    lv_obj_set_style_border_color(hub, lv_color_hex(0xBDE8FF), 0);
    lv_obj_set_style_border_width(hub, 3, 0);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_color(hub, lv_color_hex(0x27C7F7), 0);
    lv_obj_set_style_shadow_width(hub, 18, 0);
    lv_obj_set_style_shadow_opa(hub, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(hub, 0, 0);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *hub_label = make_label(hub, LV_SYMBOL_HOME, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_font(hub_label, &lv_font_montserrat_22, 0);
    lv_obj_center(hub_label);

    for (size_t i = 0; i < ui.overview_markers.size(); ++i) {
        lv_obj_t *marker = lv_obj_create(radar);
        lv_obj_set_size(marker, 34, 34);
        lv_obj_set_style_bg_color(marker, lv_color_hex(kMarkerColours[i]), 0);
        lv_obj_set_style_border_color(marker, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(marker, 2, 0);
        lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(marker, 0, 0);
        lv_obj_remove_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *number = make_label(marker, "", lv_color_hex(0xFFFFFF));
        lv_label_set_text_fmt(number, "%u", static_cast<unsigned>(i + 1));
        lv_obj_set_style_text_font(number, &lv_font_montserrat_14, 0);
        lv_obj_center(number);
        lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
        ui.overview_markers[i] = marker;
    }

    lv_obj_t *summary = lv_obj_create(content);
    lv_obj_set_size(summary, ui.portrait ? 440 : 330, ui.portrait ? 250 : 390);
    lv_obj_set_style_bg_color(summary, lv_color_hex(0x0C1820), 0);
    lv_obj_set_style_border_color(summary, lv_color_hex(0x17465D), 0);
    lv_obj_set_style_border_width(summary, 1, 0);
    lv_obj_set_style_radius(summary, 14, 0);
    lv_obj_set_style_pad_all(summary, 10, 0);
    lv_obj_set_style_pad_gap(summary, 5, 0);
    lv_obj_set_flex_flow(summary, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(summary, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(summary, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_t *summary_title = make_label(summary, "Last known positions", lv_color_hex(0x80C9F2));
    lv_obj_set_style_text_font(summary_title, &lv_font_montserrat_18, 0);
    for (size_t i = 0; i < ui.overview_labels.size(); ++i) {
        lv_obj_t *label = make_label(summary, "Waiting for a collar report...", lv_color_hex(0xE7F4FA));
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_bg_color(label, lv_color_hex(0x102733), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
        lv_obj_set_style_border_color(label, lv_color_hex(kMarkerColours[i]), 0);
        lv_obj_set_style_border_width(label, 2, 0);
        lv_obj_set_style_border_side(label, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_pad_all(label, 7, 0);
        ui.overview_labels[i] = label;
    }
}

const char *settings_field_title(SettingsField field)
{
    switch (field) {
    case SettingsField::PrimarySsid: return "Primary Wi-Fi name";
    case SettingsField::PrimaryPassword: return "Primary Wi-Fi password";
    case SettingsField::SecondarySsid: return "Secondary Wi-Fi name";
    case SettingsField::SecondaryPassword: return "Secondary Wi-Fi password";
    case SettingsField::AccessPointSsid: return "Off-grid local network name";
    case SettingsField::AccessPointPassword: return "Off-grid local network password";
    case SettingsField::OverviewTimeout: return "Overview timeout (seconds)";
    case SettingsField::DimTimeout: return "Dim timeout (seconds)";
    case SettingsField::ScreenOffTimeout: return "Screen-off timeout (seconds)";
    case SettingsField::DimBrightness: return "Dim brightness (percent)";
    }
    return "Setting";
}

bool settings_field_password(SettingsField field)
{
    return field == SettingsField::PrimaryPassword ||
           field == SettingsField::SecondaryPassword ||
           field == SettingsField::AccessPointPassword;
}

bool settings_field_numeric(SettingsField field)
{
    return field == SettingsField::OverviewTimeout ||
           field == SettingsField::DimTimeout ||
           field == SettingsField::ScreenOffTimeout ||
           field == SettingsField::DimBrightness;
}

const char *settings_field_value(UiState &ui, SettingsField field, char *buffer, std::size_t size)
{
    switch (field) {
    case SettingsField::PrimarySsid: return ui.settings.primary.ssid;
    case SettingsField::PrimaryPassword: return ui.settings.primary.password;
    case SettingsField::SecondarySsid: return ui.settings.secondary.ssid;
    case SettingsField::SecondaryPassword: return ui.settings.secondary.password;
    case SettingsField::AccessPointSsid: return ui.settings.access_point_ssid;
    case SettingsField::AccessPointPassword: return ui.settings.access_point_password;
    case SettingsField::OverviewTimeout:
        std::snprintf(buffer, size, "%u", ui.settings.overview_timeout_seconds); break;
    case SettingsField::DimTimeout:
        std::snprintf(buffer, size, "%u", ui.settings.dim_timeout_seconds); break;
    case SettingsField::ScreenOffTimeout:
        std::snprintf(buffer, size, "%u", ui.settings.screen_off_timeout_seconds); break;
    case SettingsField::DimBrightness:
        std::snprintf(buffer, size, "%u", ui.settings.dim_brightness_percent); break;
    }
    return buffer;
}

void close_settings_editor(UiState &ui)
{
    if (ui.settings_modal != nullptr) lv_obj_delete(ui.settings_modal);
    ui.settings_modal = nullptr;
    ui.settings_input = nullptr;
    ui.settings_keyboard = nullptr;
    ui.settings_error = nullptr;
}

bool apply_settings_editor_value(UiState &ui, const char *value, const char **error)
{
    if (value == nullptr) return false;
    auto copy_text = [](char *destination, std::size_t capacity, const char *source) {
        std::strncpy(destination, source, capacity - 1);
        destination[capacity - 1] = '\0';
    };
    switch (ui.editing_field) {
    case SettingsField::PrimarySsid:
        if (value[0] != '\0' && !bluepaws::hub::validSsid(value)) {
            *error = "Use between 1 and 32 characters."; return false;
        }
        copy_text(ui.settings.primary.ssid, sizeof(ui.settings.primary.ssid), value); break;
    case SettingsField::SecondarySsid:
        if (value[0] != '\0' && !bluepaws::hub::validSsid(value)) {
            *error = "Use between 1 and 32 characters."; return false;
        }
        copy_text(ui.settings.secondary.ssid, sizeof(ui.settings.secondary.ssid), value); break;
    case SettingsField::AccessPointSsid:
        if (!bluepaws::hub::validSsid(value)) {
            *error = "The off-grid local network needs a name."; return false;
        }
        copy_text(ui.settings.access_point_ssid, sizeof(ui.settings.access_point_ssid), value); break;
    case SettingsField::PrimaryPassword:
    case SettingsField::SecondaryPassword:
    case SettingsField::AccessPointPassword:
        if (!bluepaws::hub::validPassword(value, true)) {
            *error = "Leave blank for open Wi-Fi, or use 8 to 63 characters."; return false;
        }
        if (ui.editing_field == SettingsField::PrimaryPassword)
            copy_text(ui.settings.primary.password, sizeof(ui.settings.primary.password), value);
        else if (ui.editing_field == SettingsField::SecondaryPassword)
            copy_text(ui.settings.secondary.password, sizeof(ui.settings.secondary.password), value);
        else
            copy_text(ui.settings.access_point_password, sizeof(ui.settings.access_point_password), value);
        break;
    default: {
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (value[0] == '\0' || end == nullptr || *end != '\0') {
            *error = "Enter a whole number."; return false;
        }
        if (ui.editing_field == SettingsField::OverviewTimeout) {
            if (parsed < 15 || parsed > 3600) {
                *error = "Choose 15 to 3600 seconds."; return false;
            }
            ui.settings.overview_timeout_seconds = static_cast<uint16_t>(parsed);
        } else if (ui.editing_field == SettingsField::DimTimeout) {
            if (parsed < 15 || parsed > 7200) {
                *error = "Choose 15 to 7200 seconds."; return false;
            }
            ui.settings.dim_timeout_seconds = static_cast<uint16_t>(parsed);
        } else if (ui.editing_field == SettingsField::ScreenOffTimeout) {
            if (parsed < 30 || parsed > 14400) {
                *error = "Choose 30 to 14400 seconds."; return false;
            }
            ui.settings.screen_off_timeout_seconds = static_cast<uint16_t>(parsed);
        } else {
            if (parsed < 1 || parsed > 50) {
                *error = "Choose a dim level from 1 to 50 percent."; return false;
            }
            ui.settings.dim_brightness_percent = static_cast<uint8_t>(parsed);
        }
        break;
    }
    }
    bluepaws::hub::sanitize(ui.settings);
    return true;
}

void settings_keyboard_event(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    if (ui == nullptr) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CANCEL) {
        close_settings_editor(*ui);
        return;
    }
    if (code != LV_EVENT_READY || ui->settings_input == nullptr) return;
    const char *error = nullptr;
    if (!apply_settings_editor_value(*ui, lv_textarea_get_text(ui->settings_input), &error)) {
        if (ui->settings_error != nullptr) lv_label_set_text(ui->settings_error, error);
        return;
    }
    const bool saved = bluepaws::settings_store::save(ui->settings);
    if (!saved) {
        if (ui->settings_error != nullptr)
            lv_label_set_text(ui->settings_error, "Could not save to device storage.");
        return;
    }
    if (ui->editing_field <= SettingsField::AccessPointPassword) {
        bluepaws::cloud::applyNetworkSettings(ui->settings);
    }
    close_settings_editor(*ui);
    lv_async_call(rebuild_current_page, ui);
}

void setting_card_clicked(lv_event_t *event)
{
    auto *ui = static_cast<UiState *>(lv_event_get_user_data(event));
    auto *target = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
    if (ui == nullptr || target == nullptr || ui->settings_modal != nullptr) return;
    const uintptr_t encoded = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target));
    if (encoded == 0) return;
    ui->editing_field = static_cast<SettingsField>(encoded - 1U);

    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
    lv_obj_align(modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x081018), 0);
    lv_obj_set_style_bg_opa(modal, 242, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_set_style_pad_all(modal, 14, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(modal, settings_field_title(ui->editing_field), lv_color_hex(0xF3F8FB));
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(title, 8, 4);
    lv_obj_t *hint = make_label(modal, "Press the tick to save or the keyboard icon to cancel.", lv_color_hex(0x80C9F2));
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(hint, 8, 30);

    lv_obj_t *input = lv_textarea_create(modal);
    lv_obj_set_size(input, LV_PCT(96), 54);
    lv_obj_align(input, LV_ALIGN_TOP_MID, 0, 62);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_max_length(input, settings_field_numeric(ui->editing_field) ? 5 :
        (ui->editing_field == SettingsField::PrimarySsid ||
         ui->editing_field == SettingsField::SecondarySsid ||
         ui->editing_field == SettingsField::AccessPointSsid ? 32 : 63));
    char buffer[16]{};
    lv_textarea_set_text(input, settings_field_value(*ui, ui->editing_field, buffer, sizeof(buffer)));
    lv_textarea_set_password_mode(input, settings_field_password(ui->editing_field));

    lv_obj_t *error_label = make_label(modal, "", lv_color_hex(0xFF8A80));
    lv_obj_set_style_text_font(error_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(error_label, 8, 122);

    lv_obj_t *keyboard = lv_keyboard_create(modal);
    lv_obj_set_size(keyboard, LV_PCT(100), ui->portrait ? 500 : 300);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(keyboard, settings_field_numeric(ui->editing_field)
        ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(keyboard, input);
    lv_obj_add_event_cb(keyboard, settings_keyboard_event, LV_EVENT_READY, ui);
    lv_obj_add_event_cb(keyboard, settings_keyboard_event, LV_EVENT_CANCEL, ui);
    lv_obj_move_foreground(modal);
    ui->settings_modal = modal;
    ui->settings_input = input;
    ui->settings_keyboard = keyboard;
    ui->settings_error = error_label;
}

lv_obj_t *create_setting_card(lv_obj_t *parent,
                              const char *title_text,
                              const char *value,
                              SettingsField field,
                              lv_color_t accent,
                              UiState &ui)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), 66);
    style_card(card, ui.dark_mode);
    lv_obj_set_style_border_color(card, accent, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_t *title = make_label(
        card,
        title_text,
        ui.dark_mode ? lv_color_hex(0x8DCDEC) : lv_color_hex(0x41657A));
    lv_obj_set_pos(title, 2, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_t *value_label = make_label(
        card,
        value,
        ui.dark_mode ? lv_color_hex(0xF3F8FB) : lv_color_hex(0x17324D));
    lv_obj_set_pos(value_label, 2, 28);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, reinterpret_cast<void *>(static_cast<uintptr_t>(field) + 1U));
    lv_obj_add_event_cb(card, setting_card_clicked, LV_EVENT_CLICKED, &ui);
    return card;
}

void make_settings_section(lv_obj_t *parent, const char *text, bool dark_mode)
{
    lv_obj_t *label = make_label(parent, text,
        dark_mode ? lv_color_hex(0x80C9F2) : lv_color_hex(0x28709A));
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
}

void create_settings_page(UiState &ui)
{
    lv_obj_t *content = bluepaws::ui::create_page_frame(
        lv_screen_active(),
        "BluePaws | Settings",
        "Tap a value to edit it",
        ui.dark_mode,
        page_actions(ui, true),
        &ui.status);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(content, ui.portrait ? 12 : 70, 0);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    const auto value_or = [](const char *value, const char *fallback) {
        return value[0] == '\0' ? fallback : value;
    };
    make_settings_section(content, "WI-FI CONNECTIONS", ui.dark_mode);
    create_setting_card(content, "PRIMARY NETWORK", value_or(ui.settings.primary.ssid, "Tap to configure"),
                        SettingsField::PrimarySsid, lv_color_hex(0x1976A3), ui);
    create_setting_card(content, "PRIMARY PASSWORD",
                        ui.settings.primary.password[0] == '\0' ? "Open / not set" : "Configured - tap to change",
                        SettingsField::PrimaryPassword, lv_color_hex(0x1976A3), ui);
    create_setting_card(content, "SECONDARY NETWORK", value_or(ui.settings.secondary.ssid, "Tap to configure"),
                        SettingsField::SecondarySsid, lv_color_hex(0x2E7D5B), ui);
    create_setting_card(content, "SECONDARY PASSWORD",
                        ui.settings.secondary.password[0] == '\0' ? "Open / not set" : "Configured - tap to change",
                        SettingsField::SecondaryPassword, lv_color_hex(0x2E7D5B), ui);

    make_settings_section(content, "OFF-GRID LOCAL NETWORK", ui.dark_mode);
    lv_obj_t *automatic_note = make_label(
        content,
        "Connection order: primary Wi-Fi, then the secondary phone hotspot. "
        "This local network starts automatically only when neither is available; "
        "the safety behaviour is always active.",
        ui.dark_mode ? lv_color_hex(0xC7D9E5) : lv_color_hex(0x38576D));
    lv_obj_set_width(automatic_note, LV_PCT(100));
    lv_label_set_long_mode(automatic_note, LV_LABEL_LONG_WRAP);
    create_setting_card(content, "LOCAL NETWORK NAME", ui.settings.access_point_ssid,
                        SettingsField::AccessPointSsid, lv_color_hex(0x7A5A9E), ui);
    create_setting_card(content, "LOCAL NETWORK PASSWORD",
                        ui.settings.access_point_password[0] == '\0' ? "Open / not set" : "Configured - tap to change",
                        SettingsField::AccessPointPassword, lv_color_hex(0x7A5A9E), ui);

    make_settings_section(content, "DISPLAY AND IDLE BEHAVIOUR", ui.dark_mode);
    char overview[32]{}, dim[32]{}, off[32]{}, dim_level[32]{};
    std::snprintf(overview, sizeof(overview), "%u seconds", ui.settings.overview_timeout_seconds);
    std::snprintf(dim, sizeof(dim), "%u seconds", ui.settings.dim_timeout_seconds);
    std::snprintf(off, sizeof(off), "%u seconds", ui.settings.screen_off_timeout_seconds);
    std::snprintf(dim_level, sizeof(dim_level), "%u%% brightness", ui.settings.dim_brightness_percent);
    create_setting_card(content, "OVERVIEW SCREEN AFTER", overview,
                        SettingsField::OverviewTimeout, lv_color_hex(0xB65E36), ui);
    create_setting_card(content, "DIM SCREEN AFTER", dim,
                        SettingsField::DimTimeout, lv_color_hex(0xB65E36), ui);
    create_setting_card(content, "SCREEN OFF AFTER", off,
                        SettingsField::ScreenOffTimeout, lv_color_hex(0xB65E36), ui);
    create_setting_card(content, "DIM LEVEL", dim_level,
                        SettingsField::DimBrightness, lv_color_hex(0xB65E36), ui);
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
    ui.overview_markers.fill(nullptr);
    ui.overview_labels.fill(nullptr);
    ui.drawer_cards.fill(nullptr);
    ui.drawer_summary_labels.fill(nullptr);
    ui.drawer_name_labels.fill(nullptr);
    ui.drawer_status_images.fill(nullptr);
    ui.drawer_profile_images.fill(nullptr);
    ui.drawer_fault_images.fill(nullptr);
    ui.drawer_battery_images.fill(nullptr);
    ui.drawer_battery_labels.fill(nullptr);
    ui.drawer_signal_images.fill(nullptr);
    ui.drawer_signal_labels.fill(nullptr);
    ui.drawer_radio_images.fill(nullptr);
    ui.drawer_distance_labels.fill(nullptr);
    ui.drawer_age_labels.fill(nullptr);
    ui.drawer_detail_labels.fill(nullptr);
    ui.drawer_expanded_panels.fill(nullptr);
    ui.drawer_message_labels.fill(nullptr);
    ui.drawer_jump_buttons.fill(nullptr);
    ui.drawer_follow_buttons.fill(nullptr);
    ui.drawer_trail_buttons.fill(nullptr);
    ui.drawer_command_buttons.fill(nullptr);
    ui.drawer_card_expanded.fill(false);
    ui.map_view = nullptr;
    ui.cat_list = nullptr;
    ui.diagnostics_text = nullptr;
    ui.map_drawer = nullptr;
    ui.layer_drawer = nullptr;
    ui.brightness_popup = nullptr;
    ui.brightness_slider = nullptr;
    ui.brightness_label = nullptr;
    ui.volume_popup = nullptr;
    ui.volume_slider = nullptr;
    ui.volume_label = nullptr;
    ui.quick_settings_tray = nullptr;
    ui.quick_settings_handle = nullptr;
    ui.brightness_hide_timer = nullptr;
    ui.volume_hide_timer = nullptr;
    ui.settings_modal = nullptr;
    ui.settings_input = nullptr;
    ui.settings_keyboard = nullptr;
    ui.settings_error = nullptr;
    ui.quick_settings_open = false;
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
    case AppPage::Overview:
        create_overview_page(ui);
        break;
    }

    if (ui.active_page != AppPage::Overview) create_quick_settings_tray(ui);

    update_ui(ui);
    if (ui.update_timer == nullptr) {
        ui.update_timer = lv_timer_create(update_timer, 1000, &ui);
    }
    if (ui.gesture_timer == nullptr) {
        ui.gesture_timer = lv_timer_create(gesture_timer, kGesturePollMs, &ui);
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
    std::strncpy(ui.settings.primary.ssid,
                 HOME_HUB_WIFI_SSID,
                 sizeof(ui.settings.primary.ssid) - 1);
    std::strncpy(ui.settings.primary.password,
                 HOME_HUB_WIFI_PASSWORD,
                 sizeof(ui.settings.primary.password) - 1);
    bluepaws::settings_store::load(ui.settings);
    ui.brightness_percent = ui.settings.brightness_percent;
    ui.volume_percent = ui.settings.volume_percent;
    guition_jc4880p443c_backlight_set(ui.brightness_percent);
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
        log_map_storage_probe(ui);
    }
    ui.simulator.reset(kTestOrigin, uptime_ms());
    ui.cloud_enabled = bluepaws::cloud::start(ui.settings);
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(kTag, "Could not acquire LVGL lock");
        return;
    }
    create_ui(ui);
    lvgl_port_unlock();
    ESP_LOGI(kTag, "BluePaws Home Hub testbed UI started");
}
