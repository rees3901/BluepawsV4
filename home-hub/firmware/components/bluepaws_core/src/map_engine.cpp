#include "bluepaws/map_engine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bluepaws::map {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

uint8_t clampZoom(uint8_t zoom) {
    return std::clamp(zoom, kMinimumZoom, kMaximumZoom);
}

double wrapLongitude(double longitude) {
    double wrapped = std::fmod(longitude + 180.0, 360.0);
    if (wrapped < 0.0) wrapped += 360.0;
    return wrapped - 180.0;
}

int64_t floorToInt(double value) {
    return static_cast<int64_t>(std::floor(value));
}

uint32_t wrapTileX(int64_t x, uint32_t tile_count) {
    int64_t wrapped = x % static_cast<int64_t>(tile_count);
    if (wrapped < 0) wrapped += tile_count;
    return static_cast<uint32_t>(wrapped);
}

}  // namespace

double worldSize(uint8_t zoom) {
    return static_cast<double>(kTileSize) *
           static_cast<double>(uint32_t{1} << clampZoom(zoom));
}

GeoPoint normalize(GeoPoint point) {
    point.latitude = std::clamp(point.latitude, -kMaximumLatitude, kMaximumLatitude);
    point.longitude = wrapLongitude(point.longitude);
    return point;
}

WorldPoint project(GeoPoint point, uint8_t zoom) {
    point = normalize(point);
    const double size = worldSize(zoom);
    const double latitude_radians = point.latitude * kPi / 180.0;
    const double sin_latitude = std::sin(latitude_radians);
    return {
        (point.longitude + 180.0) / 360.0 * size,
        (0.5 - std::log((1.0 + sin_latitude) / (1.0 - sin_latitude)) /
                   (4.0 * kPi)) * size,
    };
}

GeoPoint unproject(WorldPoint point, uint8_t zoom) {
    const double size = worldSize(zoom);
    double wrapped_x = std::fmod(point.x, size);
    if (wrapped_x < 0.0) wrapped_x += size;
    const double clamped_y = std::clamp(point.y, 0.0, size);
    const double longitude = wrapped_x / size * 360.0 - 180.0;
    const double mercator = kPi - 2.0 * kPi * clamped_y / size;
    const double latitude = 180.0 / kPi * std::atan(std::sinh(mercator));
    return normalize({latitude, longitude});
}

TileId tileAt(WorldPoint point, uint8_t zoom) {
    zoom = clampZoom(zoom);
    const uint32_t tile_count = uint32_t{1} << zoom;
    const int64_t tile_x = floorToInt(point.x / kTileSize);
    const int64_t tile_y = std::clamp<int64_t>(
        floorToInt(point.y / kTileSize), 0, static_cast<int64_t>(tile_count) - 1);
    return {zoom, wrapTileX(tile_x, tile_count), static_cast<uint32_t>(tile_y)};
}

Viewport::Viewport(uint16_t width, uint16_t height, GeoPoint center, uint8_t zoom)
    : width_(width), height_(height), center_(normalize(center)), zoom_(clampZoom(zoom)) {}

void Viewport::resize(uint16_t width, uint16_t height) {
    width_ = width;
    height_ = height;
}

void Viewport::setCenter(GeoPoint center) {
    center_ = normalize(center);
}

void Viewport::setZoom(uint8_t zoom) {
    zoom_ = clampZoom(zoom);
}

void Viewport::panBy(double drag_x, double drag_y) {
    WorldPoint world_center = project(center_, zoom_);
    world_center.x -= drag_x;
    world_center.y -= drag_y;
    center_ = unproject(world_center, zoom_);
}

ScreenPoint Viewport::toScreen(GeoPoint point) const {
    const WorldPoint world_center = project(center_, zoom_);
    WorldPoint world_point = project(point, zoom_);
    const double size = worldSize(zoom_);
    double delta_x = world_point.x - world_center.x;
    if (delta_x > size / 2.0) delta_x -= size;
    if (delta_x < -size / 2.0) delta_x += size;
    return {width_ / 2.0 + delta_x, height_ / 2.0 + world_point.y - world_center.y};
}

GeoPoint Viewport::toGeo(ScreenPoint point) const {
    WorldPoint world_center = project(center_, zoom_);
    world_center.x += point.x - width_ / 2.0;
    world_center.y += point.y - height_ / 2.0;
    return unproject(world_center, zoom_);
}

TileGrid Viewport::visibleTiles(uint8_t overscan_tiles) const {
    TileGrid result{};
    if (width_ == 0 || height_ == 0) return result;

    const WorldPoint world_center = project(center_, zoom_);
    const double left = world_center.x - width_ / 2.0;
    const double top = world_center.y - height_ / 2.0;
    const int64_t first_x = floorToInt(left / kTileSize) - overscan_tiles;
    const int64_t last_x = floorToInt((left + width_ - 1.0) / kTileSize) + overscan_tiles;
    const int64_t first_y = floorToInt(top / kTileSize) - overscan_tiles;
    const int64_t last_y = floorToInt((top + height_ - 1.0) / kTileSize) + overscan_tiles;
    const uint32_t tile_count = uint32_t{1} << zoom_;

    for (int64_t y = first_y; y <= last_y; ++y) {
        if (y < 0 || y >= tile_count) continue;
        for (int64_t x = first_x; x <= last_x; ++x) {
            if (result.count == result.tiles.size()) {
                result.truncated = true;
                return result;
            }
            result.tiles[result.count++] = {
                {zoom_, wrapTileX(x, tile_count), static_cast<uint32_t>(y)},
                static_cast<int32_t>(std::lround(x * kTileSize - left)),
                static_cast<int32_t>(std::lround(y * kTileSize - top)),
            };
        }
    }
    return result;
}

FitResult fitPoints(const GeoPoint *points, std::size_t count,
                    uint16_t viewport_width, uint16_t viewport_height,
                    uint16_t padding, uint8_t minimum_zoom,
                    uint8_t maximum_zoom) {
    FitResult result{};
    minimum_zoom = clampZoom(minimum_zoom);
    maximum_zoom = clampZoom(maximum_zoom);
    if (minimum_zoom > maximum_zoom) std::swap(minimum_zoom, maximum_zoom);
    if (points == nullptr || count == 0 || viewport_width <= padding * 2U ||
        viewport_height <= padding * 2U) {
        return result;
    }

    double min_x = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double min_y = std::numeric_limits<double>::max();
    double max_y = std::numeric_limits<double>::lowest();
    for (std::size_t i = 0; i < count; ++i) {
        const WorldPoint world = project(points[i], 0);
        min_x = std::min(min_x, world.x);
        max_x = std::max(max_x, world.x);
        min_y = std::min(min_y, world.y);
        max_y = std::max(max_y, world.y);
    }

    const double available_width = viewport_width - padding * 2U;
    const double available_height = viewport_height - padding * 2U;
    const double span_x = max_x - min_x;
    const double span_y = max_y - min_y;
    double scale = std::numeric_limits<double>::infinity();
    if (span_x > 0.0) scale = std::min(scale, available_width / span_x);
    if (span_y > 0.0) scale = std::min(scale, available_height / span_y);

    uint8_t zoom = maximum_zoom;
    if (std::isfinite(scale)) {
        const double raw_zoom = std::floor(std::log2(std::max(scale, 1.0)));
        zoom = static_cast<uint8_t>(std::clamp(
            raw_zoom, static_cast<double>(minimum_zoom), static_cast<double>(maximum_zoom)));
    }

    const double zoom_scale = static_cast<double>(uint32_t{1} << zoom);
    result.center = unproject({(min_x + max_x) * 0.5 * zoom_scale,
                               (min_y + max_y) * 0.5 * zoom_scale}, zoom);
    result.zoom = zoom;
    result.valid = true;
    return result;
}

}  // namespace bluepaws::map
