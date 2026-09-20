#pragma once

#include "desktop_backend.hpp"
#include "export_task_runner.hpp"
#include "pipeline_launch.hpp"
#include "review_photo_inspection_coordinator.hpp"
#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <memory>

class EditController;
class QApplication;

/// Owns one private editing session, including admission, photo selection,
/// persistence before export, progress, partial results and terminal policy.
class PipelineRunController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool exporting READ exporting NOTIFY changed)
    Q_PROPERTY(bool exportPending READ exportPending NOTIFY changed)
    Q_PROPERTY(bool navigationEnabled READ navigationEnabled NOTIFY changed)
    Q_PROPERTY(bool interactive READ interactive CONSTANT)
    Q_PROPERTY(bool finished READ finished NOTIFY changed)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY changed)
    Q_PROPERTY(int completedCount READ completedCount NOTIFY changed)
    Q_PROPERTY(int photoCount READ photoCount NOTIFY changed)
    Q_PROPERTY(QVariantList photos READ photos NOTIFY changed)
    Q_PROPERTY(QVariantMap captureMetadata READ captureMetadata NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(QString outputFolder READ outputFolder NOTIFY changed)
  public:
    PipelineRunController(
        std::shared_ptr<DesktopBackend> backend,
        EditController& editor,
        PipelineLaunchRequest request,
        QObject* parent = nullptr
    );
    ~PipelineRunController() override;
    bool busy() const noexcept;
    bool exporting() const noexcept {
        return export_watcher_.isRunning();
    }
    bool exportPending() const noexcept {
        return completion_requested_;
    }
    bool navigationEnabled() const noexcept;
    bool interactive() const noexcept {
        return request_.interactive;
    }
    bool finished() const noexcept {
        return finished_;
    }
    int currentIndex() const noexcept {
        return current_index_;
    }
    int completedCount() const noexcept {
        return static_cast<int>(completed_paths_.size());
    }
    int photoCount() const noexcept {
        return static_cast<int>(inputs_.size());
    }
    QVariantList photos() const;
    QVariantMap captureMetadata() const {
        return inspection_.presentation();
    }
    QString statusText() const;
    QString errorText() const {
        return error_text_;
    }
    QString outputFolder() const {
        return output_folder_;
    }

    Q_INVOKABLE void start();
    Q_INVOKABLE void addFiles(const QList<QUrl>& files);
    Q_INVOKABLE void selectPhoto(int index);
    Q_INVOKABLE bool configureExport(const QUrl& folder, const QVariantMap& options);
    Q_INVOKABLE void complete();
    Q_INVOKABLE void stopExport();
    Q_INVOKABLE void cancel();
  signals:
    void changed();

  private:
    struct AdmissionResult {
        QVector<DesktopBackend::PipelineInput> inputs;
        QStringList errors;
    };
    void admit(const QStringList& paths);
    void finishAdmission();
    void beginExport();
    void finishExport();
    void persistenceFailed();
    void finish(const QString& outcome, int exit_code, const QStringList& errors = {});

    std::shared_ptr<DesktopBackend> backend_;
    EditController& editor_;
    ReviewPhotoInspectionCoordinator inspection_;
    PipelineLaunchRequest request_;
    QVector<DesktopBackend::PipelineInput> inputs_;
    QFutureWatcher<AdmissionResult> admission_watcher_;
    QFutureWatcher<ExportTaskResult> export_watcher_;
    std::shared_ptr<std::atomic_bool> cancellation_token_;
    std::shared_ptr<std::atomic_bool> admission_cancellation_;
    QSet<QString> completed_paths_;
    QString error_text_;
    QString output_folder_;
    QString progress_title_;
    int current_index_ = -1;
    int progress_current_ = 0;
    int progress_total_ = 0;
    bool started_ = false;
    bool completion_requested_ = false;
    bool cancel_requested_ = false;
    bool terminal_ = false;
    bool finished_ = false;
};

[[nodiscard]] int runPipelineEdit(QApplication& application, const PipelineLaunchRequest& request);
