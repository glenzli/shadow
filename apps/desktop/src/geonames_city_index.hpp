#pragma once

#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

/// Immutable, latitude-sorted city-level GeoNames index.
///
/// The checked text format is deliberately small and inspectable. Loading and
/// lookup are performed by the provider off the UI thread; this owner contains
/// no application, Catalog, network, or presentation state.
class GeoNamesCityIndex final {
  public:
    struct CityMatch final {
        QString country_code;
        QString country_name;
        QString administrative_area;
        QString locality;
        double distance_km = 0.0;
    };

    [[nodiscard]] static std::shared_ptr<const GeoNamesCityIndex>
    load(const QString& path, QString* diagnostic);

    [[nodiscard]] std::optional<CityMatch>
    nearest(double latitude, double longitude, double maximum_distance_km) const;
    [[nodiscard]] QString datasetVersion() const;
    [[nodiscard]] std::size_t cityCount() const noexcept;

  private:
    struct CityRecord final {
        std::int32_t latitude_e7 = 0;
        std::int32_t longitude_e7 = 0;
        std::uint32_t population = 0;
        QString country_code;
        QString country_name;
        QString administrative_area;
        QString locality;
    };

    GeoNamesCityIndex(QString dataset_version, std::vector<CityRecord> cities);

    QString dataset_version_;
    std::vector<CityRecord> cities_;
};
