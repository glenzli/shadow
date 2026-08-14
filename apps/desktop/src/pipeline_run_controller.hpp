#pragma once

#include "export_task_runner.hpp"
#include "pipeline_launch.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>

class DesktopBackend;
class EditController;
class ExportBackend;
class QApplication;

/// Owns the complete external-editor lifecycle.  It is the only owner that
/// may turn a caller request into a terminal result and process exit.
class PipelineRunController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)

  public:
    PipelineRunController(
        std::shared_ptr<DesktopBackend> backend,
        EditController& editor,
        PipelineLaunchRequest request,
        QObject* parent = nullptr
    );
    ~PipelineRunController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString statusText() const;

    Q_INVOKABLE void start();
    Q_INVOKABLE void complete();
    Q_INVOKABLE void cancel();

  signals:
    void changed();

  private:
    void beginExport();
    void finishExport();
    void finish(const QString& outcome, int exit_code, const QStringList& errors = {});

    std::shared_ptr<DesktopBackend> backend_;
    EditController& editor_;
    PipelineLaunchRequest request_;
    QString photo_id_;
    QString representation_id_;
    QString source_path_;
    QString title_;
    QString status_text_;
    bool started_ = false;
    bool completion_requested_ = false;
    bool terminal_ = false;
    std::shared_ptr<std::atomic_bool> cancellation_token_;
    QFutureWatcher<ExportTaskResult> export_watcher_;
};

[[nodiscard]] int runPipelineEdit(QApplication& application, const PipelineLaunchRequest& request);
