#ifndef BLUEPAWS_HOME_HUB_MAP_ENGINE_H
#define BLUEPAWS_HOME_HUB_MAP_ENGINE_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace bluepaws::map {

constexpr uint16_t kTileSize = 256;
constexpr uint8_t kMinimumZoom = 0;
constexpr uint8_t kMaximumZoom = 22;
constexpr double kMaximumLatitude = 85.0511287798066;
constexpr std::size_t kMaximumVisibleTiles = 36;

struct GeoPoint {
    double latitude = 0.0;
    double longitude = 0.0;
};

struct WorldPoint {
    double x = 0.0;
    double y = 0.0;
};

struct ScreenPoint {
    double x = 0.0;
    double y = 0.0;
};

struct TileId {
    uint8_t zoom = 0;
    uint32_t x = 0;
    uint32_t y = 0;

    bool operator==(const TileId &other) const {
        return zoom == other.zoom && x == other.x && y == other.y;
    }
};

struct TilePlacement {
    TileId id{};
    int32_t screen_x = 0;
    int32_t screen_y = 0;
};

struct TileGrid {
    std::array<TilePlacement, kMaximumVisibleTiles> tiles{};
    std::size_t count = 0;
    bool truncated = false;
};

struct FitResult {
    GeoPoint center{};
    uint8_t zoom = 0;
    bool valid = false;
};

double worldSize(uint8_t zoom);
GeoPoint normalize(GeoPoint point);
WorldPoint project(GeoPoint point, uint8_t zoom);
GeoPoint unproject(WorldPoint point, uint8_t zoom);
TileId tileAt(WorldPoint point, uint8_t zoom);

class Viewport {
public:
    Viewport(uint16_t width, uint16_t height, GeoPoint center, uint8_t zoom);

    void resize(uint16_t width, uint16_t height);
    void setCenter(GeoPoint center);
    void setZoom(uint8_t zoom);
    void panBy(double drag_x, double drag_y);

    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }
    GeoPoint center() const { return center_; }
    uint8_t zoom() const { return zoom_; }

    ScreenPoint toScreen(GeoPoint point) const;
    GeoPoint toGeo(ScreenPoint point) const;
    TileGrid visibleTiles(uint8_t overscan_tiles = 1) const;

private:
    uint16_t width_;
    uint16_t height_;
    GeoPoint center_;
    uint8_t zoom_;
};

FitResult fitPoints(const GeoPoint *points, std::size_t count,
                    uint16_t viewport_width, uint16_t viewport_height,
                    uint16_t padding, uint8_t minimum_zoom,
                    uint8_t maximum_zoom);

}  // namespace bluepaws::map

#endif
