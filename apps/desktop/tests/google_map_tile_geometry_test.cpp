#include "map/google_map_tile_geometry.hpp"

#include <QCoreApplication>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using shadow::desktop::maps::layoutVisibleGoogleMapTiles;

[[nodiscard]] bool expect(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);

    bool passed = true;
    const auto world = layoutVisibleGoogleMapTiles(512, 512, 0.0, 0.0, 1.0, 256, 256);
    passed &= expect(world.request_zoom == 1, "request zoom changed");
    passed &= expect(world.tiles.size() == 4, "equatorial viewport tile set changed");

    const auto fractional = layoutVisibleGoogleMapTiles(320, 240, 31.2304, 121.4737, 8.5, 256, 256);
    passed &= expect(
        std::abs(fractional.scale - std::sqrt(2.0)) < 0.000001,
        "fractional zoom scale changed"
    );
    passed &= expect(!fractional.tiles.isEmpty(), "fractional viewport is empty");

    const auto antimeridian = layoutVisibleGoogleMapTiles(640, 320, 0.0, 179.9, 2.0, 256, 256);
    bool has_wrapped_zero = false;
    bool has_wrapped_last = false;
    for (const auto& tile : antimeridian.tiles) {
        has_wrapped_zero |= tile.id.x == 0;
        has_wrapped_last |= tile.id.x == 3;
        passed &= expect(tile.id.y >= 0 && tile.id.y < 4, "y tile escaped world");
    }
    passed &= expect(has_wrapped_zero && has_wrapped_last, "antimeridian wrapping changed");

    const auto polar = layoutVisibleGoogleMapTiles(1024, 768, 90.0, 0.0, 3.0, 256, 256);
    for (const auto& tile : polar.tiles) {
        passed &= expect(tile.id.y >= 0 && tile.id.y < 8, "polar y tile escaped world");
    }

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
