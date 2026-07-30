#include "map/google_map_tiles_protocol.hpp"

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>

namespace {

using shadow::desktop::maps::makeSessionRequestBody;
using shadow::desktop::maps::parseGoogleMapErrorReason;
using shadow::desktop::maps::parseHttpCachePolicy;
using shadow::desktop::maps::parseSessionResponse;
using shadow::desktop::maps::parseViewportResponse;

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
    const QByteArray terrain = makeSessionRequestBody("terrain", "zh-CN", "CN");
    passed &= expect(terrain.contains("\"mapType\":\"terrain\""), "map type missing");
    passed &= expect(terrain.contains("\"layerRoadmap\":true"), "terrain road layer missing");

    QString error;
    const auto session = parseSessionResponse(
        R"({"session":"token","expiry":1900000000,"tileWidth":512,"tileHeight":512,"imageFormat":"png"})",
        &error
    );
    passed &= expect(session.has_value(), "valid session rejected");
    passed &= expect(session && session->token == "token", "session token changed");
    passed &= expect(session && session->tile_width == 512, "tile width changed");
    passed &= expect(
        !parseSessionResponse(R"({"session":""})", &error).has_value(),
        "invalid session accepted"
    );

    const auto viewport = parseViewportResponse(
        R"({"copyright":"Map data 2026","maxZoomRects":[{"maxZoom":19},{"maxZoom":17}]})",
        &error
    );
    passed &= expect(viewport.has_value(), "valid viewport rejected");
    passed &= expect(
        viewport && viewport->copyright_text == "Map data 2026",
        "viewport copyright changed"
    );
    passed &= expect(viewport && viewport->maximum_zoom == 17, "conservative maximum zoom changed");
    passed &= expect(
        !parseViewportResponse(R"({"copyright":"","maxZoomRects":[]})", &error).has_value()
            && error == "invalid-viewport-contract",
        "viewport without required attribution was accepted"
    );

    const auto cache = parseHttpCachePolicy(
        "private, max-age=120, stale-while-revalidate=\"30\", must-revalidate",
        "\"tile-v2\""
    );
    passed &= expect(cache.is_private, "private directive lost");
    passed &= expect(cache.max_age_seconds == 120, "max-age changed");
    passed &= expect(cache.stale_while_revalidate_seconds == 30, "swr changed");
    passed &= expect(cache.must_revalidate, "must-revalidate lost");
    passed &= expect(cache.etag == "\"tile-v2\"", "etag changed");
    const auto no_store = parseHttpCachePolicy("max-age=900, no-store", "\"x\"");
    passed &= expect(no_store.no_store, "no-store lost");
    passed &= expect(no_store.max_age_seconds == 0, "no-store retained max age");
    passed &= expect(no_store.etag.isEmpty(), "no-store retained etag");

    passed &= expect(
        parseGoogleMapErrorReason(R"({"error":{"details":[{"reason":"rateLimitExceeded"}]}})")
            == "rate-limited",
        "provider error reason changed"
    );

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
