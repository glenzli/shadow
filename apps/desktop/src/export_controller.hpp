#pragma once

#include "export_task_runner.hpp"
#include "localized_ui_message.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>

class QEvent;
class DesktopBackend;
class ExportBackend;
class ExportPresetStore;
class ExportWatermarkStore;

class ExportController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(int completedCount READ completedCount NOTIFY progressChanged)
    Q_PROPERTY(int failedCount READ failedCount NOTIFY progressChanged)
    Q_PROPERTY(int currentCount READ currentCount NOTIFY progressChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY progressChanged)
    Q_PROPERTY(bool cancellationRequested READ cancellationRequested NOTIFY cancellationRequestedChanged)
    Q_PROPERTY(QStringList errors READ errors NOTIFY errorsChanged)
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)
    Q_PROPERTY(QVariantList watermarks READ watermarks NOTIFY watermarksChanged)

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
    [[nodiscard]] int failedCount() const noexcept;
    [[nodiscard]] int currentCount() const noexcept;
    [[nodiscard]] int totalCount() const noexcept;
    [[nodiscard]] bool cancellationRequested() const noexcept;
    [[nodiscard]] QStringList errors() const;
    [[nodiscard]] QVariantList presets() const;
    [[nodiscard]] QVariantList watermarks() const;

    Q_INVOKABLE void startExport(
        const QVariantList& targets,
        const QUrl& destination_folder,
        const QVariantMap& options
    );
    Q_INVOKABLE void cancelExport();
    Q_INVOKABLE QString savePreset(
        const QString& name,
        const QVariantMap& options
    );
    Q_INVOKABLE QString updatePreset(
        const QString& preset_id,
        const QString& name,
        const QVariantMap& options
    );
    Q_INVOKABLE void removePreset(const QString& preset_id);
    Q_INVOKABLE QString saveWatermark(
        const QString& name,
        const QVariantMap& definition
    );
    Q_INVOKABLE QString updateWatermark(
        const QString& watermark_id,
        const QString& name,
        const QVariantMap& definition
    );
    Q_INVOKABLE void removeWatermark(const QString& watermark_id);

signals:
    void busyChanged();
    void statusTextChanged();
    void progressChanged();
    void cancellationRequestedChanged();
    void errorsChanged();
    void presetsChanged();
    void watermarksChanged();
    void exportFinished(
        int completed,
        int failed,
        bool cancelled,
        const QStringList& paths,
        const QStringList& errors
    );

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void startRecoveryDrain();
    void finishExport();
    void applyProgress(
        quint64 generation,
        int completed,
        int failed,
        int current,
        int total,
        const QString& current_title
    );
    void setStatusMessage(LocalizedUiMessage status);
    void setRawStatusText(QString status);

    std::shared_ptr<ExportBackend> export_backend_;
    std::unique_ptr<ExportPresetStore> preset_store_;
    std::unique_ptr<ExportWatermarkStore> watermark_store_;
    LocalizedUiMessage status_message_;
    QString raw_status_text_;
    int completed_count_ = 0;
    int failed_count_ = 0;
    int current_count_ = 0;
    int total_count_ = 0;
    bool cancellation_requested_ = false;
    QStringList errors_;
    std::shared_ptr<std::atomic_bool> cancellation_token_;
    quint64 export_generation_ = 0;
    QFutureWatcher<ExportTaskResult> watcher_;
};
