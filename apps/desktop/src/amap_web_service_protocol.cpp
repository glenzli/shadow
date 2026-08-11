#include "amap_web_service_protocol.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStringList>

#include <utility>

namespace shadow::desktop::maps {
namespace {

[[nodiscard]] QString scalarString(const QJsonValue& value) {
    return value.isString() ? value.toString().trimmed() : QString{};
}

[[nodiscard]] QString cityString(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString().trimmed();
    }
    const QJsonArray values = value.toArray();
    return values.isEmpty() ? QString{} : scalarString(values.at(0));
}

[[nodiscard]] std::optional<AmapServiceFailure> rootFailure(
    const QJsonDocument& document,
    const QJsonParseError& parse_error
) {
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return AmapServiceFailure{
            .code = QStringLiteral("malformed-response"),
            .message = QStringLiteral("AMap returned malformed JSON."),
        };
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("status")).toString() == QStringLiteral("1")) {
        return std::nullopt;
    }
    QString code = root.value(QStringLiteral("infocode")).toString().trimmed();
    if (code.isEmpty()) {
        code = QStringLiteral("provider-error");
    }
    QString message = root.value(QStringLiteral("info")).toString().trimmed();
    if (message.isEmpty()) {
        message = QStringLiteral("AMap rejected the request.");
    }
    return AmapServiceFailure{.code = code, .message = message};
}

[[nodiscard]] QString countryCode(const QString& adcode, const QString& country_name) {
    if (adcode.startsWith(QStringLiteral("81"))) {
        return QStringLiteral("HK");
    }
    if (adcode.startsWith(QStringLiteral("82"))) {
        return QStringLiteral("MO");
    }
    if (country_name.contains(QStringLiteral("中国"))
        || country_name.compare(QStringLiteral("China"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("CN");
    }
    return {};
}

[[nodiscard]] std::optional<std::pair<double, double>> coordinate(const QString& value) {
    const QStringList parts = value.split(QChar(','));
    if (parts.size() != 2) {
        return std::nullopt;
    }
    bool longitude_ok = false;
    bool latitude_ok = false;
    const double longitude = parts.at(0).toDouble(&longitude_ok);
    const double latitude = parts.at(1).toDouble(&latitude_ok);
    if (!longitude_ok || !latitude_ok || longitude < -180.0 || longitude > 180.0
        || latitude < -90.0 || latitude > 90.0) {
        return std::nullopt;
    }
    return std::pair{latitude, longitude};
}

} // namespace

AmapReverseGeocodeResponse parseAmapReverseGeocodeResponse(const QByteArray& payload) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parse_error);
    if (const auto failure = rootFailure(document, parse_error)) {
        return {.failure = failure};
    }
    const QJsonObject root = document.object();
    const QJsonObject regeocode = root.value(QStringLiteral("regeocode")).toObject();
    const QJsonObject component =
        regeocode.value(QStringLiteral("addressComponent")).toObject();
    const QString country_name = scalarString(component.value(QStringLiteral("country")));
    const QString administrative_area =
        scalarString(component.value(QStringLiteral("province")));
    const QString city = cityString(component.value(QStringLiteral("city")));
    const QString district = scalarString(component.value(QStringLiteral("district")));
    const QString adcode = scalarString(component.value(QStringLiteral("adcode")));
    const QString display_name =
        scalarString(regeocode.value(QStringLiteral("formatted_address")));
    const QString locality = !city.isEmpty()
                                 ? city
                                 : (administrative_area.endsWith(QChar(0x5e02))
                                        ? administrative_area
                                        : (!district.isEmpty() ? district
                                                               : administrative_area));
    if (country_name.isEmpty() || locality.isEmpty()) {
        return {
            .failure = AmapServiceFailure{
                .code = QStringLiteral("incomplete-address"),
                .message = QStringLiteral("AMap returned no structured locality."),
            },
        };
    }
    return {
        .result = AmapReverseGeocodeResult{
            .country_code = countryCode(adcode, country_name),
            .country_name = country_name,
            .administrative_area = administrative_area,
            .locality = locality,
            .district = district,
            .display_name = display_name,
        },
    };
}

AmapPlaceSearchResponse parseAmapPlaceSearchResponse(const QByteArray& payload) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parse_error);
    if (const auto failure = rootFailure(document, parse_error)) {
        return {.failure = failure};
    }
    std::vector<AmapPlaceSearchResult> results;
    const QJsonArray pois = document.object().value(QStringLiteral("pois")).toArray();
    results.reserve(static_cast<std::size_t>(pois.size()));
    for (const QJsonValue& value : pois) {
        const QJsonObject poi = value.toObject();
        const QString name = scalarString(poi.value(QStringLiteral("name")));
        const auto location = coordinate(scalarString(poi.value(QStringLiteral("location"))));
        if (name.isEmpty() || !location) {
            continue;
        }
        results.push_back(AmapPlaceSearchResult{
            .id = scalarString(poi.value(QStringLiteral("id"))),
            .name = name,
            .address = cityString(poi.value(QStringLiteral("address"))),
            .administrative_area = scalarString(poi.value(QStringLiteral("pname"))),
            .locality = cityString(poi.value(QStringLiteral("cityname"))),
            .gcj02_latitude = location->first,
            .gcj02_longitude = location->second,
        });
    }
    return {.results = std::move(results)};
}

} // namespace shadow::desktop::maps
