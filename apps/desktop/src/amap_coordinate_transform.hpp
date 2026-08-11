#pragma once

namespace shadow::desktop::maps {

struct GeographicCoordinate final {
    double latitude = 0.0;
    double longitude = 0.0;
};

/// AMap's domestic services use GCJ-02 while Shadow's Catalog owns WGS84.
/// Conversion stays at the provider boundary; persisted photo coordinates are
/// never rewritten for a particular map vendor.
[[nodiscard]] bool amapDomesticCoordinateSupported(double latitude, double longitude) noexcept;
[[nodiscard]] GeographicCoordinate wgs84ToGcj02(double latitude, double longitude) noexcept;
[[nodiscard]] GeographicCoordinate gcj02ToWgs84(double latitude, double longitude) noexcept;

} // namespace shadow::desktop::maps
