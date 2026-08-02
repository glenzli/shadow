#include "geonames_city_index.hpp"

#include <QByteArray>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace {

constexpr qint64 MAXIMUM_INDEX_BYTES = 64 * 1024 * 1024;
constexpr qsizetype MAXIMUM_LINE_BYTES = 4096;
constexpr std::size_t MAXIMUM_CITY_COUNT = 300'000;
constexpr double EARTH_RADIUS_KM = 6371.0088;
constexpr double LATITUDE_KM_PER_DEGREE = 110.574;
constexpr auto INDEX_MAGIC = "# shadow-geonames-city-index-v1";

[[nodiscard]] bool validIdentity(const QByteArray& value) {
    if (value.isEmpty() || value.size() > 128) {
        return false;
    }
    return std::all_of(value.cbegin(), value.cend(), [](const char byte) {
        const auto character = static_cast<unsigned char>(byte);
        return (character >= static_cast<unsigned char>('a')
                && character <= static_cast<unsigned char>('z'))
               || (character >= static_cast<unsigned char>('A')
                   && character <= static_cast<unsigned char>('Z'))
               || (character >= static_cast<unsigned char>('0')
                   && character <= static_cast<unsigned char>('9'))
               || byte == '.' || byte == '_' || byte == '-';
    });
}

[[nodiscard]] bool validCountryCode(const QByteArray& value) {
    return value.size() == 2 && value.at(0) >= 'A' && value.at(0) <= 'Z'
           && value.at(1) >= 'A' && value.at(1) <= 'Z';
}

[[nodiscard]] std::optional<QString> checkedUtf8(const QByteArray& value, const bool required) {
    if ((required && value.isEmpty()) || value.size() > 512 || value.contains('\0')) {
        return std::nullopt;
    }
    const QString decoded = QString::fromUtf8(value);
    if (decoded.contains(QChar::ReplacementCharacter) || decoded.contains(QLatin1Char('\t'))
        || decoded.contains(QLatin1Char('\n')) || decoded.contains(QLatin1Char('\r'))) {
        return std::nullopt;
    }
    return decoded;
}

[[nodiscard]] double radians(const double degrees) {
    return degrees * std::numbers::pi_v<double> / 180.0;
}

[[nodiscard]] double distanceKm(
    const double latitude_a,
    const double longitude_a,
    const double latitude_b,
    const double longitude_b
) {
    const double latitude_delta = radians(latitude_b - latitude_a);
    const double longitude_delta = radians(longitude_b - longitude_a);
    const double sine_latitude = std::sin(latitude_delta / 2.0);
    const double sine_longitude = std::sin(longitude_delta / 2.0);
    const double haversine = sine_latitude * sine_latitude
                             + std::cos(radians(latitude_a)) * std::cos(radians(latitude_b))
                                   * sine_longitude * sine_longitude;
    return 2.0 * EARTH_RADIUS_KM
           * std::asin(std::sqrt(std::clamp(haversine, 0.0, 1.0)));
}

void setDiagnostic(QString* const diagnostic, const QString& value) {
    if (diagnostic != nullptr) {
        *diagnostic = value;
    }
}

} // namespace

GeoNamesCityIndex::GeoNamesCityIndex(QString dataset_version, std::vector<CityRecord> cities) :
    dataset_version_(std::move(dataset_version)), cities_(std::move(cities)) {}

std::shared_ptr<const GeoNamesCityIndex>
GeoNamesCityIndex::load(const QString& path, QString* const diagnostic) {
    if (diagnostic != nullptr) {
        diagnostic->clear();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setDiagnostic(diagnostic, QStringLiteral("could not open the offline city index"));
        return {};
    }
    if (file.size() <= 0 || file.size() > MAXIMUM_INDEX_BYTES) {
        setDiagnostic(diagnostic, QStringLiteral("offline city index size is invalid"));
        return {};
    }

    const QByteArray header = file.readLine(MAXIMUM_LINE_BYTES + 1).trimmed();
    const QList<QByteArray> header_fields = header.split('\t');
    if (header_fields.size() != 3 || header_fields.at(0) != INDEX_MAGIC
        || !validIdentity(header_fields.at(1))
        || header_fields.at(2) != QByteArrayLiteral("CC-BY-4.0")) {
        setDiagnostic(diagnostic, QStringLiteral("offline city index header is invalid"));
        return {};
    }

    std::vector<CityRecord> cities;
    cities.reserve(200'000);
    std::int32_t previous_latitude = std::numeric_limits<std::int32_t>::min();
    while (!file.atEnd()) {
        QByteArray line = file.readLine(MAXIMUM_LINE_BYTES + 1);
        if (line.size() > MAXIMUM_LINE_BYTES || (!line.endsWith('\n') && !file.atEnd())) {
            setDiagnostic(diagnostic, QStringLiteral("offline city index contains an oversized row"));
            return {};
        }
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QList<QByteArray> fields = line.split('\t');
        if (fields.size() != 7) {
            setDiagnostic(diagnostic, QStringLiteral("offline city index row shape is invalid"));
            return {};
        }

        bool latitude_ok = false;
        bool longitude_ok = false;
        bool population_ok = false;
        const qlonglong latitude_value = fields.at(0).toLongLong(&latitude_ok);
        const qlonglong longitude_value = fields.at(1).toLongLong(&longitude_ok);
        const qulonglong population_value = fields.at(2).toULongLong(&population_ok);
        const auto country_name = checkedUtf8(fields.at(4), true);
        const auto administrative_area = checkedUtf8(fields.at(5), false);
        const auto locality = checkedUtf8(fields.at(6), true);
        if (!latitude_ok || latitude_value < -900'000'000 || latitude_value > 900'000'000
            || !longitude_ok || longitude_value < -1'800'000'000
            || longitude_value > 1'800'000'000 || !population_ok
            || population_value > std::numeric_limits<std::uint32_t>::max()
            || !validCountryCode(fields.at(3)) || !country_name || !administrative_area
            || !locality) {
            setDiagnostic(diagnostic, QStringLiteral("offline city index row value is invalid"));
            return {};
        }
        const auto latitude_e7 = static_cast<std::int32_t>(latitude_value);
        if (latitude_e7 < previous_latitude) {
            setDiagnostic(diagnostic, QStringLiteral("offline city index is not latitude sorted"));
            return {};
        }
        previous_latitude = latitude_e7;
        cities.push_back({
            .latitude_e7 = latitude_e7,
            .longitude_e7 = static_cast<std::int32_t>(longitude_value),
            .population = static_cast<std::uint32_t>(population_value),
            .country_code = QString::fromLatin1(fields.at(3)),
            .country_name = *country_name,
            .administrative_area = *administrative_area,
            .locality = *locality,
        });
        if (cities.size() > MAXIMUM_CITY_COUNT) {
            setDiagnostic(diagnostic, QStringLiteral("offline city index contains too many rows"));
            return {};
        }
    }
    if (cities.empty()) {
        setDiagnostic(diagnostic, QStringLiteral("offline city index contains no cities"));
        return {};
    }
    return std::shared_ptr<const GeoNamesCityIndex>(
        new GeoNamesCityIndex(QString::fromLatin1(header_fields.at(1)), std::move(cities))
    );
}

std::optional<GeoNamesCityIndex::CityMatch> GeoNamesCityIndex::nearest(
    const double latitude,
    const double longitude,
    const double maximum_distance_km
) const {
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90.0
        || latitude > 90.0 || longitude < -180.0 || longitude > 180.0
        || !std::isfinite(maximum_distance_km) || maximum_distance_km <= 0.0) {
        return std::nullopt;
    }
    const double latitude_window_e7 =
        std::ceil(maximum_distance_km / LATITUDE_KM_PER_DEGREE * 10'000'000.0);
    const double latitude_e7 = latitude * 10'000'000.0;
    const auto begin = std::lower_bound(
        cities_.cbegin(),
        cities_.cend(),
        latitude_e7 - latitude_window_e7,
        [](const CityRecord& city, const double value) {
            return static_cast<double>(city.latitude_e7) < value;
        }
    );

    const CityRecord* best = nullptr;
    double best_distance = maximum_distance_km;
    for (auto iterator = begin; iterator != cities_.cend(); ++iterator) {
        if (static_cast<double>(iterator->latitude_e7) > latitude_e7 + latitude_window_e7) {
            break;
        }
        const double candidate_distance = distanceKm(
            latitude,
            longitude,
            static_cast<double>(iterator->latitude_e7) / 10'000'000.0,
            static_cast<double>(iterator->longitude_e7) / 10'000'000.0
        );
        if (candidate_distance < best_distance
            || (std::abs(candidate_distance - best_distance) < 0.000'001 && best != nullptr
                && iterator->population > best->population)) {
            best = &*iterator;
            best_distance = candidate_distance;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return CityMatch{
        .country_code = best->country_code,
        .country_name = best->country_name,
        .administrative_area = best->administrative_area,
        .locality = best->locality,
        .distance_km = best_distance,
    };
}

QString GeoNamesCityIndex::datasetVersion() const {
    return dataset_version_;
}

std::size_t GeoNamesCityIndex::cityCount() const noexcept {
    return cities_.size();
}
