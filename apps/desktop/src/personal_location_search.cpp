#include "personal_location_search.hpp"

#include <QLocale>
#include <QStringList>
#include <QVariantMap>
#include <QtConcurrentRun>

#include <utility>

namespace {

constexpr qsizetype MINIMUM_QUERY_LENGTH = 2;
constexpr std::size_t RESULT_LIMIT = 8;

[[nodiscard]] QString normalizedKeyComponent(const QString& value) {
    return value.simplified().toLower();
}

[[nodiscard]] QString localityKey(const GeoNamesCityIndex::CitySearchMatch& match) {
    QStringList parts{match.country_code.toLower()};
    const QString administrative_area = normalizedKeyComponent(match.administrative_area);
    if (!administrative_area.isEmpty()) {
        parts.push_back(administrative_area);
    }
    parts.push_back(normalizedKeyComponent(match.locality));
    return parts.join(QChar(0x001f));
}

[[nodiscard]] QString localizedCountryName(const GeoNamesCityIndex::CitySearchMatch& match) {
    const QLocale::Territory territory = QLocale::codeToTerritory(match.country_code);
    if (territory != QLocale::AnyTerritory) {
        const QString localized = QLocale::territoryToString(territory).trimmed();
        if (!localized.isEmpty()) {
            return localized;
        }
    }
    return match.country_name;
}

[[nodiscard]] QString localityLabel(const GeoNamesCityIndex::CitySearchMatch& match) {
    const QString country_name = localizedCountryName(match);
    QStringList parts{match.locality};
    if (!match.administrative_area.isEmpty()
        && match.administrative_area.compare(match.locality, Qt::CaseInsensitive) != 0) {
        parts.push_back(match.administrative_area);
    }
    if (!country_name.isEmpty() && country_name.compare(match.locality, Qt::CaseInsensitive) != 0
        && country_name.compare(match.administrative_area, Qt::CaseInsensitive) != 0) {
        parts.push_back(country_name);
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace

PersonalLocationSearch::PersonalLocationSearch(QString index_path, QObject* const parent) :
    QObject(parent), index_path_(std::move(index_path)) {
    connect(
        &watcher_,
        &QFutureWatcher<TaskResult>::finished,
        this,
        &PersonalLocationSearch::finishTask
    );
}

PersonalLocationSearch::~PersonalLocationSearch() {
    watcher_.waitForFinished();
}

QVariantList PersonalLocationSearch::results() const {
    return results_;
}

bool PersonalLocationSearch::busy() const noexcept {
    return task_running_;
}

QString PersonalLocationSearch::errorText() const {
    return error_text_;
}

QString PersonalLocationSearch::activeQuery() const {
    return active_query_;
}

void PersonalLocationSearch::search(const QString& query) {
    const QString normalized = query.simplified();
    if (requested_query_ == normalized && (task_running_ || active_query_ == normalized)) {
        return;
    }
    requested_query_ = normalized;
    if (normalized.size() < MINIMUM_QUERY_LENGTH) {
        ++request_id_;
        replacement_pending_ = false;
        active_query_.clear();
        results_.clear();
        error_text_.clear();
        emit stateChanged();
        return;
    }
    if (task_running_) {
        replacement_pending_ = true;
        return;
    }
    startTask();
}

void PersonalLocationSearch::clear() {
    search({});
}

PersonalLocationSearch::TaskResult PersonalLocationSearch::runTask(
    std::shared_ptr<const GeoNamesCityIndex> index,
    QString index_path,
    QString query,
    const std::uint64_t request_id
) {
    TaskResult result;
    result.index = std::move(index);
    result.query = std::move(query);
    result.request_id = request_id;
    if (!result.index) {
        result.index = GeoNamesCityIndex::load(index_path, &result.diagnostic);
    }
    if (result.index) {
        result.matches = result.index->search(result.query, RESULT_LIMIT);
    }
    return result;
}

QVariantList PersonalLocationSearch::resultVariants(
    const std::vector<GeoNamesCityIndex::CitySearchMatch>& matches
) {
    QVariantList values;
    values.reserve(static_cast<qsizetype>(matches.size()));
    for (const GeoNamesCityIndex::CitySearchMatch& match : matches) {
        values.push_back(
            QVariantMap{
                {QStringLiteral("key"), localityKey(match)},
                {QStringLiteral("label"), localityLabel(match)},
                {QStringLiteral("locality"), match.locality},
                {QStringLiteral("administrativeArea"), match.administrative_area},
                {QStringLiteral("countryCode"), match.country_code},
            }
        );
    }
    return values;
}

void PersonalLocationSearch::startTask() {
    task_running_ = true;
    replacement_pending_ = false;
    error_text_.clear();
    active_request_id_ = ++request_id_;
    emit stateChanged();
    watcher_.setFuture(
        QtConcurrent::run(runTask, index_, index_path_, requested_query_, active_request_id_)
    );
}

void PersonalLocationSearch::finishTask() {
    TaskResult result = watcher_.result();
    task_running_ = false;
    index_ = result.index;
    const bool accepted =
        result.request_id == active_request_id_ && result.query == requested_query_;
    if (accepted) {
        active_query_ = result.query;
        results_ = resultVariants(result.matches);
        error_text_ = result.index ? QString{} : tr("Offline city data could not be loaded.");
    }
    if ((replacement_pending_ || !accepted) && requested_query_.size() >= MINIMUM_QUERY_LENGTH) {
        startTask();
        return;
    }
    replacement_pending_ = false;
    emit stateChanged();
}
