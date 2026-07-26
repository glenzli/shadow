#include "cache_maintenance_controller.hpp"

#include <QtConcurrentRun>

#include <exception>
#include <utility>

namespace {

[[nodiscard]] QVariantMap inventory_map(
    const BackendCacheMaintenanceInventory& inventory
) {
    return {
        {QStringLiteral("catalogLiveBlobCount"),
         QVariant::fromValue<qulonglong>(inventory.catalog_live_blob_count)},
        {QStringLiteral("cacheBlobCount"),
         QVariant::fromValue<qulonglong>(inventory.cache_blob_count)},
        {QStringLiteral("cacheBlobByteLength"),
         QVariant::fromValue<qulonglong>(inventory.cache_blob_byte_length)},
        {QStringLiteral("unknownEntryCount"),
         QVariant::fromValue<qulonglong>(inventory.unknown_entry_count)},
        {QStringLiteral("unsupportedAlgorithmCount"),
         QVariant::fromValue<uint>(inventory.unsupported_algorithm_count)},
    };
}

[[nodiscard]] QVariantMap sweep_map(
    const BackendCacheMaintenanceSweep& sweep
) {
    QVariantMap result{
        {QStringLiteral("dryRun"), sweep.dry_run},
        {QStringLiteral("catalogLiveBlobCount"),
         QVariant::fromValue<qulonglong>(sweep.catalog_live_blob_count)},
        {QStringLiteral("cacheBlobCount"),
         QVariant::fromValue<qulonglong>(sweep.cache_blob_count)},
        {QStringLiteral("cacheBlobByteLength"),
         QVariant::fromValue<qulonglong>(sweep.cache_blob_byte_length)},
        {QStringLiteral("unknownEntryCount"),
         QVariant::fromValue<qulonglong>(sweep.unknown_entry_count)},
        {QStringLiteral("unsupportedAlgorithmCount"),
         QVariant::fromValue<uint>(sweep.unsupported_algorithm_count)},
        {QStringLiteral("retainedBlobCount"),
         QVariant::fromValue<qulonglong>(sweep.retained_blob_count)},
        {QStringLiteral("recentlyProtectedBlobCount"),
         QVariant::fromValue<qulonglong>(sweep.recently_protected_blob_count)},
        {QStringLiteral("recentlyProtectedByteLength"),
         QVariant::fromValue<qulonglong>(sweep.recently_protected_byte_length)},
        {QStringLiteral("reclaimedBlobCount"),
         QVariant::fromValue<qulonglong>(sweep.reclaimed_blob_count)},
        {QStringLiteral("reclaimedByteLength"),
         QVariant::fromValue<qulonglong>(sweep.reclaimed_byte_length)},
    };
    return result;
}

[[nodiscard]] CacheMaintenanceTaskResult run_task(
    const std::shared_ptr<DesktopBackend>& backend,
    const CacheMaintenanceTaskKind kind
) {
    CacheMaintenanceTaskResult result;
    result.kind = kind;
    try {
        switch (kind) {
        case CacheMaintenanceTaskKind::Inventory:
            result.payload = inventory_map(backend->cacheMaintenanceInventory());
            break;
        case CacheMaintenanceTaskKind::Plan:
            result.payload = sweep_map(backend->planCacheMaintenanceSweep());
            break;
        case CacheMaintenanceTaskKind::Sweep:
            result.payload = sweep_map(backend->runCacheMaintenanceSweep());
            break;
        }
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

} // namespace

CacheMaintenanceController::CacheMaintenanceController(
    std::shared_ptr<DesktopBackend> backend,
    QObject* const parent
)
    : QObject(parent),
      backend_(std::move(backend)) {
    connect(
        &watcher_,
        &QFutureWatcher<CacheMaintenanceTaskResult>::finished,
        this,
        &CacheMaintenanceController::finishTask
    );
}

CacheMaintenanceController::~CacheMaintenanceController() {
    watcher_.waitForFinished();
}

bool CacheMaintenanceController::busy() const noexcept {
    return watcher_.isRunning();
}

QString CacheMaintenanceController::statusText() const {
    return status_text_;
}

QString CacheMaintenanceController::errorText() const {
    return error_text_;
}

QVariantMap CacheMaintenanceController::inventory() const {
    return inventory_;
}

QVariantMap CacheMaintenanceController::plannedSweep() const {
    return planned_sweep_;
}

QVariantMap CacheMaintenanceController::completedSweep() const {
    return completed_sweep_;
}

bool CacheMaintenanceController::hasPlan() const noexcept {
    return !planned_sweep_.isEmpty();
}

void CacheMaintenanceController::refreshInventory() {
    startTask(CacheMaintenanceTaskKind::Inventory);
}

void CacheMaintenanceController::planSafeCleanup() {
    startTask(CacheMaintenanceTaskKind::Plan);
}

void CacheMaintenanceController::runPlannedCleanup() {
    if (watcher_.isRunning() || !hasPlan()) {
        return;
    }
    startTask(CacheMaintenanceTaskKind::Sweep);
}

void CacheMaintenanceController::startTask(const CacheMaintenanceTaskKind kind) {
    if (watcher_.isRunning()) {
        return;
    }
    setErrorText({});
    switch (kind) {
    case CacheMaintenanceTaskKind::Inventory:
        setStatusText(tr("Inspecting local preview cache…"));
        break;
    case CacheMaintenanceTaskKind::Plan:
        setStatusText(tr("Preparing a safe cleanup preview…"));
        break;
    case CacheMaintenanceTaskKind::Sweep:
        setStatusText(tr("Removing only verified unused cache files…"));
        break;
    }
    emit busyChanged();
    watcher_.setFuture(QtConcurrent::run([backend = backend_, kind]() {
        return run_task(backend, kind);
    }));
}

void CacheMaintenanceController::finishTask() {
    const CacheMaintenanceTaskResult result = watcher_.result();
    emit busyChanged();
    if (!result.error.isEmpty()) {
        setErrorText(result.error);
        setStatusText(tr("Cache maintenance could not finish."));
        return;
    }

    switch (result.kind) {
    case CacheMaintenanceTaskKind::Inventory:
        inventory_ = result.payload;
        if (!planned_sweep_.isEmpty()) {
            planned_sweep_.clear();
            emit plannedSweepChanged();
        }
        setStatusText(tr("Cache usage updated."));
        emit inventoryChanged();
        break;
    case CacheMaintenanceTaskKind::Plan:
        planned_sweep_ = result.payload;
        completed_sweep_.clear();
        inventory_ = result.payload;
        setStatusText(tr("Safe cleanup preview is ready."));
        emit inventoryChanged();
        emit plannedSweepChanged();
        emit completedSweepChanged();
        break;
    case CacheMaintenanceTaskKind::Sweep:
        completed_sweep_ = result.payload;
        planned_sweep_.clear();
        inventory_ = result.payload;
        setStatusText(tr("Safe cache cleanup finished."));
        emit inventoryChanged();
        emit plannedSweepChanged();
        emit completedSweepChanged();
        break;
    }
}

void CacheMaintenanceController::setStatusText(const QString& value) {
    if (status_text_ == value) {
        return;
    }
    status_text_ = value;
    emit statusTextChanged();
}

void CacheMaintenanceController::setErrorText(const QString& value) {
    if (error_text_ == value) {
        return;
    }
    error_text_ = value;
    emit errorTextChanged();
}
