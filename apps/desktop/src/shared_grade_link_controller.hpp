#pragma once

#include "adjustment_transfer_controller.hpp"
#include "shared_grade_link_operations.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QVariantList>

class EditController;

struct SharedGradeLinkEditor final {
    std::function<SharedGradeLinkSource()> capture;
    std::function<BackendBatchPhotoTarget()> current;
    std::function<AdjustmentTransferAdmission(const SharedGradeLinkSource&)> reserve_publication;
    std::function<AdjustmentTransferAdmission(const BackendPhotoEditState&)> reserve_target;
    std::function<void(
        const BackendPhotoEditState&,
        const BackendPhotoEditState&,
        const QString&,
        const QString&
    )>
        accept;
    std::function<void()> release;
    std::function<bool()> subject_masks_allowed;
    std::function<bool()> foreground_busy;
};

[[nodiscard]] SharedGradeLinkEditor sharedGradeLinkEditor(EditController& editor);

// An explicit publication has one immutable revision and its own recovery
// queue. Copy/paste clipboard and its Undo receipts remain independent.
class SharedGradeLinkController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool admissionEnabled READ admissionEnabled NOTIFY changed)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY changed)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY changed)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY changed)
    Q_PROPERTY(QString requestSourceName READ requestSourceName NOTIFY changed)
    Q_PROPERTY(QString requestSourcePhotoId READ requestSourcePhotoId NOTIFY changed)
    Q_PROPERTY(QString nodeName READ nodeName NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString publishedLayerId READ publishedLayerId NOTIFY changed)
    Q_PROPERTY(QString publishedRevisionId READ publishedRevisionId NOTIFY changed)
    Q_PROPERTY(int publishedRevisionNumber READ publishedRevisionNumber NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int completed READ completed NOTIFY changed)
    Q_PROPERTY(int failed READ failed NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)

  public:
    SharedGradeLinkController(
        SharedGradeLinkOperations operations,
        SharedGradeLinkEditor editor,
        QObject* parent = nullptr
    );
    ~SharedGradeLinkController() override;

    bool busy() const {
        return busy_;
    }
    bool admissionEnabled() const {
        return !profile_switch_pending_;
    }
    bool canRetry() const;
    bool canCancel() const {
        return busy_;
    }
    QString sourceName() const {
        return source_.name;
    }
    QString requestSourceName() const {
        return request_source_ ? request_source_->name : source_.name;
    }
    QString requestSourcePhotoId() const {
        return request_source_ ? request_source_->state.photo_id : source_.state.photo_id;
    }
    QString nodeName() const;
    QString statusText() const;
    QString publishedLayerId() const {
        return published_.layer_id;
    }
    QString publishedRevisionId() const {
        return published_.revision_id;
    }
    int publishedRevisionNumber() const {
        return static_cast<int>(published_.revision_number);
    }
    int total() const {
        return total_;
    }
    int completed() const;
    int failed() const;
    QVariantList results() const;

    Q_INVOKABLE void requestPublication();
    Q_INVOKABLE void requestLinks();
    Q_INVOKABLE void
    publish(const QString& label, const QVariantList& targets, bool allow_spatial_masks = false);
    Q_INVOKABLE void linkToPhotos(const QVariantList& targets, bool allow_spatial_masks = false);
    Q_INVOKABLE void syncSelected();
    Q_INVOKABLE void refreshCurrentLinks();
    Q_INVOKABLE void retryFailed();
    Q_INVOKABLE void cancel();
    void setProfileSwitchPending(bool pending);
    void foregroundWorkChanged();
    Q_INVOKABLE void retranslateUi() {
        emit changed();
    }

  signals:
    void changed();
    void photosChanged();
    void sharedNodesChanged();
    void presentationRequested(const QString& mode);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Row final {
        SharedGradeLinkTarget target;
        std::shared_ptr<const BackendSharedGradeNode> shared;
        QString state = QStringLiteral("pending");
        BackendSharedGradeLinkStatus issue = BackendSharedGradeLinkStatus::Ready;
        QString detail;
        LocalizedUiMessage message;
        bool source_publication = false;
    };
    struct PublicationResult final {
        BackendSharedGradePublicationReceipt receipt;
        QString error;
        bool cancelled = false;
    };
    struct SetupResult final {
        BackendSharedGradeNode shared;
        AdjustmentTransferClipboard clipboard;
        QVector<Row> rows;
        BackendSharedGradeLinkStatus issue = BackendSharedGradeLinkStatus::Ready;
        QString error;
        bool cancelled = false;
        bool has_more = false;
        QString next_photo_id;
    };
    struct PrepareResult final {
        SharedGradeLinkPrepared prepared;
        bool cancelled = false;
    };
    struct WriteResult final {
        BackendSharedGradeLinkUpdateResult receipt;
        QString error;
    };
    struct PreviewResult final {
        bool completed = false;
        QString error;
    };

    bool captureRequest(const QString& mode);
    bool refreshFrozenSourceHead(SharedGradeLinkSource& source);
    void startRefresh(bool selected_only);
    bool normalizeTargets(
        const QVariantList& targets,
        const QString& source_photo_id,
        QVector<SharedGradeLinkTarget>& normalized
    );
    void startPublication();
    void finishPublication();
    void startSetup();
    void finishSetup();
    void next();
    void finishPrepare();
    void reserveAndWrite();
    void finishWrite();
    void startPreview();
    void finishPreview();
    void finishRow(
        const QString& state,
        BackendSharedGradeLinkStatus issue,
        QString detail = {},
        LocalizedUiMessage message = {}
    );
    void finishQueue();
    void recordResult(const Row& row);
    void startSelectedPage();
    QString issueText(BackendSharedGradeLinkStatus issue) const;

    SharedGradeLinkOperations operations_;
    SharedGradeLinkEditor editor_;
    SharedGradeLinkSource source_;
    std::optional<SharedGradeLinkSource> request_source_;
    BackendSharedGradeNode published_;
    AdjustmentTransferClipboard clipboard_;
    QVector<SharedGradeLinkTarget> selected_targets_;
    QVector<Row> rows_;
    QVector<Row> result_rows_;
    QVector<Row> retry_rows_;
    QVector<int> queue_;
    QFutureWatcher<PublicationResult> publication_watcher_;
    QFutureWatcher<SetupResult> setup_watcher_;
    QFutureWatcher<PrepareResult> prepare_watcher_;
    QFutureWatcher<WriteResult> write_watcher_;
    QFutureWatcher<PreviewResult> preview_watcher_;
    SharedGradeLinkPrepared prepared_;
    BackendPhotoEditState preview_state_;
    std::shared_ptr<AdjustmentTransferCancellation> cancellation_;
    std::shared_ptr<AdjustmentTransferCancellation> preview_cancellation_;
    QString publication_label_;
    LocalizedUiMessage error_;
    QString subscription_cursor_;
    QSet<QString> selected_subscription_ids_;
    int active_row_ = -1;
    int reservation_attempts_ = 0;
    int selected_cursor_ = 0;
    int total_ = 0;
    int completed_ = 0;
    int failed_ = 0;
    bool busy_ = false;
    bool reserved_ = false;
    bool publishing_ = false;
    bool setup_running_ = false;
    bool previewing_ = false;
    bool publication_failed_ = false;
    bool setup_failed_ = false;
    bool update_subscriptions_ = false;
    bool refresh_current_ = false;
    bool refresh_selected_only_ = false;
    bool allow_spatial_masks_ = false;
    bool profile_switch_pending_ = false;
    bool setup_initialized_ = false;
    bool subscriptions_pending_ = false;
};
