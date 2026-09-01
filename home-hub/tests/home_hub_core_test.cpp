#include "bluepaws/cat_simulator.h"
#include "bluepaws/cat_store.h"
#include "bluepaws/map_engine.h"

#include <cassert>
#include <cmath>
#include <cstdio>

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
    assert(store.setAppearance(1001, "🐈", "#1e88e5", true));
    assert(store.find(1001)->appearance.photo_available);
}

}  // namespace

int main() {
    projectionRoundTrips();
    viewportPansAndLaysOutTiles();
    fitAllKeepsPointsInsidePadding();
    storeRetainsLastValidPosition();
    simulatorUsesTheSharedStatePath();
    storeRejectsOlderTruth();
    std::puts("Home Hub portable core: all tests passed");
}
