#include "guition_jc3248w535c.h"

#include "bluepaws/cat_simulator.h"
#include "bluepaws/cat_store.h"
#include "bluepaws/hub_settings.h"
#include "home_hub_bluetooth.h"
#include "home_hub_cloud.h"
#include "home_hub_config.h"
#include "home_hub_settings_store.h"
#include "home_hub_web.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_lvgl_port.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace {

constexpr char kTag[] = "bluepaws_s3";
constexpr bluepaws::map::GeoPoint kHome{51.905879, -2.239486};
constexpr std::array<uint32_t, bluepaws::kMaximumCats> kMarkerColours{
    0x18B8E8, 0xFF6B52, 0x20D6A2, 0xFFB000,
    0x8B5CF6, 0x00B8C8, 0x70574E, 0x1976D2,
};

bluepaws::CatStore store;
bluepaws::CatSimulator simulator{kHome};
bluepaws::hub::Settings settings = bluepaws::hub::defaultSettings();
bool cloud_authorized = false;
uint64_t pending_control_revision = 0;
bool pending_control = false;
lv_obj_t *summary_label;
lv_obj_t *mode_button_label;
lv_obj_t *sd_label;
lv_obj_t *network_label;
std::array<lv_obj_t *, 3> cat_name_labels{};
std::array<lv_obj_t *, 3> cat_detail_labels{};
std::array<lv_obj_t *, 3> cat_badges{};

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

void update_cards()
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
        std::memcpy(proposed.display_name, control.display_name,
                    sizeof(proposed.display_name));
        std::memcpy(proposed.home_emoji, control.home_emoji,
                    sizeof(proposed.home_emoji));
        std::memcpy(proposed.portable_emoji, control.portable_emoji,
                    sizeof(proposed.portable_emoji));
        std::memcpy(proposed.marker_colour, control.marker_colour,
                    sizeof(proposed.marker_colour));
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
    if (network_label != nullptr) {
        const char *link = cloud.wifi_station_connected ? cloud.wifi_ssid
            : (cloud.effective_mode == bluepaws::hub::CommunicationsMode::OffGrid
                ? "BluePaws local AP" : "Wi-Fi connecting");
        lv_label_set_text_fmt(network_label, "%s  |  Wi-Fi %s  |  BT %s",
                              cloud_authorized ? "Cloud enabled" : "Local mode",
                              link,
                              bluetooth.advertising ? "Home beacon"
                              : (bluetooth.scanning ? "Listening" : "Off"));
    }
    for (std::size_t index = 0; index < cat_name_labels.size(); ++index) {
        const bluepaws::CatRecord *cat = store.at(index);
        if (cat == nullptr) continue;
        lv_label_set_text_fmt(cat_name_labels[index], "%u  %s",
                              static_cast<unsigned>(index + 1), cat->name);
        lv_label_set_text_fmt(cat_detail_labels[index],
                              "%u%%  %d dBm  %lus",
                              cat->latest.battery_percent,
                              cat->latest.rssi,
                              static_cast<unsigned long>(
                                  (now_ms - cat->latest.received_at_ms) / 1000U));
        lv_obj_set_style_bg_color(cat_badges[index],
                                  lv_color_hex(kMarkerColours[index]), 0);
    }
    lv_label_set_text_fmt(summary_label, "%u/%u collars available  |  %s",
                          static_cast<unsigned>(store.size()),
                          static_cast<unsigned>(bluepaws::kMaximumCats),
                          cloud_updates > 0 ? "Cloud refreshed" : "Local state");
}

void refresh_timer(lv_timer_t *)
{
    update_cards();
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
    lv_label_set_text(mode_button_label,
                      bluepaws::hub::communicationsModeName(
                          settings.communications_mode));
}

void create_ui(const guition_jc3248w535c_sd_info_t &sd)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x07141F), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = make_panel(screen, 0x122638, 0);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 480, 58);
    make_label(header, "BluePaws", 0xFFFFFF, &lv_font_montserrat_22);
    lv_obj_align(lv_obj_get_child(header, 0), LV_ALIGN_LEFT_MID, 12, -8);
    network_label = make_label(header, "Starting Wi-Fi and local services", 0x7DDDE8,
                               &lv_font_montserrat_14);
    lv_obj_set_width(network_label, 325);
    lv_label_set_long_mode(network_label, LV_LABEL_LONG_DOT);
    lv_obj_align(network_label, LV_ALIGN_LEFT_MID, 12, 15);

    lv_obj_t *mode_button = lv_button_create(header);
    lv_obj_set_size(mode_button, 118, 38);
    lv_obj_align(mode_button, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_color(mode_button, lv_color_hex(0x087F91), 0);
    lv_obj_set_style_radius(mode_button, 10, 0);
    lv_obj_add_event_cb(mode_button, mode_clicked, LV_EVENT_CLICKED, nullptr);
    mode_button_label = make_label(
        mode_button, bluepaws::hub::communicationsModeName(
                         settings.communications_mode), 0xFFFFFF,
                                   &lv_font_montserrat_14);
    lv_obj_center(mode_button_label);

    lv_obj_t *overview = make_panel(screen, 0x0D2232, 12);
    lv_obj_set_pos(overview, 8, 66);
    lv_obj_set_size(overview, 255, 246);

    lv_obj_t *safe = make_panel(overview, 0x0B4F50, 10);
    lv_obj_set_pos(safe, 8, 8);
    lv_obj_set_size(safe, 239, 48);
    lv_obj_set_style_border_color(safe, lv_color_hex(0x24D7C4), 0);
    lv_obj_set_style_border_width(safe, 2, 0);
    lv_obj_t *safe_text = make_label(safe, LV_SYMBOL_OK " Home Hub services active",
                                     0x6FFFE7, &lv_font_montserrat_18);
    lv_obj_center(safe_text);

    make_label(overview, "S3 Home Hub runtime", 0xFFFFFF, &lv_font_montserrat_18);
    lv_obj_set_pos(lv_obj_get_child(overview, 1), 12, 70);
    lv_obj_t *features = make_label(
        overview,
        LV_SYMBOL_OK " Wi-Fi + automatic fallback\n"
        LV_SYMBOL_OK " Cloud + command polling\n"
        LV_SYMBOL_OK " Local web + mDNS\n"
        LV_SYMBOL_OK " Native BLE mode policy",
        0xB8D4E2, &lv_font_montserrat_14);
    lv_obj_set_style_text_line_space(features, 9, 0);
    lv_obj_set_pos(features, 14, 102);

    summary_label = make_label(overview, "Loading collar state",
                               0x61F6E5, &lv_font_montserrat_14);
    lv_obj_align(summary_label, LV_ALIGN_BOTTOM_MID, 0, -26);
    sd_label = make_label(overview, sd.mounted ? "SD ready" : "SD not inserted",
                          sd.mounted ? 0x7BE495 : 0xF4C66A,
                          &lv_font_montserrat_14);
    lv_obj_align(sd_label, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_obj_t *cards = lv_obj_create(screen);
    lv_obj_set_pos(cards, 271, 66);
    lv_obj_set_size(cards, 201, 246);
    lv_obj_set_style_bg_opa(cards, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards, 0, 0);
    lv_obj_set_style_pad_all(cards, 0, 0);
    lv_obj_clear_flag(cards, LV_OBJ_FLAG_SCROLLABLE);

    for (std::size_t index = 0; index < 3; ++index) {
        lv_obj_t *card = make_panel(cards, 0x102A40, 10);
        lv_obj_set_pos(card, 0, static_cast<int>(index) * 82);
        lv_obj_set_size(card, 201, 76);
        lv_obj_set_style_border_color(card, lv_color_hex(0x18B8E8), 0);

        cat_badges[index] = lv_obj_create(card);
        lv_obj_set_pos(cat_badges[index], 8, 9);
        lv_obj_set_size(cat_badges[index], 38, 38);
        lv_obj_set_style_radius(cat_badges[index], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(cat_badges[index], 2, 0);
        lv_obj_set_style_border_color(cat_badges[index], lv_color_hex(0xAEEBFF), 0);
        lv_obj_clear_flag(cat_badges[index], LV_OBJ_FLAG_SCROLLABLE);

        cat_name_labels[index] = make_label(card, "", 0xFFFFFF, &lv_font_montserrat_18);
        lv_obj_set_pos(cat_name_labels[index], 16, 14);
        lv_obj_set_style_pad_left(cat_name_labels[index], 40, 0);
        cat_detail_labels[index] = make_label(card, "", 0x86D9E8, &lv_font_montserrat_14);
        lv_obj_set_pos(cat_detail_labels[index], 14, 51);
    }
    update_cards();
    lv_timer_create(refresh_timer, 1000, nullptr);
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "BluePaws compact Home Hub bring-up");
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
    cloud_authorized = bluepaws::cloud::start(settings);
    if (!bluepaws::bluetooth::start(settings.bluetooth_enabled,
                                    settings.communications_mode)) {
        ESP_LOGE(kTag, "Bluetooth control failed to start");
    }
    if (!bluepaws::web::start()) {
        ESP_LOGE(kTag, "Local dashboard failed to start");
    }
    if (lvgl_port_lock(0)) {
        create_ui(sd);
        lvgl_port_unlock();
    }
}
