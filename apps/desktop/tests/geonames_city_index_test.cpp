#include "geonames_city_index.hpp"

#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "GeoNames city-index contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] QString writeIndex(const QTemporaryDir& directory, const QByteArray& payload) {
    const QString path = directory.filePath(QStringLiteral("cities.tsv"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size()) {
        return {};
    }
    file.close();
    return path;
}

} // namespace

int main() {
    QTemporaryDir directory;
    const QByteArray valid_index = QByteArrayLiteral(
        "# shadow-geonames-city-index-v1\tgeonames-test-2026-08-03\tCC-BY-4.0\n"
        "-338688000\t1512093000\t5312163\tAU\tAustralia\tNew South Wales\tSydney\n"
        "312304000\t1214737000\t24874500\tCN\tChina\tShanghai\tShanghai\n"
        "399042000\t1164074000\t21893095\tCN\tChina\tBeijing\tBeijing\n"
    );
    QString diagnostic;
    const auto index = GeoNamesCityIndex::load(writeIndex(directory, valid_index), &diagnostic);
    if (!require(index != nullptr, "valid index did not load")
        || !require(diagnostic.isEmpty(), "valid index produced a diagnostic")
        || !require(index->cityCount() == 3, "valid index lost a city")
        || !require(
            index->datasetVersion() == QStringLiteral("geonames-test-2026-08-03"),
            "dataset identity was not preserved"
        )) {
        return EXIT_FAILURE;
    }

    const auto shanghai = index->nearest(31.2305, 121.4738, 125.0);
    if (!require(shanghai.has_value(), "nearby city was not found")
        || !require(shanghai->country_code == QStringLiteral("CN"), "country code drifted")
        || !require(shanghai->administrative_area == QStringLiteral("Shanghai"), "area drifted")
        || !require(shanghai->locality == QStringLiteral("Shanghai"), "city drifted")
        || !require(shanghai->distance_km < 0.1, "nearest-city distance is implausible")
        || !require(
            !index->nearest(0.0, -140.0, 50.0).has_value(),
            "distant ocean coordinate invented a city"
        )) {
        return EXIT_FAILURE;
    }

    const QByteArray unsorted_index = QByteArrayLiteral(
        "# shadow-geonames-city-index-v1\ttest\tCC-BY-4.0\n"
        "10000000\t0\t1\tCN\tChina\tA\tNorth\n"
        "0\t0\t1\tCN\tChina\tA\tSouth\n"
    );
    diagnostic.clear();
    if (!require(
            !GeoNamesCityIndex::load(writeIndex(directory, unsorted_index), &diagnostic)
                && !diagnostic.isEmpty(),
            "unsorted data was not rejected"
        )) {
        return EXIT_FAILURE;
    }

    const QString production_path =
        qEnvironmentVariable("SHADOW_TEST_GEONAMES_CITY_INDEX_PATH").trimmed();
    if (!production_path.isEmpty()) {
        diagnostic.clear();
        const auto production = GeoNamesCityIndex::load(production_path, &diagnostic);
        const auto production_shanghai =
            production ? production->nearest(31.2304, 121.4737, 125.0) : std::nullopt;
        if (!require(production != nullptr, "prepared production index did not load")
            || !require(diagnostic.isEmpty(), "prepared production index produced a diagnostic")
            || !require(production->cityCount() > 200'000, "prepared production index is incomplete")
            || !require(
                production_shanghai && production_shanghai->country_code == QStringLiteral("CN"),
                "prepared production index did not resolve Shanghai"
            )) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
