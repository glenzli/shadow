#pragma once

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

namespace shadow::desktop::maps {

struct AmapServiceFailure final {
    QString code;
    QString message;
};

struct AmapReverseGeocodeResult final {
    QString country_code;
    QString country_name;
    QString administrative_area;
    QString locality;
    QString district;
    QString display_name;
};

struct AmapPlaceSearchResult final {
    QString id;
    QString name;
    QString address;
    QString administrative_area;
    QString locality;
    double gcj02_latitude = 0.0;
    double gcj02_longitude = 0.0;
};

struct AmapReverseGeocodeResponse final {
    std::optional<AmapReverseGeocodeResult> result;
    std::optional<AmapServiceFailure> failure;
};

struct AmapPlaceSearchResponse final {
    std::vector<AmapPlaceSearchResult> results;
    std::optional<AmapServiceFailure> failure;
};

[[nodiscard]] AmapReverseGeocodeResponse parseAmapReverseGeocodeResponse(const QByteArray& payload);
[[nodiscard]] AmapPlaceSearchResponse parseAmapPlaceSearchResponse(const QByteArray& payload);

} // namespace shadow::desktop::maps
