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

lv_obj_t *mode_button_label = nullptr;
lv_obj_t *network_label = nullptr;
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
std::array<lv_obj_t *, 3> drawer_rows{};

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
void map_tapped(lv_event_t *) { if (drawer_open) set_drawer(false); }

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

void mode_clicked(lv_event_t *)
{
    const uint8_t next = (static_cast<uint8_t>(settings.communications_mode) + 1U) % 3U;
    const auto previous = settings.communications_mode;
    settings.communications_mode = static_cast<bluepaws::hub::CommunicationsMode>(next);
    if (!bluepaws::settings_store::save(settings) ||
        !bluepaws::cloud::applyNetworkSettings(settings)) {
        settings.communications_mode = previous;
        bluepaws::settings_store::save(settings);
        return;
    }
    bluepaws::bluetooth::apply(settings.bluetooth_enabled,
                               settings.communications_mode);
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

    lv_label_set_text(mode_button_label,
                      bluepaws::hub::communicationsModeName(cloud.effective_mode));
    const char *link = cloud.wifi_station_connected ? cloud.wifi_ssid
        : (cloud.effective_mode == bluepaws::hub::CommunicationsMode::OffGrid
            ? "Local AP" : "Connecting");
    lv_label_set_text_fmt(network_label, "Wi-Fi %s  |  BT %s", link,
                          bluetooth.advertising ? "Home"
                          : (bluetooth.scanning ? "Scan" : "Off"));

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

void create_ui(const guition_jc3248w535c_sd_info_t &sd)
{
    sd_mounted = sd.mounted;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x07141F), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = make_panel(screen, 0x122638, 0);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 480, kHeaderHeight);
    lv_obj_t *title = make_label(header, "Home Hub", 0xFFFFFF, &lv_font_montserrat_18);
    lv_obj_set_pos(title, 10, 5);
    network_label = make_label(header, "Starting local services", 0x7DDDE8,
                               &lv_font_montserrat_14);
    lv_obj_set_pos(network_label, 10, 30);
    lv_obj_set_width(network_label, 330);
    lv_label_set_long_mode(network_label, LV_LABEL_LONG_DOT);

    lv_obj_t *mode_button = lv_button_create(header);
    lv_obj_set_pos(mode_button, 358, 8);
    lv_obj_set_size(mode_button, 112, 38);
    lv_obj_set_style_bg_color(mode_button, lv_color_hex(0x087F91), 0);
    lv_obj_set_style_radius(mode_button, 9, 0);
    lv_obj_add_event_cb(mode_button, mode_clicked, LV_EVENT_CLICKED, nullptr);
    mode_button_label = make_label(mode_button,
        bluepaws::hub::communicationsModeName(settings.communications_mode),
        0xFFFFFF, &lv_font_montserrat_14);
    lv_obj_center(mode_button_label);

    map_view = lv_obj_create(screen);
    lv_obj_set_pos(map_view, 0, kHeaderHeight);
    lv_obj_set_size(map_view, 480, kMapHeight);
    lv_obj_set_style_bg_color(map_view, lv_color_hex(0xDDE6D8), 0);
    lv_obj_set_style_bg_opa(map_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(map_view, 0, 0);
    lv_obj_set_style_pad_all(map_view, 0, 0);
    lv_obj_clear_flag(map_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(map_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(map_view, map_tapped, LV_EVENT_CLICKED, nullptr);

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

    for (std::size_t i = 0; i < drawer_rows.size(); ++i) {
        drawer_rows[i] = make_panel(drawer, 0x102A40, 8);
        lv_obj_set_pos(drawer_rows[i], 8, 78 + static_cast<int>(i) * 60);
        lv_obj_set_size(drawer_rows[i], 172, 54);
        lv_obj_set_style_border_color(drawer_rows[i], lv_color_hex(kMarkerColours[i]), 0);
        lv_obj_t *text = make_label(drawer_rows[i], "", 0xFFFFFF,
                                    &lv_font_montserrat_14);
        lv_obj_set_pos(text, 9, 5);
        lv_obj_set_style_text_line_space(text, 4, 0);
    }

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
