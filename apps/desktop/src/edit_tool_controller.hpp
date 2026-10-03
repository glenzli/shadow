#pragma once
#include "desktop_backend.hpp"
#include "edit_tool_protocol.hpp"
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <atomic>
#include <memory>
#include <optional>
class EditController;
struct EditToolTaskResult final {
    BackendEditCommitReceipt commit;
    QJsonObject artifact;
    QString error, code;
};

// One bounded external proposal in the same editor owner as human input. The
// transport never writes Catalog directly and never owns a parallel draft.
class EditToolController final : public QObject {
    Q_OBJECT
  public:
    EditToolController(
        EditController& owner,
        std::shared_ptr<DesktopBackend> backend,
        QString admitted_source
    );
    ~EditToolController() override;
    void receive(const QByteArray& line);
    void disconnectClient();
    void setAdmissionError(const QString& message);
  signals:
    void reply(const QJsonObject& response);
    void shutdownRequested();

  private:
    struct Snapshot final {
        QJsonObject identity;
        BackendGradeStack stack;
        QString source;
        qint64 source_bytes = -1, source_modified_ms = -1;
        std::uint64_t photo_generation = 0, working_revision = 0, owner_epoch = 0;
    };
    bool current() const;
    bool gestureActive() const;
    bool admit(const EditToolProtocol::Request& request);
    void capture(const QString& id);
    void preview(const EditToolProtocol::Request& request);
    void apply(const EditToolProtocol::Request& request);
    void exportFile(const EditToolProtocol::Request& request);
    void finishTask();
    void invalidate();
    void reserveCommit(bool reserved);
    void operationRunning(bool running);
    void fail(const QString& id, const QString& code, const QString& message);
    EditController& owner_;
    std::shared_ptr<DesktopBackend> backend_;
    QString session_id_, admitted_source_, admission_error_, proposal_id_, candidate_node_;
    std::optional<Snapshot> snapshot_;
    BackendGradeStack candidate_;
    QJsonArray candidate_changes_;
    std::optional<EditToolProtocol::Request> task_;
    QFutureWatcher<EditToolTaskResult> watcher_;
    // 0 rendering, 1 cancelled before publication, 2 publishing, 3 finished.
    std::shared_ptr<std::atomic_int> preview_stage_;
    std::uint64_t preview_token_ = 0, owner_epoch_ = 0;
    QSet<QString> seen_ids_;
    QQueue<QString> id_order_;
};
