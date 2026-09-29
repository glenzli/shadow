#pragma once

#include "backend/lut_export_types.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QUrl>
#include <QVariantList>

#include <optional>

/// Owns one captured export, background baking, cancellation, and atomic save.
class LutExportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool canBake READ canBake NOTIFY stateChanged)
    Q_PROPERTY(QStringList includedNodes READ includedNodes NOTIFY stateChanged)
    Q_PROPERTY(QVariantList omissions READ omissions NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString measurementText READ measurementText NOTIFY stateChanged)
    Q_PROPERTY(QString savedPath READ savedPath NOTIFY stateChanged)

public:
    using Prepare = std::function<BackendLutExportSnapshot(bool)>;
    explicit LutExportController(Prepare prepare, QObject* parent = nullptr);
    ~LutExportController() override;

    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] bool ready() const noexcept { return !result_.document.isEmpty() && !cancelled_; }
    [[nodiscard]] bool canBake() const noexcept { return snapshot_.can_bake && !busy() && !cancelled_; }
    [[nodiscard]] QStringList includedNodes() const { return snapshot_.included_nodes; }
    [[nodiscard]] QVariantList omissions() const;
    [[nodiscard]] QString errorText() const { return error_; }
    [[nodiscard]] QString measurementText() const;
    [[nodiscard]] QString savedPath() const { return saved_path_; }

    Q_INVOKABLE void prepare(bool selected_only);
    Q_INVOKABLE void bake(int size);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool save(const QUrl& destination);

signals:
    void stateChanged();

private:
    [[nodiscard]] QString omissionReason(const QString& reason) const;
    Prepare prepare_;
    BackendLutExportSnapshot snapshot_;
    BackendLutExportResult result_;
    QFutureWatcher<BackendLutExportResult> watcher_;
    QString error_;
    QString saved_path_;
    bool cancelled_ = false;
    bool busy_ = false;
    std::optional<bool> pending_prepare_;
};
