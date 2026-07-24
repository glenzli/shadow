#pragma once

#include "desktop_backend.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class QSettings;

struct ExportTaskResult final {
    int requested = 0;
    int completed = 0;
    int failed = 0;
    QStringList destination_paths;
    QStringList errors;
};

class ExportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(int completedCount READ completedCount NOTIFY progressChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY progressChanged)
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)

public:
    explicit ExportController(
        std::shared_ptr<DesktopBackend> backend,
        const QString& isolated_settings_file = {},
        QObject* parent = nullptr
    );
    ~ExportController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] int completedCount() const noexcept;
    [[nodiscard]] int totalCount() const noexcept;
    [[nodiscard]] QVariantList presets() const;

    Q_INVOKABLE void startExport(
        const QVariantList& targets,
        const QUrl& destination_folder,
        const QVariantMap& options
    );
    Q_INVOKABLE QString savePreset(
        const QString& name,
        const QVariantMap& options
    );
    Q_INVOKABLE void removePreset(const QString& preset_id);

signals:
    void busyChanged();
    void statusTextChanged();
    void progressChanged();
    void presetsChanged();
    void exportFinished(int completed, int failed, const QStringList& paths);

private:
    void finishExport();
    void persistPresets();
    [[nodiscard]] static QVariantList defaultPresets();
    [[nodiscard]] static QVariantMap normalizedPreset(
        const QString& id,
        const QString& name,
        const QVariantMap& options
    );

    std::shared_ptr<DesktopBackend> backend_;
    std::unique_ptr<QSettings> settings_;
    QVariantList presets_;
    QString status_text_;
    int completed_count_ = 0;
    int total_count_ = 0;
    QFutureWatcher<ExportTaskResult> watcher_;
};
