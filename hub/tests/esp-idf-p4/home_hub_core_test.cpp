#include "bluepaws/cat_simulator.h"
#include "bluepaws/cat_store.h"
#include "bluepaws/hub_settings.h"
#include "bluepaws/hub_mode_policy.h"
#include "bluepaws/map_engine.h"
#include "bluepaws/qr_payload.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

bool near(double actual, double expected, double tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

void projectionRoundTrips() {
    const bluepaws::map::GeoPoint fixture{51.5074, -0.1278};
    const auto world = bluepaws::map::project(fixture, 17);
    const auto restored = bluepaws::map::unproject(world, 17);
    assert(near(restored.latitude, fixture.latitude, 1.0e-9));
    assert(near(restored.longitude, fixture.longitude, 1.0e-9));

    const auto equator = bluepaws::map::project({0.0, 0.0}, 1);
    assert(near(equator.x, 256.0, 1.0e-9));
    assert(near(equator.y, 256.0, 1.0e-9));
    assert((bluepaws::map::tileAt(equator, 1) == bluepaws::map::TileId{1, 1, 1}));
}

void viewportPansAndLaysOutTiles() {
    bluepaws::map::Viewport viewport(800, 480, {51.5074, -0.1278}, 16);
    const auto center_screen = viewport.toScreen(viewport.center());
    assert(near(center_screen.x, 400.0, 1.0e-6));
    assert(near(center_screen.y, 240.0, 1.0e-6));

    const auto before = viewport.center();
    viewport.panBy(100.0, 80.0);
    const auto after = viewport.center();
    assert(after.longitude < before.longitude);
    assert(after.latitude > before.latitude);
    const auto restored = viewport.toGeo(viewport.toScreen({51.51, -0.12}));
    assert(near(restored.latitude, 51.51, 1.0e-8));
    assert(near(restored.longitude, -0.12, 1.0e-8));

    const auto grid = viewport.visibleTiles();
    assert(grid.count >= 20);
    assert(grid.count <= bluepaws::map::kMaximumVisibleTiles);
    assert(!grid.truncated);
    for (std::size_t i = 0; i < grid.count; ++i) {
        assert(grid.tiles[i].id.zoom == 16);
    }
}

void viewportCentersAndZoomsAtScreenPoint() {
    bluepaws::map::Viewport viewport(800, 480, {51.5074, -0.1278}, 15);
    const bluepaws::map::ScreenPoint tap{620.0, 130.0};
    const auto expected_center = viewport.toGeo(tap);

    viewport.centerAndZoom(tap, 16);

    assert(viewport.zoom() == 16);
    assert(near(viewport.center().latitude, expected_center.latitude, 1.0e-9));
    assert(near(viewport.center().longitude, expected_center.longitude, 1.0e-9));
    const auto centered = viewport.toScreen(expected_center);
    assert(near(centered.x, 400.0, 1.0e-6));
    assert(near(centered.y, 240.0, 1.0e-6));

    viewport.centerAndZoom({400.0, 240.0}, 255);
    assert(viewport.zoom() == bluepaws::map::kMaximumZoom);
}

void fitAllKeepsPointsInsidePadding() {
    const bluepaws::map::GeoPoint cats[] = {
        {51.505, -0.15}, {51.515, -0.10}, {51.49, -0.08}, {51.53, -0.18},
    };
    const auto fit = bluepaws::map::fitPoints(cats, 4, 800, 480, 48, 10, 17);
    assert(fit.valid);
    assert(fit.zoom >= 10 && fit.zoom <= 17);
    bluepaws::map::Viewport viewport(800, 480, fit.center, fit.zoom);
    for (const auto &cat : cats) {
        const auto screen = viewport.toScreen(cat);
        assert(screen.x >= 48.0 && screen.x <= 752.0);
        assert(screen.y >= 48.0 && screen.y <= 432.0);
    }
}

void storeRetainsLastValidPosition() {
    bluepaws::CatStore store;
    bluepaws::CatTelemetry first{};
    first.device_id = 1001;
    first.latitude_e7 = 515074000;
    first.longitude_e7 = -1278000;
    first.observed_at = 123;
    first.position_valid = true;
    first.source = bluepaws::TelemetrySource::LoRa;
    assert(store.apply(first) == bluepaws::ApplyResult::Added);
    assert(store.setName(1001, "Podge"));

    auto no_fix = first;
    no_fix.sequence = 2;
    no_fix.position_valid = false;
    no_fix.rssi = -108;
    assert(store.apply(no_fix) == bluepaws::ApplyResult::Updated);
    const auto *record = store.find(1001);
    assert(record != nullptr);
    assert(record->has_position);
    assert(record->last_valid_latitude_e7 == first.latitude_e7);
    assert(record->latest.rssi == -108);
    assert(record->latest.source == bluepaws::TelemetrySource::LoRa);
}

void simulatorUsesTheSharedStatePath() {
    bluepaws::CatStore store;
    bluepaws::CatSimulator simulator({51.5074, -0.1278});
    simulator.reset({51.5074, -0.1278}, 1000);
    simulator.update(1000, store);
    assert(store.size() == bluepaws::kMaximumCats);
    const auto initial_latitude = store.find(1001)->last_valid_latitude_e7;
    simulator.update(121000, store);
    assert(store.size() == bluepaws::kMaximumCats);
    assert(store.find(1001)->last_valid_latitude_e7 != initial_latitude);
    assert(store.find(1008)->latest.source == bluepaws::TelemetrySource::Simulation);

    bluepaws::CatTelemetry ninth{};
    ninth.device_id = 2000;
    assert(store.apply(ninth) == bluepaws::ApplyResult::CapacityReached);
}

void storeRejectsOlderTruth() {
    bluepaws::CatStore store;
    bluepaws::CatTelemetry cloud{};
    cloud.device_id = 1001;
    cloud.observed_at = 200;
    cloud.revision = 42;
    cloud.sequence = 7;
    cloud.latitude_e7 = 519000000;
    cloud.position_valid = true;
    cloud.source = bluepaws::TelemetrySource::Cloud;
    cloud.link = bluepaws::TelemetryLink::Lte;
    assert(store.apply(cloud) == bluepaws::ApplyResult::Added);

    auto delayed_lora = cloud;
    delayed_lora.observed_at = 199;
    delayed_lora.revision = 0;
    delayed_lora.sequence = 99;
    delayed_lora.latitude_e7 = 510000000;
    delayed_lora.source = bluepaws::TelemetrySource::LoRa;
    assert(store.apply(delayed_lora) == bluepaws::ApplyResult::IgnoredStale);
    assert(store.find(1001)->last_valid_latitude_e7 == cloud.latitude_e7);

    auto newer_cloud = cloud;
    newer_cloud.revision = 43;
    newer_cloud.latitude_e7 = 520000000;
    assert(store.apply(newer_cloud) == bluepaws::ApplyResult::Updated);
    assert(store.find(1001)->last_valid_latitude_e7 == newer_cloud.latitude_e7);

    auto newer_local = newer_cloud;
    newer_local.observed_at = 201;
    newer_local.revision = 0;
    newer_local.sequence = 8;
    newer_local.latitude_e7 = 521000000;
    newer_local.source = bluepaws::TelemetrySource::LoRa;
    newer_local.link = bluepaws::TelemetryLink::LoRa;
    assert(store.apply(newer_local) == bluepaws::ApplyResult::Updated);

    auto delayed_cloud_replay = newer_cloud;
    delayed_cloud_replay.revision = 999;
    delayed_cloud_replay.latitude_e7 = 499000000;
    assert(store.apply(delayed_cloud_replay) == bluepaws::ApplyResult::IgnoredStale);
    assert(store.find(1001)->last_valid_latitude_e7 == newer_local.latitude_e7);

    auto lte_presence = newer_cloud;
    lte_presence.observed_at = 202;
    lte_presence.revision = 1000;
    lte_presence.position_valid = false;
    lte_presence.status_code = 0;
    assert(store.apply(lte_presence) == bluepaws::ApplyResult::Updated);
    assert(store.find(1001)->last_valid_latitude_e7 == newer_local.latitude_e7);
    assert(store.find(1001)->latest.link == bluepaws::TelemetryLink::Lte);
    assert(store.setAppearance(1001, "🐈", "#1e88e5", true));
    assert(store.find(1001)->appearance.photo_available);
}

void settingsRemainSafeAndOrdered() {
    auto settings = bluepaws::hub::defaultSettings();
    assert(std::strcmp(settings.access_point_ssid, "BluePaws.local_IP:192.168.4.1") == 0);
    assert(settings.communications_mode == bluepaws::hub::CommunicationsMode::Home);
    assert(settings.overview_timeout_seconds == 120);
    assert(settings.reporting_profile == bluepaws::hub::ReportingProfile::Normal);
    assert(bluepaws::hub::reportingIntervalMs(settings.reporting_profile) == 60000U);
    assert(std::strcmp(bluepaws::hub::reportingProfileName(
                           bluepaws::hub::ReportingProfile::PowerSave),
                       "power_save") == 0);
    assert(bluepaws::hub::reportingIntervalMs(
               bluepaws::hub::ReportingProfile::PowerSave) == 180000U);
    assert(bluepaws::hub::reportingIntervalMs(
               bluepaws::hub::ReportingProfile::Active) == 30000U);
    std::strcpy(settings.access_point_ssid, "Old hotspot name");
    settings.overview_timeout_seconds = 2;
    settings.dim_timeout_seconds = 1;
    settings.screen_off_timeout_seconds = 1;
    settings.brightness_percent = 255;
    settings.dim_brightness_percent = 0;
    settings.communications_mode = static_cast<bluepaws::hub::CommunicationsMode>(99);
    settings.reporting_profile = static_cast<bluepaws::hub::ReportingProfile>(99);
    settings.display_name[0] = '\0';
    settings.home_emoji[0] = '\0';
    settings.portable_emoji[0] = '\0';
    std::strcpy(settings.marker_colour, "invalid");
    bluepaws::hub::sanitize(settings);
    assert(std::strcmp(settings.access_point_ssid, "BluePaws.local_IP:192.168.4.1") == 0);
    assert(settings.overview_timeout_seconds == 15);
    assert(settings.dim_timeout_seconds >= settings.overview_timeout_seconds);
    assert(settings.screen_off_timeout_seconds >= settings.dim_timeout_seconds);
    assert(settings.brightness_percent == 100);
    assert(settings.dim_brightness_percent == 1);
    assert(settings.communications_mode == bluepaws::hub::CommunicationsMode::Home);
    assert(settings.reporting_profile == bluepaws::hub::ReportingProfile::Normal);
    assert(std::strcmp(settings.display_name, "Home Hub") == 0);
    assert(std::strcmp(settings.home_emoji, "Home") == 0);
    assert(std::strcmp(settings.portable_emoji, "Hub") == 0);
    assert(std::strcmp(settings.marker_colour, "#38bdf8") == 0);
    assert(bluepaws::hub::validSsid("Reesnet Guest"));
    assert(!bluepaws::hub::validSsid(""));
    assert(bluepaws::hub::validPassword("password"));
    assert(!bluepaws::hub::validPassword("short"));
}

void communicationsModesUseOneDeterministicPolicy() {
    using bluepaws::hub::CommunicationsMode;
    assert(bluepaws::hub::preferredNetwork(CommunicationsMode::Home) == 0);
    assert(bluepaws::hub::modeAllowsNetwork(CommunicationsMode::Home, 0));
    assert(bluepaws::hub::modeAllowsNetwork(CommunicationsMode::Home, 1));
    assert(bluepaws::hub::effectiveModeForNetwork(0) == CommunicationsMode::Home);
    assert(bluepaws::hub::effectiveModeForNetwork(1) == CommunicationsMode::Portable);

    assert(bluepaws::hub::preferredNetwork(CommunicationsMode::Portable) == 1);
    assert(!bluepaws::hub::modeAllowsNetwork(CommunicationsMode::Portable, 0));
    assert(bluepaws::hub::modeAllowsNetwork(CommunicationsMode::Portable, 1));

    assert(!bluepaws::hub::modeAllowsNetwork(CommunicationsMode::OffGrid, 0));
    assert(!bluepaws::hub::modeAllowsNetwork(CommunicationsMode::OffGrid, 1));
    assert(!bluepaws::hub::modeAllowsNetwork(CommunicationsMode::Home, 2));
}

void relativePositionProvidesDistanceAndClockDirection() {
    const bluepaws::map::GeoPoint hub{51.8642, -2.2382};
    const auto north_east = bluepaws::hub::relativePosition(hub, {51.8652, -2.2372});
    assert(north_east.valid);
    assert(north_east.distance_metres > 100.0 && north_east.distance_metres < 150.0);
    assert(north_east.clock_hour == 1 || north_east.clock_hour == 2);
    assert(std::strcmp(north_east.cardinal, "NE") == 0);
    const auto south = bluepaws::hub::relativePosition(hub, {51.8632, -2.2382});
    assert(south.valid);
    assert(south.clock_hour == 6);
    assert(std::strcmp(south.cardinal, "S") == 0);
}

void qrPayloadsAreStrictAndEscaped() {
    bluepaws::qr::ParsedPayload parsed{};
    assert(bluepaws::qr::parse(
        "WIFI:T:WPA;S:BluePaws\\; Lab;P:cat\\:tracker\\,secret;H:true;;", parsed));
    assert(parsed.type == bluepaws::qr::PayloadType::Wifi);
    assert(std::strcmp(parsed.wifi.ssid, "BluePaws; Lab") == 0);
    assert(std::strcmp(parsed.wifi.password, "cat:tracker,secret") == 0);
    assert(parsed.wifi.hidden);
    assert(parsed.wifi.security == bluepaws::qr::WifiSecurity::Wpa);

    assert(bluepaws::qr::parse("WIFI:T:nopass;S:Guest;P:;;", parsed));
    assert(parsed.wifi.security == bluepaws::qr::WifiSecurity::Open);
    assert(!bluepaws::qr::parse("WIFI:T:WPA;S:Home;P:short;;", parsed));
    assert(!bluepaws::qr::parse("WIFI:T:WEP;S:Legacy;P:12345678;;", parsed));
    assert(!bluepaws::qr::parse("https://example.com", parsed));
    assert(bluepaws::qr::parse("BLUEPAWS:COLLAR:BP4-001122", parsed));
    assert(parsed.type == bluepaws::qr::PayloadType::Collar);
    assert(std::strcmp(parsed.collar_id, "BP4-001122") == 0);
}

}  // namespace

int main() {
    assert(std::strcmp(bluepaws::hub::communicationsModeName(
                           bluepaws::hub::CommunicationsMode::Home), "Home") == 0);
    assert(std::strcmp(bluepaws::hub::communicationsModeName(
                           bluepaws::hub::CommunicationsMode::Portable), "Portable") == 0);
    assert(std::strcmp(bluepaws::hub::communicationsModeName(
                           bluepaws::hub::CommunicationsMode::OffGrid), "Off-Grid") == 0);
    projectionRoundTrips();
    viewportPansAndLaysOutTiles();
    viewportCentersAndZoomsAtScreenPoint();
    fitAllKeepsPointsInsidePadding();
    storeRetainsLastValidPosition();
    simulatorUsesTheSharedStatePath();
    storeRejectsOlderTruth();
    settingsRemainSafeAndOrdered();
    communicationsModesUseOneDeterministicPolicy();
    relativePositionProvidesDistanceAndClockDirection();
    qrPayloadsAreStrictAndEscaped();
    std::puts("Home Hub portable core: all tests passed");
}
