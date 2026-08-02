#include "geonames_library_reverse_geocoder.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "GeoNames reverse-geocoder contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool waitUntil(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return condition();
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("cities.tsv"));
    QFile file(path);
    const QByteArray payload = QByteArrayLiteral(
        "# shadow-geonames-city-index-v1\tgeonames-test\tCC-BY-4.0\n"
        "312304000\t1214737000\t24874500\tCN\tChina\tShanghai\tShanghai\n"
    );
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size()) {
        std::cerr << "could not create GeoNames provider fixture\n";
        return EXIT_FAILURE;
    }
    file.close();

    auto provider = makeGeoNamesLibraryReverseGeocoder(path);
    if (!require(provider->available(), "readable index did not make the provider available")) {
        return EXIT_FAILURE;
    }
    bool completed = false;
    std::optional<BackendLibraryPlaceResolutionResult> resolved;
    QString error;
    provider->reverseGeocode(
        {
            .latitude_e7 = 312'305'000,
            .longitude_e7 = 1'214'738'000,
            .photo_count = 2,
        },
        [&completed, &resolved, &error](
            std::optional<BackendLibraryPlaceResolutionResult> result,
            QString failure
        ) {
            completed = true;
            resolved = std::move(result);
            error = std::move(failure);
        }
    );
    if (!require(waitUntil([&completed]() { return completed; }), "async lookup did not finish")
        || !require(error.isEmpty() && resolved.has_value(), "valid lookup failed")
        || !require(resolved->country_code == QStringLiteral("CN"), "country code drifted")
        || !require(resolved->locality == QStringLiteral("Shanghai"), "city drifted")
        || !require(
            resolved->provider_id == QStringLiteral("geonames-offline")
                && resolved->provider_version == QStringLiteral("v1"),
            "provider identity is not the v1 offline contract"
        )) {
        return EXIT_FAILURE;
    }

    auto missing = makeGeoNamesLibraryReverseGeocoder(directory.filePath(QStringLiteral("missing")));
    completed = false;
    error.clear();
    missing->reverseGeocode({}, [&completed, &error](auto, QString failure) {
        completed = true;
        error = std::move(failure);
    });
    return require(!missing->available(), "missing index reported availability")
               && require(completed && !error.isEmpty(), "missing index failed silently")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
