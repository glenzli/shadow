#include "personal_location_search.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Personal location-search contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] QString writeIndex(const QTemporaryDir& directory) {
    const QByteArray payload = QByteArrayLiteral(
        "# shadow-geonames-city-index-v1\tpersonal-search-test\tCC-BY-4.0\n"
        "312304000\t1214737000\t24874500\tCN\tChina\tShanghai\tShanghai\n"
        "356895000\t1396917000\t14094034\tJP\tJapan\tTokyo\tTokyo\n"
        "399042000\t1164074000\t21893095\tCN\tChina\tBeijing\tBeijing\n"
    );
    const QString path = directory.filePath(QStringLiteral("cities.tsv"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size()) {
        return {};
    }
    file.close();
    return path;
}

void drainUntilIdle(PersonalLocationSearch& search) {
    while (search.busy()) {
        QCoreApplication::processEvents();
    }
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    PersonalLocationSearch search(writeIndex(directory));

    search.search(QStringLiteral("shang china"));
    drainUntilIdle(search);
    const QVariantList shanghai = search.results();
    if (!require(shanghai.size() == 1, "offline city query did not publish one result")) {
        return EXIT_FAILURE;
    }
    const QVariantMap result = shanghai.constFirst().toMap();
    if (!require(
            result.value(QStringLiteral("key")).toString()
                == QStringLiteral("cn\u001fshanghai\u001fshanghai"),
            "search result did not use the Catalog locality identity"
        )
        || !require(
            result.value(QStringLiteral("label")).toString().contains(QStringLiteral("Shanghai")),
            "search result did not expose a city-level label"
        )) {
        return EXIT_FAILURE;
    }

    search.search(QStringLiteral("tokyo"));
    search.search(QStringLiteral("beijing"));
    drainUntilIdle(search);
    if (!require(
            search.activeQuery() == QStringLiteral("beijing") && search.results().size() == 1
                && search.results().constFirst().toMap().value(QStringLiteral("key")).toString()
                       == QStringLiteral("cn\u001fbeijing\u001fbeijing"),
            "rapid query replacement published a stale result"
        )) {
        return EXIT_FAILURE;
    }

    search.clear();
    return require(
               search.results().isEmpty() && search.activeQuery().isEmpty()
                   && search.errorText().isEmpty(),
               "clearing the query did not clear its projection"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
