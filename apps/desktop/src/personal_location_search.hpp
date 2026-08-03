#pragma once

#include "geonames_city_index.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <vector>

/// Device-local, asynchronous city search for Personal Profile.
///
/// This owner loads and retains the packaged GeoNames index, coalesces rapid
/// query replacement, rejects stale worker results, and exposes only bounded
/// provider-independent locality identities to QML. It never reads photos,
/// persists profile state, or performs network requests.
class PersonalLocationSearch final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList results READ results NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString activeQuery READ activeQuery NOTIFY stateChanged)

  public:
    explicit PersonalLocationSearch(QString index_path, QObject* parent = nullptr);
    ~PersonalLocationSearch() override;

    PersonalLocationSearch(const PersonalLocationSearch&) = delete;
    PersonalLocationSearch& operator=(const PersonalLocationSearch&) = delete;

    [[nodiscard]] QVariantList results() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QString activeQuery() const;

    Q_INVOKABLE void search(const QString& query);
    Q_INVOKABLE void clear();

  signals:
    void stateChanged();

  private:
    struct TaskResult final {
        std::shared_ptr<const GeoNamesCityIndex> index;
        std::vector<GeoNamesCityIndex::CitySearchMatch> matches;
        QString query;
        QString diagnostic;
        std::uint64_t request_id = 0;
    };

    [[nodiscard]] static TaskResult runTask(
        std::shared_ptr<const GeoNamesCityIndex> index,
        QString index_path,
        QString query,
        std::uint64_t request_id
    );
    [[nodiscard]] static QVariantList
    resultVariants(const std::vector<GeoNamesCityIndex::CitySearchMatch>& matches);
    void startTask();
    void finishTask();

    QString index_path_;
    std::shared_ptr<const GeoNamesCityIndex> index_;
    QFutureWatcher<TaskResult> watcher_;
    QVariantList results_;
    QString requested_query_;
    QString active_query_;
    QString error_text_;
    std::uint64_t request_id_ = 0;
    std::uint64_t active_request_id_ = 0;
    bool task_running_ = false;
    bool replacement_pending_ = false;
};
