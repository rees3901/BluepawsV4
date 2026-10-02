#include "guition_jc3248w535c.h"

#include "bluepaws/cat_simulator.h"
#include "bluepaws/cat_store.h"
#include "bluepaws/hub_settings.h"
#include "bluepaws/map_engine.h"
#include "home_hub_bluetooth.h"
#include "home_hub_cloud.h"
#include "home_hub_config.h"
#include "home_hub_settings_store.h"
#include "home_hub_web.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

namespace {

constexpr char kTag[] = "bluepaws_s3";
constexpr bluepaws::map::GeoPoint kHome{51.905879, -2.239486};
constexpr char kTileRoot[] = "/sdcard/bluepaws/maps/layers/osm-road-100km/tiles";
constexpr int kHeaderHeight = 54;
constexpr int kMapHeight = 266;
constexpr int kDrawerWidth = 188;
constexpr std::size_t kTileSlots = 9;
constexpr std::array<uint32_t, bluepaws::kMaximumCats> kMarkerColours{
    0x18B8E8, 0xFF6B52, 0x20D6A2, 0xFFB000,
    0x8B5CF6, 0x00B8C8, 0x70574E, 0x1976D2,
};

bluepaws::CatStore store;
bluepaws::CatSimulator simulator{kHome};
bluepaws::hub::Settings settings = bluepaws::hub::defaultSettings();
bluepaws::map::Viewport viewport{480, kMapHeight, kHome, 15};
bool cloud_authorized = false;
bool sd_mounted = false;
bool drawer_open = true;
uint64_t pending_control_revision = 0;
bool pending_control = false;

lv_obj_t *header = nullptr;
lv_obj_t *mode_dropdown = nullptr;
lv_obj_t *network_label = nullptr;
lv_obj_t *wifi_status_label = nullptr;
lv_obj_t *bluetooth_status_label = nullptr;
lv_obj_t *clock_label = nullptr;
lv_obj_t *mode_confirmation = nullptr;
lv_obj_t *mode_confirmation_title = nullptr;
lv_obj_t *mode_confirmation_body = nullptr;
bluepaws::hub::CommunicationsMode pending_mode = bluepaws::hub::CommunicationsMode::Home;
lv_obj_t *map_view = nullptr;
lv_obj_t *drawer = nullptr;
lv_obj_t *hamburger = nullptr;
lv_obj_t *safety_label = nullptr;
lv_obj_t *summary_label = nullptr;
lv_obj_t *zoom_label = nullptr;
std::array<lv_obj_t *, kTileSlots> tile_images{};
struct TileSlot {
    bluepaws::map::TileId requested{};
    bluepaws::map::TileId loaded{};
    lv_image_dsc_t descriptor{};
    uint8_t *pixels = nullptr;
    bool pending = false;
    bool valid = false;
};
std::array<TileSlot, kTileSlots> tile_slots{};
std::array<lv_obj_t *, bluepaws::kMaximumCats> markers{};
lv_obj_t *drawer_list = nullptr;
std::array<lv_obj_t *, bluepaws::kMaximumCats> drawer_rows{};
lv_point_t map_press_start{};
lv_point_t last_map_tap{};
uint32_t last_map_tap_ms = 0;
bool map_press_active = false;
bool map_press_moved = false;
bool map_press_multitouch = false;
lv_obj_t *settings_overlay = nullptr;
lv_obj_t *settings_body = nullptr;
lv_obj_t *settings_status = nullptr;
lv_obj_t *scan_dropdown = nullptr;
lv_obj_t *editor_overlay = nullptr;
lv_obj_t *editor_input = nullptr;
lv_obj_t *editor_visibility = nullptr;
lv_obj_t *editor_title = nullptr;
lv_obj_t *editor_error = nullptr;
lv_obj_t *brightness_value = nullptr;
lv_obj_t *info_label = nullptr;
std::array<lv_obj_t *, 5> settings_tabs{};
std::array<lv_obj_t *, bluepaws::kMaximumCats> settings_cat_labels{};
uint8_t settings_tab = 0;
uint8_t editing_field = 0;
char pending_scan_ssid[33]{};
bool pending_scan_primary = true;
uint32_t scan_generation = 0;
bluepaws::cloud::WifiScanSnapshot scan_snapshot{};

void refresh_settings_status(const bluepaws::cloud::Status &cloud);

lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t colour,
                     const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

lv_obj_t *make_panel(lv_obj_t *parent, uint32_t colour, int radius)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_style_bg_color(panel, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x28516E), 0);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

lv_obj_t *make_control(lv_obj_t *parent, const char *text, int x, int y,
                       int width, int height, lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x102638), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_90, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x5DDDF0), 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = make_label(button, text, 0xFFFFFF, &lv_font_montserrat_18);
    lv_obj_center(label);
    return button;
}

bool tile_pack_available()
{
    struct stat info{};
    return sd_mounted && stat(kTileRoot, &info) == 0 && S_ISDIR(info.st_mode);
}

struct JpegIo {
    FILE *file;
    uint8_t *pixels;
};

size_t jpeg_input(JDEC *decoder, uint8_t *buffer, size_t length)
{
    FILE *file = static_cast<JpegIo *>(decoder->device)->file;
    if (buffer != nullptr) return std::fread(buffer, 1, length, file);
    return std::fseek(file, static_cast<long>(length), SEEK_CUR) == 0 ? length : 0;
}

int jpeg_output(JDEC *decoder, void *bitmap, JRECT *rect)
{
    auto *pixels = static_cast<JpegIo *>(decoder->device)->pixels;
    const auto *rgb = static_cast<const uint8_t *>(bitmap);
    const unsigned width = rect->right - rect->left + 1;
    for (unsigned y = rect->top; y <= rect->bottom; ++y) {
        for (unsigned x = rect->left; x <= rect->right; ++x) {
            const unsigned source = ((y - rect->top) * width + x - rect->left) * 3;
            const uint16_t colour = static_cast<uint16_t>(
                ((rgb[source] & 0xF8U) << 8) |
                ((rgb[source + 1] & 0xFCU) << 3) |
                (rgb[source + 2] >> 3));
            const unsigned target = (y * bluepaws::map::kTileSize + x) * 2;
            pixels[target] = static_cast<uint8_t>(colour);
            pixels[target + 1] = static_cast<uint8_t>(colour >> 8);
        }
    }
    return 1;
}

bool decode_tile(TileSlot &slot, const bluepaws::map::TileId &id)
{
    char path[176]{};
    std::snprintf(path, sizeof(path), "%s/%u/%lu/%lu.jpg", kTileRoot,
                  id.zoom, static_cast<unsigned long>(id.x),
                  static_cast<unsigned long>(id.y));
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    constexpr size_t kPixelBytes = bluepaws::map::kTileSize *
                                    bluepaws::map::kTileSize * 2;
    if (slot.pixels == nullptr) {
        slot.pixels = static_cast<uint8_t *>(heap_caps_malloc(
            kPixelBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (slot.pixels == nullptr) {
        std::fclose(file);
        ESP_LOGE(kTag, "No PSRAM for map tile");
        return false;
    }
    JpegIo io{file, slot.pixels};
    auto *workspace = static_cast<uint8_t *>(heap_caps_malloc(
        4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (workspace == nullptr) {
        std::fclose(file);
        return false;
    }
    JDEC decoder{};
    const JRESULT prepared = jd_prepare(&decoder, jpeg_input, workspace,
                                         4096, &io);
    const bool dimensions_ok = prepared == JDR_OK &&
        decoder.width == bluepaws::map::kTileSize &&
        decoder.height == bluepaws::map::kTileSize;
    const JRESULT decoded = dimensions_ok
        ? jd_decomp(&decoder, jpeg_output, 0) : prepared;
    heap_caps_free(workspace);
    std::fclose(file);
    if (!dimensions_ok || decoded != JDR_OK) {
        ESP_LOGW(kTag, "Map tile decode failed (%d, %d): %s",
                 static_cast<int>(prepared), static_cast<int>(decoded), path);
        return false;
    }
    slot.descriptor = {};
    slot.descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot.descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    slot.descriptor.header.w = bluepaws::map::kTileSize;
    slot.descriptor.header.h = bluepaws::map::kTileSize;
    slot.descriptor.header.stride = bluepaws::map::kTileSize * 2;
    slot.descriptor.data_size = kPixelBytes;
    slot.descriptor.data = slot.pixels;
    ESP_LOGI(kTag, "Decoded map tile %s", path);
    return true;
}

void refresh_tiles()
{
    const auto grid = viewport.visibleTiles(0);
    const bool available = tile_pack_available();
    for (std::size_t i = 0; i < tile_images.size(); ++i) {
        TileSlot &slot = tile_slots[i];
        if (i >= grid.count || !available) {
            lv_obj_add_flag(tile_images[i], LV_OBJ_FLAG_HIDDEN);
            slot.pending = false;
            continue;
        }
        const auto &tile = grid.tiles[i];
        lv_obj_set_pos(tile_images[i], tile.screen_x, tile.screen_y);
        if (slot.valid && slot.loaded == tile.id) {
            lv_obj_remove_flag(tile_images[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        if (!(slot.pending && slot.requested == tile.id)) {
            lv_obj_add_flag(tile_images[i], LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(tile_images[i], nullptr);
            if (slot.valid) lv_image_cache_drop(&slot.descriptor);
            slot.valid = false;
            slot.requested = tile.id;
            slot.pending = true;
        }
    }
    if (zoom_label != nullptr) lv_label_set_text_fmt(zoom_label, "z%u", viewport.zoom());
}

void load_next_tile(lv_timer_t *)
{
    // Let the first frame and staged Wi-Fi/BLE startup settle before SD reads
    // and software JPEG conversion begin on this power-limited board.
    if (esp_timer_get_time() < 5'000'000) return;
    for (std::size_t i = 0; i < tile_slots.size(); ++i) {
        TileSlot &slot = tile_slots[i];
        if (!slot.pending) continue;
        slot.pending = false;
        if (!decode_tile(slot, slot.requested)) {
            lv_obj_add_flag(tile_images[i], LV_OBJ_FLAG_HIDDEN);
            return;
        }
        slot.loaded = slot.requested;
        slot.valid = true;
        lv_image_set_src(tile_images[i], &slot.descriptor);
        lv_obj_remove_flag(tile_images[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(tile_images[i], 0);
        return;
    }
}

void refresh_markers()
{
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const bluepaws::CatRecord *cat = store.at(i);
        if (cat == nullptr || !cat->has_position) {
            lv_obj_add_flag(markers[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const auto point = viewport.toScreen({
            cat->last_valid_latitude_e7 / 1.0e7,
            cat->last_valid_longitude_e7 / 1.0e7,
        });
        if (point.x < -18 || point.y < -18 || point.x > 498 ||
            point.y > kMapHeight + 18) {
            lv_obj_add_flag(markers[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_pos(markers[i], static_cast<int>(point.x) - 14,
                       static_cast<int>(point.y) - 14);
        lv_obj_remove_flag(markers[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(markers[i]);
    }
    if (drawer != nullptr && drawer_open) lv_obj_move_foreground(drawer);
    if (hamburger != nullptr && !drawer_open) lv_obj_move_foreground(hamburger);
}

void set_drawer(bool open)
{
    drawer_open = open;
    if (open) {
        lv_obj_remove_flag(drawer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hamburger, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(drawer);
    } else {
        lv_obj_add_flag(drawer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(hamburger, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hamburger);
    }
}

void drawer_toggle(lv_event_t *) { set_drawer(!drawer_open); }

int32_t point_distance_squared(const lv_point_t &a, const lv_point_t &b)
{
    const int32_t dx = a.x - b.x;
    const int32_t dy = a.y - b.y;
    return dx * dx + dy * dy;
}

void map_pressed(lv_event_t *)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_indev_get_point(indev, &map_press_start);
    map_press_active = true;
    map_press_moved = false;
    map_press_multitouch = guition_jc3248w535c_touch_count() > 1;
}

void map_pressing(lv_event_t *)
{
    if (!map_press_active) return;
    const int8_t pinch_steps = guition_jc3248w535c_take_pinch_steps();
    if (pinch_steps != 0) {
        const int zoom = std::clamp(static_cast<int>(viewport.zoom()) +
                                    pinch_steps, 5, 17);
        viewport.setZoom(static_cast<uint8_t>(zoom));
        refresh_tiles();
        refresh_markers();
        map_press_multitouch = true;
    }
    if (guition_jc3248w535c_touch_count() > 1) {
        map_press_multitouch = true;
        last_map_tap_ms = 0;
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point{};
    lv_indev_get_point(indev, &point);
    if (point_distance_squared(point, map_press_start) > 100) {
        map_press_moved = true;
        last_map_tap_ms = 0;
    }
    lv_point_t vector{};
    lv_indev_get_vect(indev, &vector);
    if (vector.x == 0 && vector.y == 0) return;
    viewport.panBy(vector.x, vector.y);
    refresh_tiles();
    refresh_markers();
}

void map_released(lv_event_t *)
{
    if (!map_press_active) return;
    map_press_active = false;
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point{};
    lv_indev_get_point(indev, &point);
    if (map_press_moved || map_press_multitouch ||
        point_distance_squared(point, map_press_start) > 100) {
        last_map_tap_ms = 0;
        return;
    }
    if (drawer_open) {
        set_drawer(false);
        last_map_tap_ms = 0;
        return;
    }
    const uint32_t now = lv_tick_get();
    if (last_map_tap_ms != 0 && now - last_map_tap_ms < 360 &&
        point_distance_squared(point, last_map_tap) < 900) {
        last_map_tap_ms = 0;
        const bluepaws::map::ScreenPoint local{
            static_cast<double>(point.x),
            static_cast<double>(point.y - kHeaderHeight),
        };
        viewport.centerAndZoom(local,
            std::min<uint8_t>(17, viewport.zoom() + 1));
        refresh_tiles();
        refresh_markers();
        return;
    }
    last_map_tap = point;
    last_map_tap_ms = now;
}

void zoom_in(lv_event_t *)
{
    viewport.setZoom(std::min<uint8_t>(17, viewport.zoom() + 1));
    refresh_tiles();
    refresh_markers();
}

void zoom_out(lv_event_t *)
{
    viewport.setZoom(std::max<uint8_t>(5, viewport.zoom() - 1));
    refresh_tiles();
    refresh_markers();
}

void home_clicked(lv_event_t *)
{
    std::array<bluepaws::map::GeoPoint, bluepaws::kMaximumCats + 1> points{};
    std::size_t count = 1;
    points[0] = kHome;
    for (std::size_t i = 0; i < store.size() && count < points.size(); ++i) {
        const auto *cat = store.at(i);
        if (cat != nullptr && cat->has_position) {
            points[count++] = {cat->last_valid_latitude_e7 / 1.0e7,
                               cat->last_valid_longitude_e7 / 1.0e7};
        }
    }
    const auto fit = bluepaws::map::fitPoints(points.data(), count, 480,
                                               kMapHeight, 42, 5, 17);
    if (fit.valid) {
        viewport.setCenter(fit.center);
        viewport.setZoom(fit.zoom > 5 ? fit.zoom - 1 : fit.zoom);
    }
    refresh_tiles();
    refresh_markers();
}

const char *mode_consequence(bluepaws::hub::CommunicationsMode mode)
{
    switch (mode) {
    case bluepaws::hub::CommunicationsMode::Home:
        return "Try home Wi-Fi first, then portable Wi-Fi. If both fail, start the local hotspot. The Home BLE beacon is allowed only while actually at home.";
    case bluepaws::hub::CommunicationsMode::Portable:
        return "Use the saved portable Wi-Fi. If unavailable, fall back to the local hotspot. The Home BLE beacon stays off.";
    case bluepaws::hub::CommunicationsMode::OffGrid:
        return "Stop joining Wi-Fi networks and start the local hotspot. Home BLE advertising stays off; scanning is on request only.";
    }
    return "Apply this hub mode?";
}

bool apply_mode(bluepaws::hub::CommunicationsMode mode)
{
    const auto previous = settings.communications_mode;
    settings.communications_mode = mode;
    if (!bluepaws::settings_store::save(settings) ||
        !bluepaws::cloud::applyNetworkSettings(settings)) {
        settings.communications_mode = previous;
        bluepaws::settings_store::save(settings);
        return false;
    }
    bluepaws::bluetooth::apply(settings.bluetooth_enabled,
                               settings.communications_mode);
    return true;
}

void mode_cancelled(lv_event_t *)
{
    lv_obj_add_flag(mode_confirmation, LV_OBJ_FLAG_HIDDEN);
}

void mode_accepted(lv_event_t *)
{
    lv_obj_add_flag(mode_confirmation, LV_OBJ_FLAG_HIDDEN);
    if (!apply_mode(pending_mode)) {
        ESP_LOGE(kTag, "Could not apply requested hub mode");
    }
    lv_dropdown_set_selected(mode_dropdown,
        static_cast<uint32_t>(bluepaws::cloud::status().effective_mode));
}

void mode_changed(lv_event_t *)
{
    const uint32_t selected = lv_dropdown_get_selected(mode_dropdown);
    if (selected > 2) return;
    pending_mode = static_cast<bluepaws::hub::CommunicationsMode>(selected);
    lv_dropdown_set_selected(mode_dropdown,
        static_cast<uint32_t>(bluepaws::cloud::status().effective_mode));
    if (pending_mode == settings.communications_mode) return;
    lv_label_set_text_fmt(mode_confirmation_title, "Switch to %s?",
        bluepaws::hub::communicationsModeName(pending_mode));
    lv_label_set_text(mode_confirmation_body, mode_consequence(pending_mode));
    lv_obj_remove_flag(mode_confirmation, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mode_confirmation);
}

void update_runtime()
{
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    const std::size_t cloud_updates = bluepaws::cloud::drain(store);
    if (!cloud_authorized && cloud_updates == 0) simulator.update(now_ms, store);

    bluepaws::cloud::ControlUpdate control{};
    while (bluepaws::cloud::takeControlUpdate(control)) {
        if (control.revision <= settings.cloud_settings_revision) continue;
        bluepaws::hub::Settings proposed = settings;
        proposed.bluetooth_enabled = control.bluetooth_enabled;
        proposed.reporting_profile = control.reporting_profile;
        std::memcpy(proposed.display_name, control.display_name, sizeof(proposed.display_name));
        std::memcpy(proposed.home_emoji, control.home_emoji, sizeof(proposed.home_emoji));
        std::memcpy(proposed.portable_emoji, control.portable_emoji, sizeof(proposed.portable_emoji));
        std::memcpy(proposed.marker_colour, control.marker_colour, sizeof(proposed.marker_colour));
        if (bluepaws::settings_store::save(proposed)) {
            settings = proposed;
            pending_control_revision = control.revision;
            pending_control = true;
        }
    }

    bluepaws::hub::CommunicationsMode requested_mode{};
    if (bluepaws::web::takeRequestedMode(requested_mode)) {
        settings.communications_mode = requested_mode;
        if (bluepaws::settings_store::save(settings)) {
            bluepaws::cloud::applyNetworkSettings(settings);
        }
    }
    bool requested_bluetooth = false;
    if (bluepaws::web::takeRequestedBluetooth(requested_bluetooth)) {
        settings.bluetooth_enabled = requested_bluetooth;
        bluepaws::settings_store::save(settings);
    }

    bluepaws::cloud::Status cloud = bluepaws::cloud::status();
    bluepaws::bluetooth::apply(settings.bluetooth_enabled, cloud.effective_mode);
    const bluepaws::bluetooth::Status bluetooth = bluepaws::bluetooth::status();
    if (pending_control && bluetooth.settled) {
        settings.cloud_settings_revision = pending_control_revision;
        if (bluepaws::settings_store::save(settings)) {
            bluepaws::cloud::acknowledgeControlUpdate(
                pending_control_revision, settings.bluetooth_enabled,
                settings.reporting_profile);
            pending_control = false;
            cloud = bluepaws::cloud::status();
        }
    }
    bluepaws::web::updateSnapshot(store, cloud, settings, bluetooth);
    refresh_settings_status(cloud);

    if (mode_confirmation == nullptr ||
        lv_obj_has_flag(mode_confirmation, LV_OBJ_FLAG_HIDDEN)) {
        lv_dropdown_set_selected(mode_dropdown,
            static_cast<uint32_t>(cloud.effective_mode));
    }
    const uint32_t mode_colour =
        cloud.effective_mode == bluepaws::hub::CommunicationsMode::OffGrid
            ? 0x563516
            : (cloud.effective_mode == bluepaws::hub::CommunicationsMode::Portable
                ? 0x17433A : 0x122638);
    lv_obj_set_style_bg_color(header, lv_color_hex(mode_colour), 0);
    const char *link = cloud.wifi_station_connected ? cloud.wifi_ssid
        : (cloud.effective_mode == bluepaws::hub::CommunicationsMode::OffGrid
            ? "Local AP" : "Connecting");
    lv_label_set_text_fmt(network_label, "%s%s", link,
        cloud.wifi_station_connected ? " connected" : "");
    lv_label_set_text(wifi_status_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_status_label,
        lv_color_hex(cloud.wifi_station_connected
            ? (cloud.wifi_rssi_dbm >= -67 ? 0x4AE2B7
               : cloud.wifi_rssi_dbm >= -80 ? 0xF1C45A : 0xED6C61)
            : (cloud.effective_mode == bluepaws::hub::CommunicationsMode::OffGrid
                ? 0x4AE2B7 : 0x82929E)), 0);
    lv_label_set_text(bluetooth_status_label,
        bluetooth.advertising || bluetooth.scanning
            ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_BLUETOOTH "x");
    lv_obj_set_style_text_color(bluetooth_status_label,
        lv_color_hex(bluetooth.advertising || bluetooth.scanning
            ? 0x4AE2B7 : 0xED6C61), 0);
    if (cloud.time_synchronized) {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        lv_label_set_text_fmt(clock_label, "%02d:%02d", local.tm_hour,
                              local.tm_min);
    } else {
        lv_label_set_text(clock_label, "--:--");
    }

    unsigned recent = 0;
    for (std::size_t i = 0; i < store.size(); ++i) {
        const auto *cat = store.at(i);
        if (cat != nullptr && now_ms - cat->latest.received_at_ms <= 3600000U) ++recent;
    }
    if (store.size() > 0 && recent == store.size()) {
        lv_label_set_text(safety_label, LV_SYMBOL_OK " All pets accounted for");
        lv_obj_set_style_text_color(safety_label, lv_color_hex(0x69F7D8), 0);
    } else {
        lv_label_set_text_fmt(safety_label, "%u/%u recently seen", recent,
                              static_cast<unsigned>(store.size()));
        lv_obj_set_style_text_color(safety_label, lv_color_hex(0xF4C66A), 0);
    }
    lv_label_set_text_fmt(summary_label, "%u collars  |  %s",
                          static_cast<unsigned>(store.size()),
                          tile_pack_available() ? "SD map ready" : "Map pack unavailable");
    for (std::size_t i = 0; i < drawer_rows.size(); ++i) {
        const auto *cat = store.at(i);
        if (cat == nullptr) {
            lv_obj_add_flag(drawer_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(drawer_rows[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(lv_obj_get_child(drawer_rows[i], 0),
                              "%u  %s\n%u%%  %d dBm  %lus",
                              static_cast<unsigned>(i + 1), cat->name,
                              cat->latest.battery_percent, cat->latest.rssi,
                              static_cast<unsigned long>((now_ms - cat->latest.received_at_ms) / 1000U));
    }
    refresh_markers();
}

void refresh_timer(lv_timer_t *) { update_runtime(); }

void show_settings_tab(uint8_t tab);

void settings_closed(lv_event_t *)
{
    if (settings_overlay != nullptr)
        lv_obj_add_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);
}

void settings_tab_clicked(lv_event_t *event)
{
    show_settings_tab(static_cast<uint8_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

void editor_closed(lv_event_t *)
{
    pending_scan_ssid[0] = '\0';
    if (editor_overlay != nullptr)
        lv_obj_add_flag(editor_overlay, LV_OBJ_FLAG_HIDDEN);
}

void editor_visibility_clicked(lv_event_t *)
{
    const bool hidden = lv_textarea_get_password_mode(editor_input);
    lv_textarea_set_password_mode(editor_input, !hidden);
    lv_label_set_text(lv_obj_get_child(editor_visibility, 0),
        hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}

void editor_saved(lv_event_t *)
{
    const char *value = lv_textarea_get_text(editor_input);
    const bool password = (editing_field & 1U) != 0;
    if (password ? !bluepaws::hub::validPassword(value)
                 : (value[0] != '\0' && !bluepaws::hub::validSsid(value))) {
        lv_label_set_text(editor_error, password
            ? "Password: blank or 8-63 characters"
            : "Wi-Fi name must be 32 characters or fewer");
        return;
    }
    bluepaws::hub::Settings proposed = settings;
    bluepaws::hub::WifiNetwork &network = editing_field < 2
        ? proposed.primary : proposed.secondary;
    if (password && pending_scan_ssid[0] != '\0' &&
        (editing_field < 2) == pending_scan_primary) {
        if (std::strcmp(network.ssid, pending_scan_ssid) != 0)
            network.password[0] = '\0';
        std::snprintf(network.ssid, sizeof(network.ssid), "%s", pending_scan_ssid);
    }
    char *destination = password ? network.password : network.ssid;
    const size_t capacity = password ? sizeof(network.password)
                                     : sizeof(network.ssid);
    std::snprintf(destination, capacity, "%s", value);
    if (!password && value[0] == '\0') network.password[0] = '\0';
    if (!bluepaws::settings_store::save(proposed) ||
        !bluepaws::cloud::applyNetworkSettings(proposed)) {
        bluepaws::settings_store::save(settings);
        lv_label_set_text(editor_error, "Could not save or apply Wi-Fi settings");
        return;
    }
    settings = proposed;
    editor_closed(nullptr);
    show_settings_tab(0);
}

void open_editor(uint8_t field)
{
    editing_field = field;
    const bool password = (field & 1U) != 0;
    const bool primary = field < 2;
    const bluepaws::hub::WifiNetwork &network = primary
        ? settings.primary : settings.secondary;
    const char *value = password ? network.password : network.ssid;
    if (editor_overlay == nullptr) {
        editor_overlay = make_panel(lv_screen_active(), 0x0B1C2A, 0);
        lv_obj_set_size(editor_overlay, 480, 320);
        lv_obj_add_flag(editor_overlay, LV_OBJ_FLAG_FLOATING);
        editor_title = make_label(editor_overlay, "Wi-Fi", 0xFFFFFF,
                                   &lv_font_montserrat_18);
        lv_obj_set_pos(editor_title, 10, 9);
        make_control(editor_overlay, "Cancel", 276, 4, 90, 38, editor_closed);
        make_control(editor_overlay, "Save", 374, 4, 96, 38, editor_saved);
        editor_input = lv_textarea_create(editor_overlay);
        lv_obj_set_pos(editor_input, 10, 50);
        lv_obj_set_size(editor_input, 410, 43);
        lv_textarea_set_one_line(editor_input, true);
        lv_textarea_set_max_length(editor_input, 64);
        editor_visibility = make_control(editor_overlay,
            LV_SYMBOL_EYE_CLOSE, 427, 51, 43, 41, editor_visibility_clicked);
        editor_error = make_label(editor_overlay, "", 0xF1C45A,
                                   &lv_font_montserrat_14);
        lv_obj_set_pos(editor_error, 10, 96);
        lv_obj_t *keyboard = lv_keyboard_create(editor_overlay);
        lv_obj_set_pos(keyboard, 7, 122);
        lv_obj_set_size(keyboard, 466, 191);
        lv_keyboard_set_textarea(keyboard, editor_input);
    }
    lv_label_set_text(editor_title, primary
        ? (password ? "Home Wi-Fi password" : "Home Wi-Fi name")
        : (password ? "Portable Wi-Fi password" : "Portable Wi-Fi name"));
    lv_textarea_set_text(editor_input, value);
    lv_textarea_set_max_length(editor_input, password ? 64 : 32);
    lv_textarea_set_password_mode(editor_input, password);
    if (password) lv_obj_remove_flag(editor_visibility, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(editor_visibility, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lv_obj_get_child(editor_visibility, 0), LV_SYMBOL_EYE_CLOSE);
    lv_label_set_text(editor_error, "");
    lv_obj_remove_flag(editor_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(editor_overlay);
}

void wifi_field_clicked(lv_event_t *event)
{
    open_editor(static_cast<uint8_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

void wifi_scan_clicked(lv_event_t *)
{
    if (bluepaws::cloud::requestWifiScan()) {
        lv_label_set_text(settings_status, "Scanning nearby Wi-Fi...");
    } else {
        lv_label_set_text(settings_status, "Wi-Fi scan unavailable right now");
    }
}

void wifi_use_clicked(lv_event_t *event)
{
    if (scan_dropdown == nullptr || scan_snapshot.count == 0) return;
    const uint32_t choice = lv_dropdown_get_selected(scan_dropdown);
    if (choice >= scan_snapshot.count) return;
    const bool primary = reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)) == 0;
    const auto &selected = scan_snapshot.results[choice];
    if (selected.secured) {
        std::snprintf(pending_scan_ssid, sizeof(pending_scan_ssid), "%s",
                      selected.ssid);
        pending_scan_primary = primary;
        open_editor(primary ? 1 : 3);
        return;
    }
    bluepaws::hub::Settings proposed = settings;
    bluepaws::hub::WifiNetwork &network = primary
        ? proposed.primary : proposed.secondary;
    if (std::strcmp(network.ssid, selected.ssid) != 0) network.password[0] = '\0';
    std::snprintf(network.ssid, sizeof(network.ssid), "%s", selected.ssid);
    if (!bluepaws::settings_store::save(proposed) ||
        !bluepaws::cloud::applyNetworkSettings(proposed)) {
        bluepaws::settings_store::save(settings);
        lv_label_set_text(settings_status, "Could not apply selected Wi-Fi");
        return;
    }
    settings = proposed;
    show_settings_tab(0);
}

void brightness_changed(lv_event_t *event)
{
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const int value = lv_slider_get_value(slider);
    guition_jc3248w535c_backlight_set(value);
    if (brightness_value != nullptr)
        lv_label_set_text_fmt(brightness_value, "%d%%", value);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        settings.brightness_percent = static_cast<uint8_t>(value);
        bluepaws::settings_store::save(settings);
    }
}

void bluetooth_clicked(lv_event_t *)
{
    settings.bluetooth_enabled = !settings.bluetooth_enabled;
    if (!bluepaws::settings_store::save(settings)) {
        settings.bluetooth_enabled = !settings.bluetooth_enabled;
        return;
    }
    bluepaws::bluetooth::apply(settings.bluetooth_enabled,
                               bluepaws::cloud::status().effective_mode);
    show_settings_tab(1);
}

void reporting_changed(lv_event_t *event)
{
    auto *dropdown = static_cast<lv_obj_t *>(lv_event_get_target(event));
    settings.reporting_profile = static_cast<bluepaws::hub::ReportingProfile>(
        std::min<uint32_t>(2, lv_dropdown_get_selected(dropdown)));
    bluepaws::settings_store::save(settings);
}

void show_settings_tab(uint8_t tab)
{
    if (settings_body == nullptr || tab > 4) return;
    settings_tab = tab;
    settings_status = nullptr;
    scan_dropdown = nullptr;
    brightness_value = nullptr;
    info_label = nullptr;
    settings_cat_labels.fill(nullptr);
    lv_obj_clean(settings_body);
    for (std::size_t i = 0; i < settings_tabs.size(); ++i) {
        if (settings_tabs[i] != nullptr)
            lv_obj_set_style_bg_color(settings_tabs[i],
                lv_color_hex(i == tab ? 0x157FA2 : 0x183248), 0);
    }
    if (tab == 0) {
        scan_generation = 0;
        settings_status = make_label(settings_body, "Checking Wi-Fi...",
                                      0x9CDBE8, &lv_font_montserrat_14);
        lv_obj_set_pos(settings_status, 7, 5);
        lv_obj_set_width(settings_status, 440);
        make_control(settings_body, "Scan", 7, 36, 88, 40, wifi_scan_clicked);
        scan_dropdown = lv_dropdown_create(settings_body);
        lv_dropdown_set_options(scan_dropdown, "No scan results");
        lv_obj_set_pos(scan_dropdown, 105, 36);
        lv_obj_set_size(scan_dropdown, 341, 40);
        make_control(settings_body, "Use for home", 7, 84, 214, 40,
                     wifi_use_clicked);
        lv_obj_t *portable = make_control(settings_body, "Use for portable",
                                           231, 84, 215, 40, wifi_use_clicked);
        lv_obj_remove_event_cb(portable, wifi_use_clicked);
        lv_obj_add_event_cb(portable, wifi_use_clicked, LV_EVENT_CLICKED,
                             reinterpret_cast<void *>(1));
        const char *titles[] = {
            "Home Wi-Fi name", "Home password", "Portable Wi-Fi name",
            "Portable password"};
        for (uint8_t i = 0; i < 4; ++i) {
            lv_obj_t *button = make_control(settings_body, titles[i], 7,
                133 + i * 49, 439, 43, wifi_field_clicked);
            lv_obj_remove_event_cb(button, wifi_field_clicked);
            lv_obj_add_event_cb(button, wifi_field_clicked, LV_EVENT_CLICKED,
                reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        }
        refresh_settings_status(bluepaws::cloud::status());
    } else if (tab == 1) {
        auto *title = make_label(settings_body, "Hub controls", 0xFFFFFF,
                                 &lv_font_montserrat_18);
        lv_obj_set_pos(title, 8, 6);
        auto *mode = make_label(settings_body,
            "Choose Home, Portable or Off-Grid from the top bar.",
            0xA9D6E1, &lv_font_montserrat_14);
        lv_obj_set_pos(mode, 8, 36);
        lv_obj_t *button = make_control(settings_body,
            settings.bluetooth_enabled ? "Bluetooth: On" : "Bluetooth: Off",
            8, 74, 439, 44, bluetooth_clicked);
        (void)button;
        auto *profile = make_label(settings_body, "Reporting profile",
                                    0xFFFFFF, &lv_font_montserrat_14);
        lv_obj_set_pos(profile, 8, 132);
        auto *dropdown = lv_dropdown_create(settings_body);
        lv_dropdown_set_options(dropdown, "Power save\nNormal\nActive");
        lv_dropdown_set_selected(dropdown,
            static_cast<uint32_t>(settings.reporting_profile));
        lv_obj_set_pos(dropdown, 8, 157);
        lv_obj_set_size(dropdown, 439, 42);
        lv_obj_add_event_cb(dropdown, reporting_changed,
                            LV_EVENT_VALUE_CHANGED, nullptr);
    } else if (tab == 2) {
        auto *title = make_label(settings_body, "Screen brightness", 0xFFFFFF,
                                 &lv_font_montserrat_18);
        lv_obj_set_pos(title, 8, 10);
        auto *slider = lv_slider_create(settings_body);
        lv_obj_set_pos(slider, 22, 69);
        lv_obj_set_size(slider, 342, 24);
        lv_slider_set_range(slider, 10, 100);
        lv_slider_set_value(slider, settings.brightness_percent, LV_ANIM_OFF);
        lv_obj_add_event_cb(slider, brightness_changed,
                            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(slider, brightness_changed,
                            LV_EVENT_RELEASED, nullptr);
        brightness_value = make_label(settings_body, "", 0x9CDBE8,
                                      &lv_font_montserrat_18);
        lv_obj_set_pos(brightness_value, 386, 64);
        lv_label_set_text_fmt(brightness_value, "%u%%",
                              settings.brightness_percent);
        auto *note = make_label(settings_body,
            "Brightness is saved on release. Battery level is unavailable on this board.",
            0x9CDBE8, &lv_font_montserrat_14);
        lv_obj_set_pos(note, 8, 125);
        lv_obj_set_width(note, 438);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    } else if (tab == 3) {
        for (std::size_t i = 0; i < settings_cat_labels.size(); ++i) {
            settings_cat_labels[i] = make_label(settings_body, "",
                0xFFFFFF, &lv_font_montserrat_14);
            lv_obj_set_pos(settings_cat_labels[i], 10,
                7 + static_cast<int>(i) * 35);
            lv_obj_set_width(settings_cat_labels[i], 430);
        }
        refresh_settings_status(bluepaws::cloud::status());
    } else {
        info_label = make_label(settings_body, "", 0xB5DAE4,
                                 &lv_font_montserrat_14);
        lv_obj_set_pos(info_label, 9, 8);
        lv_obj_set_width(info_label, 438);
        lv_label_set_long_mode(info_label, LV_LABEL_LONG_WRAP);
        refresh_settings_status(bluepaws::cloud::status());
    }
}

void refresh_settings_status(const bluepaws::cloud::Status &cloud)
{
    if (settings_overlay == nullptr ||
        lv_obj_has_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (settings_tab == 0 && settings_status != nullptr) {
        const auto snapshot = bluepaws::cloud::wifiScanSnapshot();
        if (snapshot.state == bluepaws::cloud::WifiScanState::Scanning) {
            lv_label_set_text(settings_status, "Scanning nearby Wi-Fi...");
        } else {
            lv_label_set_text_fmt(settings_status, "%s%s%s",
                cloud.wifi_station_connected ? "Connected: " : "Current: ",
                cloud.wifi_station_connected ? cloud.wifi_ssid
                    : (cloud.effective_mode == bluepaws::hub::CommunicationsMode::OffGrid
                        ? "local hotspot" : "searching"),
                cloud.wifi_station_connected ? "  (verified)" : "");
        }
        if (scan_dropdown != nullptr && snapshot.generation != scan_generation &&
            snapshot.state == bluepaws::cloud::WifiScanState::Ready) {
            scan_generation = snapshot.generation;
            scan_snapshot = snapshot;
            char options[600]{};
            size_t offset = 0;
            for (size_t i = 0; i < snapshot.count; ++i) {
                const int written = std::snprintf(options + offset,
                    sizeof(options) - offset, "%s%s%s", i ? "\n" : "",
                    snapshot.results[i].ssid,
                    snapshot.results[i].secured ? " *" : "");
                if (written < 0 || static_cast<size_t>(written) >=
                    sizeof(options) - offset) break;
                offset += static_cast<size_t>(written);
            }
            lv_dropdown_set_options(scan_dropdown,
                snapshot.count ? options : "No networks found");
        }
    }
    if (settings_tab == 3) {
        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
        for (size_t i = 0; i < settings_cat_labels.size(); ++i) {
            if (settings_cat_labels[i] == nullptr) continue;
            const auto *cat = store.at(i);
            if (cat == nullptr) {
                lv_label_set_text(settings_cat_labels[i], "");
                continue;
            }
            lv_label_set_text_fmt(settings_cat_labels[i],
                "%u. %s   %u%%   %lus ago", static_cast<unsigned>(i + 1),
                cat->name, cat->latest.battery_percent,
                static_cast<unsigned long>((now_ms - cat->latest.received_at_ms) / 1000U));
        }
    }
    if (settings_tab == 4 && info_label != nullptr) {
        lv_label_set_text_fmt(info_label,
            "ESP32-S3 Home Hub\nMode: %s\nWi-Fi: %s\nCloud: %s (%lu snapshots)\nSD map: %s\nFree PSRAM: %lu KiB",
            bluepaws::hub::communicationsModeName(cloud.effective_mode),
            cloud.wifi_station_connected ? cloud.wifi_ssid : "not connected",
            cloud.state == bluepaws::cloud::ConnectionState::Online
                ? "online" : "offline",
            static_cast<unsigned long>(cloud.successful_snapshots),
            tile_pack_available() ? "ready" : "unavailable",
            static_cast<unsigned long>(
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));
    }
}

void settings_opened(lv_event_t *)
{
    if (settings_overlay == nullptr) {
        settings_overlay = make_panel(lv_screen_active(), 0x0B1C2A, 0);
        lv_obj_set_size(settings_overlay, 480, 320);
        lv_obj_add_flag(settings_overlay, LV_OBJ_FLAG_FLOATING);
        auto *title = make_label(settings_overlay, "Home Hub settings", 0xFFFFFF,
                                 &lv_font_montserrat_18);
        lv_obj_set_pos(title, 10, 7);
        make_control(settings_overlay, LV_SYMBOL_CLOSE, 431, 3, 43, 38,
                     settings_closed);
        const char *tabs[] = {"Wi-Fi", "Hub", "Display", "Cats", "Info"};
        for (std::size_t i = 0; i < settings_tabs.size(); ++i) {
            settings_tabs[i] = make_control(settings_overlay, tabs[i],
                6 + static_cast<int>(i) * 94, 43, 90, 34,
                settings_tab_clicked);
            lv_obj_remove_event_cb(settings_tabs[i], settings_tab_clicked);
            lv_obj_add_event_cb(settings_tabs[i], settings_tab_clicked,
                LV_EVENT_CLICKED,
                reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        }
        settings_body = lv_obj_create(settings_overlay);
        lv_obj_set_pos(settings_body, 6, 83);
        lv_obj_set_size(settings_body, 468, 231);
        lv_obj_set_style_bg_color(settings_body, lv_color_hex(0x122638), 0);
        lv_obj_set_style_border_color(settings_body, lv_color_hex(0x28516E), 0);
        lv_obj_set_style_pad_all(settings_body, 0, 0);
        lv_obj_set_scroll_dir(settings_body, LV_DIR_VER);
    }
    lv_obj_remove_flag(settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(settings_overlay);
    show_settings_tab(settings_tab);
}

void create_ui(const guition_jc3248w535c_sd_info_t &sd)
{
    sd_mounted = sd.mounted;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x07141F), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    header = make_panel(screen, 0x122638, 0);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 480, kHeaderHeight);
    lv_obj_t *title = make_label(header, "Home Hub", 0xFFFFFF, &lv_font_montserrat_18);
    lv_obj_set_pos(title, 8, 8);
    network_label = make_label(header, "Starting local services", 0x7DDDE8,
                               &lv_font_montserrat_14);
    lv_obj_set_pos(network_label, 8, 34);
    lv_obj_set_width(network_label, 414);
    lv_label_set_long_mode(network_label, LV_LABEL_LONG_DOT);

    mode_dropdown = lv_dropdown_create(header);
    lv_dropdown_set_options(mode_dropdown, "Home\nPortable\nOff-Grid");
    lv_dropdown_set_selected(mode_dropdown,
        static_cast<uint32_t>(settings.communications_mode));
    lv_obj_set_pos(mode_dropdown, 150, 3);
    lv_obj_set_size(mode_dropdown, 112, 30);
    lv_obj_set_style_bg_color(mode_dropdown, lv_color_hex(0x17475B), 0);
    lv_obj_set_style_text_color(mode_dropdown, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(mode_dropdown, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_all(mode_dropdown, 4, 0);
    lv_obj_add_event_cb(mode_dropdown, mode_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    wifi_status_label = make_label(header, LV_SYMBOL_WIFI, 0x82929E,
                                   &lv_font_montserrat_18);
    lv_obj_set_pos(wifi_status_label, 280, 12);
    bluetooth_status_label = make_label(header, LV_SYMBOL_BLUETOOTH "x",
                                        0xED6C61, &lv_font_montserrat_18);
    lv_obj_set_pos(bluetooth_status_label, 315, 12);
    clock_label = make_label(header, "--:--", 0xFFFFFF, &lv_font_montserrat_14);
    lv_obj_set_pos(clock_label, 357, 15);
    make_control(header, LV_SYMBOL_SETTINGS, 431, 5, 43, 40, settings_opened);

    map_view = lv_obj_create(screen);
    lv_obj_set_pos(map_view, 0, kHeaderHeight);
    lv_obj_set_size(map_view, 480, kMapHeight);
    lv_obj_set_style_bg_color(map_view, lv_color_hex(0xDDE6D8), 0);
    lv_obj_set_style_bg_opa(map_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(map_view, 0, 0);
    lv_obj_set_style_pad_all(map_view, 0, 0);
    lv_obj_clear_flag(map_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(map_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map_view, map_pressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(map_view, map_pressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(map_view, map_released, LV_EVENT_RELEASED, nullptr);

    for (lv_obj_t *&image : tile_images) {
        image = lv_image_create(map_view);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
    for (std::size_t i = 0; i < markers.size(); ++i) {
        markers[i] = lv_obj_create(map_view);
        lv_obj_set_size(markers[i], 28, 28);
        lv_obj_set_style_radius(markers[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(markers[i], lv_color_hex(kMarkerColours[i]), 0);
        lv_obj_set_style_bg_opa(markers[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(markers[i], 2, 0);
        lv_obj_set_style_border_color(markers[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_clear_flag(markers[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *number = make_label(markers[i], "", 0xFFFFFF, &lv_font_montserrat_14);
        lv_label_set_text_fmt(number, "%u", static_cast<unsigned>(i + 1));
        lv_obj_center(number);
        lv_obj_add_flag(markers[i], LV_OBJ_FLAG_HIDDEN);
    }

    hamburger = make_control(map_view, LV_SYMBOL_LIST, 8, 8, 46, 42, drawer_toggle);
    make_control(map_view, "+", 426, 158, 44, 42, zoom_in);
    make_control(map_view, "-", 426, 208, 44, 42, zoom_out);
    make_control(map_view, LV_SYMBOL_HOME, 374, 208, 44, 42, home_clicked);
    zoom_label = make_label(map_view, "z15", 0xFFFFFF, &lv_font_montserrat_14);
    lv_obj_set_style_bg_color(zoom_label, lv_color_hex(0x102638), 0);
    lv_obj_set_style_bg_opa(zoom_label, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(zoom_label, 4, 0);
    lv_obj_set_pos(zoom_label, 429, 130);

    drawer = make_panel(map_view, 0x0B1C2A, 0);
    lv_obj_set_pos(drawer, 0, 0);
    lv_obj_set_size(drawer, kDrawerWidth, kMapHeight);
    lv_obj_set_style_bg_opa(drawer, LV_OPA_90, 0);
    lv_obj_set_style_border_side(drawer, LV_BORDER_SIDE_RIGHT, 0);
    make_control(drawer, LV_SYMBOL_LIST, 8, 8, 42, 38, drawer_toggle);
    safety_label = make_label(drawer, "Checking collars", 0x69F7D8,
                              &lv_font_montserrat_14);
    lv_obj_set_pos(safety_label, 56, 12);
    lv_obj_set_width(safety_label, 126);
    lv_label_set_long_mode(safety_label, LV_LABEL_LONG_WRAP);
    summary_label = make_label(drawer, "Loading map", 0x8BD9E8,
                               &lv_font_montserrat_14);
    lv_obj_set_pos(summary_label, 10, 51);

    drawer_list = lv_obj_create(drawer);
    lv_obj_set_pos(drawer_list, 7, 78);
    lv_obj_set_size(drawer_list, 174, 184);
    lv_obj_set_style_bg_opa(drawer_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drawer_list, 0, 0);
    lv_obj_set_style_pad_all(drawer_list, 0, 0);
    lv_obj_set_scroll_dir(drawer_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(drawer_list, LV_SCROLLBAR_MODE_ACTIVE);
    for (std::size_t i = 0; i < drawer_rows.size(); ++i) {
        drawer_rows[i] = make_panel(drawer_list, 0x102A40, 8);
        lv_obj_set_pos(drawer_rows[i], 1, static_cast<int>(i) * 60);
        lv_obj_set_size(drawer_rows[i], 168, 54);
        lv_obj_set_style_border_color(drawer_rows[i], lv_color_hex(kMarkerColours[i]), 0);
        lv_obj_t *text = make_label(drawer_rows[i], "", 0xFFFFFF,
                                    &lv_font_montserrat_14);
        lv_obj_set_pos(text, 9, 5);
        lv_obj_set_style_text_line_space(text, 4, 0);
    }

    mode_confirmation = make_panel(screen, 0x000000, 0);
    lv_obj_set_size(mode_confirmation, 480, 320);
    lv_obj_set_style_bg_opa(mode_confirmation, LV_OPA_70, 0);
    lv_obj_add_flag(mode_confirmation, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(mode_confirmation, LV_OBJ_FLAG_FLOATING);
    lv_obj_t *dialog = make_panel(mode_confirmation, 0x122638, 12);
    lv_obj_set_pos(dialog, 35, 54);
    lv_obj_set_size(dialog, 410, 210);
    mode_confirmation_title = make_label(dialog, "Switch hub mode?", 0xFFFFFF,
                                          &lv_font_montserrat_18);
    lv_obj_set_pos(mode_confirmation_title, 14, 14);
    mode_confirmation_body = make_label(dialog, "", 0xB8CAD4,
                                         &lv_font_montserrat_14);
    lv_obj_set_pos(mode_confirmation_body, 14, 52);
    lv_obj_set_width(mode_confirmation_body, 382);
    lv_label_set_long_mode(mode_confirmation_body, LV_LABEL_LONG_WRAP);
    make_control(dialog, "Cancel", 118, 158, 126, 42, mode_cancelled);
    make_control(dialog, "OK", 258, 158, 126, 42, mode_accepted);
    lv_obj_add_flag(mode_confirmation, LV_OBJ_FLAG_HIDDEN);

    set_drawer(true);
    refresh_tiles();
    update_runtime();
    lv_timer_create(refresh_timer, 1000, nullptr);
    lv_timer_create(load_next_tile, 250, nullptr);
}

void service_start_task(void *)
{
    // The display, SD card and radios share a small power stage. Complete the
    // first frame, then sequence Wi-Fi before BLE to avoid an early reset.
    vTaskDelay(pdMS_TO_TICKS(750));
    cloud_authorized = bluepaws::cloud::start(settings);
    vTaskDelay(pdMS_TO_TICKS(2500));
    if (!bluepaws::bluetooth::start(settings.bluetooth_enabled,
                                    settings.communications_mode)) {
        ESP_LOGE(kTag, "Bluetooth control failed to start");
    }
    if (!bluepaws::web::start()) ESP_LOGE(kTag, "Local dashboard failed to start");
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "BluePaws S3 Home Hub map runtime");
    ESP_LOGI(kTag, "heap=%lu bytes, PSRAM=%lu bytes",
             static_cast<unsigned long>(esp_get_free_heap_size()),
             static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    lv_display_t *display = guition_jc3248w535c_display_start();
    if (display == nullptr) {
        ESP_LOGE(kTag, "Display/touch initialization failed");
        return;
    }
    guition_jc3248w535c_sd_info_t sd{};
    const esp_err_t sd_result = guition_jc3248w535c_sd_mount(&sd);
    if (sd_result != ESP_OK) {
        ESP_LOGW(kTag, "Continuing without SD: %s", esp_err_to_name(sd_result));
    }

    std::strncpy(settings.primary.ssid, HOME_HUB_WIFI_SSID,
                 sizeof(settings.primary.ssid) - 1);
    std::strncpy(settings.primary.password, HOME_HUB_WIFI_PASSWORD,
                 sizeof(settings.primary.password) - 1);
    bluepaws::settings_store::load(settings);
    guition_jc3248w535c_backlight_set(settings.brightness_percent);
    simulator.reset(kHome, static_cast<uint32_t>(esp_timer_get_time() / 1000ULL));

    if (lvgl_port_lock(0)) {
        create_ui(sd);
        lvgl_port_unlock();
    }
    if (xTaskCreate(service_start_task, "s3_services", 4096, nullptr, 4,
                    nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Could not create staged service startup task");
    }
}
