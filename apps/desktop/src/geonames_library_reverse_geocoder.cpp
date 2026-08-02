#include "geonames_library_reverse_geocoder.hpp"

#include "geonames_city_index.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLocale>
#include <QPointer>
#include <QtConcurrentRun>

#include <cstdint>
#include <optional>
#include <utility>

namespace {

constexpr double MAXIMUM_CITY_DISTANCE_KM = 125.0;
constexpr auto INDEX_FILE_NAME = "shadow-geonames-cities-v1.tsv";

struct LookupTaskResult final {
    std::shared_ptr<const GeoNamesCityIndex> index;
    std::optional<GeoNamesCityIndex::CityMatch> match;
    QString diagnostic;
};

[[nodiscard]] LookupTaskResult lookupTask(
    std::shared_ptr<const GeoNamesCityIndex> index,
    const QString& index_path,
    const BackendLibraryPlaceResolutionCandidate& candidate
) {
    LookupTaskResult task;
    task.index = std::move(index);
    if (!task.index) {
        task.index = GeoNamesCityIndex::load(index_path, &task.diagnostic);
    }
    if (!task.index) {
        return task;
    }
    task.match = task.index->nearest(
        static_cast<double>(candidate.latitude_e7) / 10'000'000.0,
        static_cast<double>(candidate.longitude_e7) / 10'000'000.0,
        MAXIMUM_CITY_DISTANCE_KM
    );
    return task;
}

[[nodiscard]] QString localizedCountryName(const GeoNamesCityIndex::CityMatch& match) {
    const QLocale::Territory territory = QLocale::codeToTerritory(match.country_code);
    if (territory != QLocale::AnyTerritory) {
        const QString localized = QLocale::territoryToString(territory).trimmed();
        if (!localized.isEmpty()) {
            return localized;
        }
    }
    return match.country_name;
}

class GeoNamesLibraryReverseGeocoder final : public QObject, public LibraryReverseGeocoder {
  public:
    explicit GeoNamesLibraryReverseGeocoder(QString index_path) :
        index_path_(std::move(index_path)) {}

    ~GeoNamesLibraryReverseGeocoder() override {
        stopping_ = true;
        cancel();
    }

    [[nodiscard]] bool available() const noexcept override {
        const QFileInfo info(index_path_);
        return info.isFile() && info.isReadable();
    }

    void reverseGeocode(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        Completion completion
    ) override {
        cancel();
        if (!available()) {
            completion(
                std::nullopt,
                QCoreApplication::translate(
                    "GeoNamesLibraryReverseGeocoder",
                    "Offline city data is unavailable."
                )
            );
            return;
        }
        const std::uint64_t request_generation = ++generation_;
        completion_ = std::move(completion);
        auto* const watcher = new QFutureWatcher<LookupTaskResult>(this);
        active_watcher_ = watcher;
        const QPointer<GeoNamesLibraryReverseGeocoder> guard(this);
        connect(
            watcher,
            &QFutureWatcher<LookupTaskResult>::finished,
            watcher,
            [guard, watcher, request_generation]() {
                const LookupTaskResult task = watcher->result();
                watcher->deleteLater();
                if (!guard || guard->stopping_ || request_generation != guard->generation_) {
                    return;
                }
                guard->active_watcher_ = nullptr;
                guard->index_ = task.index;
                auto completion = std::move(guard->completion_);
                guard->completion_ = {};
                if (!completion) {
                    return;
                }
                if (!task.index) {
                    completion(
                        std::nullopt,
                        QCoreApplication::translate(
                            "GeoNamesLibraryReverseGeocoder",
                            "Offline city data could not be loaded."
                        )
                    );
                    return;
                }
                if (!task.match) {
                    completion(
                        std::nullopt,
                        QCoreApplication::translate(
                            "GeoNamesLibraryReverseGeocoder",
                            "No nearby city was found in offline location data."
                        )
                    );
                    return;
                }
                const QString country_name = localizedCountryName(*task.match);
                QStringList display_parts;
                display_parts.push_back(task.match->locality);
                if (!task.match->administrative_area.isEmpty()
                    && task.match->administrative_area != task.match->locality) {
                    display_parts.push_back(task.match->administrative_area);
                }
                if (!country_name.isEmpty()) {
                    display_parts.push_back(country_name);
                }
                completion(
                    BackendLibraryPlaceResolutionResult{
                        .country_code = task.match->country_code,
                        .country_name = country_name,
                        .administrative_area = task.match->administrative_area,
                        .locality = task.match->locality,
                        .display_name = display_parts.join(QStringLiteral(", ")),
                        .provider_id = QStringLiteral("geonames-offline"),
                        .provider_version = QStringLiteral("v1"),
                        .locale = QLocale::system().bcp47Name(),
                    },
                    {}
                );
            }
        );
        watcher->setFuture(QtConcurrent::run(lookupTask, index_, index_path_, candidate));
    }

    void cancel() noexcept override {
        ++generation_;
        completion_ = {};
        if (active_watcher_ != nullptr) {
            active_watcher_->cancel();
            active_watcher_ = nullptr;
        }
    }

  private:
    QString index_path_;
    std::shared_ptr<const GeoNamesCityIndex> index_;
    QFutureWatcher<LookupTaskResult>* active_watcher_ = nullptr;
    Completion completion_;
    std::uint64_t generation_ = 0;
    bool stopping_ = false;
};

} // namespace

QString defaultGeoNamesCityIndexPath() {
    const QString override_path =
        qEnvironmentVariable("SHADOW_GEONAMES_CITY_INDEX_PATH").trimmed();
    if (!override_path.isEmpty()) {
        return QDir::cleanPath(override_path);
    }
#if defined(Q_OS_MACOS)
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../Resources/GeoNames/%1").arg(INDEX_FILE_NAME));
#else
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("GeoNames/%1").arg(INDEX_FILE_NAME));
#endif
}

std::unique_ptr<LibraryReverseGeocoder>
makeGeoNamesLibraryReverseGeocoder(const QString& index_path) {
    return std::make_unique<GeoNamesLibraryReverseGeocoder>(index_path);
}
