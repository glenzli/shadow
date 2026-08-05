#pragma once

#include "review_model.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>

/// Keeps the loaded Review rows honest when originals change outside Shadow.
///
/// Catalog browse establishes availability for the initial page. This owner
/// periodically snapshots only the already-loaded local rows, probes their
/// paths off the UI thread, and applies exact photo/location/path results back
/// to ReviewModel. It does not scan folders, decode RAW files, or mutate the
/// Catalog; a restored original therefore becomes usable again immediately,
/// while an actual relocation still requires the explicit verified relink
/// workflow.
class ReviewSourceAvailabilityMonitor final : public QObject {
    Q_OBJECT

  public:
    explicit ReviewSourceAvailabilityMonitor(ReviewModel& model, QObject* parent = nullptr);
    ~ReviewSourceAvailabilityMonitor() override;

    void refreshNow();
    [[nodiscard]] bool confirmNow(const ReviewLocalSourceProbe& source);

  signals:
    void availabilityChanged();

  private:
    [[nodiscard]] static QVector<ReviewLocalSourceAvailability>
    probeSources(QVector<ReviewLocalSourceProbe> sources);

    bool eventFilter(QObject* watched, QEvent* event) override;
    void finishRefresh();

    ReviewModel* model_;
    bool application_active_ = true;
    bool refresh_running_ = false;
    bool refresh_pending_ = false;
    QTimer refresh_timer_;
    QFutureWatcher<QVector<ReviewLocalSourceAvailability>> refresh_watcher_;
};
