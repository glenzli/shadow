#include "review_source_availability_monitor.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QtConcurrentRun>

#include <utility>

namespace {

constexpr int SOURCE_AVAILABILITY_REFRESH_INTERVAL_MS = 2'500;

} // namespace

ReviewSourceAvailabilityMonitor::ReviewSourceAvailabilityMonitor(
    ReviewModel& model,
    QObject* parent
) : QObject(parent), model_(&model) {
    refresh_timer_.setInterval(SOURCE_AVAILABILITY_REFRESH_INTERVAL_MS);
    refresh_timer_.setTimerType(Qt::CoarseTimer);
    connect(&refresh_timer_, &QTimer::timeout, this, &ReviewSourceAvailabilityMonitor::refreshNow);
    connect(
        &refresh_watcher_,
        &QFutureWatcher<QVector<ReviewLocalSourceAvailability>>::finished,
        this,
        &ReviewSourceAvailabilityMonitor::finishRefresh
    );
    const auto refresh_after_membership_change = [this]() { refreshNow(); };
    connect(model_, &QAbstractItemModel::modelReset, this, refresh_after_membership_change);
    connect(model_, &QAbstractItemModel::rowsInserted, this, refresh_after_membership_change);
    connect(
        model_,
        &QAbstractItemModel::dataChanged,
        this,
        [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
            if (roles.isEmpty() || roles.contains(ReviewModel::LocationIdRole)
                || roles.contains(ReviewModel::SourcePathRole)
                || roles.contains(ReviewModel::IsRemoteRole)) {
                refreshNow();
            }
        }
    );
    if (auto* const application = QCoreApplication::instance()) {
        application->installEventFilter(this);
    }
    refresh_timer_.start();
}

ReviewSourceAvailabilityMonitor::~ReviewSourceAvailabilityMonitor() {
    refresh_timer_.stop();
    if (auto* const application = QCoreApplication::instance()) {
        application->removeEventFilter(this);
    }
    refresh_watcher_.waitForFinished();
}

void ReviewSourceAvailabilityMonitor::refreshNow() {
    if (!application_active_) {
        return;
    }
    if (refresh_running_) {
        refresh_pending_ = true;
        return;
    }
    auto sources = model_->localSourceProbes();
    if (sources.isEmpty()) {
        return;
    }
    refresh_running_ = true;
    refresh_watcher_.setFuture(QtConcurrent::run(probeSources, std::move(sources)));
}

bool ReviewSourceAvailabilityMonitor::confirmNow(const ReviewLocalSourceProbe& source) {
    const bool available =
        !source.source_path.trimmed().isEmpty() && QFileInfo(source.source_path).isFile();
    static_cast<void>(model_->applyLocalSourceAvailability({
        {
            .source = source,
            .available = available,
        },
    }));
    return available;
}

QVector<ReviewLocalSourceAvailability>
ReviewSourceAvailabilityMonitor::probeSources(QVector<ReviewLocalSourceProbe> sources) {
    QVector<ReviewLocalSourceAvailability> observations;
    observations.reserve(sources.size());
    for (auto& source : sources) {
        const bool available = QFileInfo(source.source_path).isFile();
        observations.push_back({
            .source = std::move(source),
            .available = available,
        });
    }
    return observations;
}

bool ReviewSourceAvailabilityMonitor::eventFilter(QObject* const watched, QEvent* const event) {
    if (watched == QCoreApplication::instance()) {
        if (event->type() == QEvent::ApplicationDeactivate) {
            application_active_ = false;
        } else if (event->type() == QEvent::ApplicationActivate) {
            application_active_ = true;
            refreshNow();
        }
    }
    return QObject::eventFilter(watched, event);
}

void ReviewSourceAvailabilityMonitor::finishRefresh() {
    const auto observations = refresh_watcher_.result();
    refresh_running_ = false;
    if (model_->applyLocalSourceAvailability(observations)) {
        emit availabilityChanged();
    }
    if (!refresh_pending_) {
        return;
    }
    refresh_pending_ = false;
    refreshNow();
}
