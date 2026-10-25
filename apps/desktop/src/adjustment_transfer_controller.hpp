#pragma once

#include "adjustment_transfer_operations.hpp"

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QVariantList>
#include <optional>

class EditController;

enum class AdjustmentTransferAdmission { Ready, Busy, Changed };
struct AdjustmentTransferEditor final {
    std::function<AdjustmentTransferSource(bool)> capture;
    std::function<BackendBatchPhotoTarget()> current;
    std::function<AdjustmentTransferAdmission(const BackendPhotoEditState&)> reserve;
    std::function<void(const BackendPhotoEditState&, const BackendPhotoEditState&)> accept;
    std::function<void()> release;
    std::function<bool()> subject_masks_allowed;
    std::function<bool()> foreground_busy;
};
[[nodiscard]] AdjustmentTransferEditor adjustmentTransferEditor(EditController& editor);

// Profile-local clipboard and a single bounded batch. Inference prepares an
// entire photo before its CAS write; cancellation never publishes a half mask.
class AdjustmentTransferController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasClipboard READ hasClipboard NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool copying READ copying NOTIFY changed)
    Q_PROPERTY(bool admissionEnabled READ admissionEnabled NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY changed)
    Q_PROPERTY(QString sourcePhotoId READ sourcePhotoId NOTIFY changed)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY changed)
    Q_PROPERTY(QString batchSourceName READ batchSourceName NOTIFY changed)
    Q_PROPERTY(int nodeCount READ nodeCount NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int completed READ completed NOTIFY changed)
    Q_PROPERTY(int failed READ failed NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString currentPhotoName READ currentPhotoName NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)
  public:
    AdjustmentTransferController(
        AdjustmentTransferOperations operations,
        AdjustmentTransferEditor editor,
        QObject* parent = nullptr
    );
    ~AdjustmentTransferController() override;
    bool hasClipboard() const {
        return !clipboard_.document.isEmpty();
    }
    bool busy() const {
        return busy_;
    }
    bool copying() const {
        return copying_;
    }
    bool admissionEnabled() const {
        return !profile_switch_pending_;
    }
    bool canUndo() const {
        return !busy_ && admissionEnabled() && !undo_receipts_.isEmpty();
    }
    bool canRetry() const;
    QString sourcePhotoId() const {
        return clipboard_.source_photo_id;
    }
    QString sourceName() const {
        return clipboard_.source_name;
    }
    QString batchSourceName() const {
        return batch_clipboard_.source_name;
    }
    int nodeCount() const {
        return clipboard_.node_count;
    }
    int total() const {
        return static_cast<int>(rows_.size());
    }
    int completed() const;
    int failed() const;
    QString statusText() const;
    QString currentPhotoName() const;
    QVariantList results() const;

    Q_INVOKABLE void copyAdjustments(bool selected_only = false, bool allow_spatial_masks = false);
    Q_INVOKABLE void applyToPhotos(const QVariantList& targets);
    Q_INVOKABLE void pasteToCurrentPhoto();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void retryFailed();
    Q_INVOKABLE void undoBatch();
    Q_INVOKABLE void dismissResults();
    void setProfileSwitchPending(bool pending);
    void foregroundWorkChanged();
    Q_INVOKABLE void retranslateUi() {
        emit changed();
    }

  signals:
    void changed();
    void photosChanged();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Row {
        BackendBatchPhotoTarget target;
        QString title;
        QString state = QStringLiteral("pending");
        AdjustmentTransferFailure failure;
    };
    struct Receipt {
        BackendPhotoEditState before;
        QString after_head;
        QString variant;
    };
    struct AppliedIdentity {
        QString after_head;
        QString variant;
    };
    struct CopyResult {
        AdjustmentTransferClipboard clipboard;
        QString error;
    };
    struct PrepareResult {
        AdjustmentTransferPrepared prepared;
        bool unchanged = false;
    };
    struct WriteResult {
        BackendPhotoEditState state;
        QString error;
    };
    struct PreviewResult {
        bool completed = false;
        QString error;
    };
    void next();
    void finishPrepare();
    void reserveAndWrite();
    void finishWrite();
    void startPreview();
    void finishPreview();
    void finishRow(const QString& state, AdjustmentTransferFailure failure = {});
    QString issueText(AdjustmentTransferIssue issue) const;
    void startQueue();

    AdjustmentTransferOperations operations_;
    AdjustmentTransferEditor editor_;
    AdjustmentTransferClipboard clipboard_;
    AdjustmentTransferClipboard batch_clipboard_;
    QVector<Row> rows_;
    QVector<int> queue_;
    QHash<QString, AppliedIdentity> applied_;
    QHash<QString, AppliedIdentity> batch_applied_;
    QHash<QString, Receipt> undo_receipts_;
    QFutureWatcher<CopyResult> copy_watcher_;
    QFutureWatcher<PrepareResult> prepare_watcher_;
    QFutureWatcher<WriteResult> write_watcher_;
    QFutureWatcher<PreviewResult> preview_watcher_;
    std::shared_ptr<AdjustmentTransferCancellation> cancellation_;
    std::shared_ptr<AdjustmentTransferCancellation> preview_cancellation_;
    AdjustmentTransferPrepared prepared_;
    BackendPhotoEditState preview_state_;
    QString error_;
    int active_row_ = -1;
    int reservation_attempts_ = 0;
    bool busy_ = false;
    bool copying_ = false;
    bool undoing_ = false;
    bool reserved_ = false;
    bool new_batch_pending_ = false;
    bool showing_copy_result_ = false;
    bool profile_switch_pending_ = false;
    bool previewing_ = false;
    std::uint64_t clipboard_generation_ = 0;
    std::uint64_t batch_generation_ = 0;
};
