#pragma once

#include "desktop_backend.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QVariantMap>

#include <memory>

class CachePreferences;

enum class CacheMaintenanceTaskKind {
    Inventory,
    Plan,
    Sweep,
};

enum class ConfiguredLimitEnforcementStage {
    Idle,
    AwaitingInventory,
    AwaitingPlan,
    AwaitingSweep,
};

struct CacheMaintenanceTaskResult final {
    CacheMaintenanceTaskKind kind = CacheMaintenanceTaskKind::Inventory;
    QVariantMap payload;
    QString error;
};

/// Preview-first desktop facade for conservative cache maintenance.
///
/// The underlying bridge already decides which content-addressed cache blobs
/// are safe to remove. This controller deliberately keeps that policy opaque:
/// it can inspect inventory, ask for a dry-run plan, then execute only after a
/// user-facing confirmation. It never offers an unconditional cache clear.
class CacheMaintenanceController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(QVariantMap inventory READ inventory NOTIFY inventoryChanged)
    Q_PROPERTY(QVariantMap plannedSweep READ plannedSweep NOTIFY plannedSweepChanged)
    Q_PROPERTY(QVariantMap completedSweep READ completedSweep NOTIFY completedSweepChanged)
    Q_PROPERTY(bool hasPlan READ hasPlan NOTIFY plannedSweepChanged)
    Q_PROPERTY(bool overConfiguredLimit READ overConfiguredLimit NOTIFY configuredLimitStateChanged)
    Q_PROPERTY(
        qulonglong configuredLimitBytes READ configuredLimitBytes NOTIFY configuredLimitStateChanged
    )

  public:
    explicit CacheMaintenanceController(
        std::shared_ptr<DesktopBackend> backend,
        CachePreferences* preferences,
        QObject* parent = nullptr
    );
    ~CacheMaintenanceController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QVariantMap inventory() const;
    [[nodiscard]] QVariantMap plannedSweep() const;
    [[nodiscard]] QVariantMap completedSweep() const;
    [[nodiscard]] bool hasPlan() const noexcept;
    [[nodiscard]] bool overConfiguredLimit() const noexcept;
    [[nodiscard]] qulonglong configuredLimitBytes() const noexcept;

    Q_INVOKABLE void refreshInventory();
    Q_INVOKABLE void planSafeCleanup();
    Q_INVOKABLE void runPlannedCleanup();
    Q_INVOKABLE void enforceConfiguredLimit();

  signals:
    void busyChanged();
    void statusTextChanged();
    void errorTextChanged();
    void inventoryChanged();
    void plannedSweepChanged();
    void completedSweepChanged();
    void configuredLimitStateChanged();

  private:
    void startTask(CacheMaintenanceTaskKind kind);
    void finishTask();
    void setStatusText(const QString& value);
    void setErrorText(const QString& value);
    void maybeContinueConfiguredLimitEnforcement(CacheMaintenanceTaskKind completed_kind);
    [[nodiscard]] bool configuredLimitEnforcementAllowed() const noexcept;
    void cancelConfiguredLimitEnforcement() noexcept;

    std::shared_ptr<DesktopBackend> backend_;
    CachePreferences* preferences_ = nullptr;
    QFutureWatcher<CacheMaintenanceTaskResult> watcher_;
    QVariantMap inventory_;
    QVariantMap planned_sweep_;
    QVariantMap completed_sweep_;
    QString status_text_;
    QString error_text_;
    ConfiguredLimitEnforcementStage configured_limit_stage_ = ConfiguredLimitEnforcementStage::Idle;
    bool enforcement_queued_ = false;
};
