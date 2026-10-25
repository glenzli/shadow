#include "adjustment_transfer_controller.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QSet>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>
#include <stdexcept>

void AdjustmentTransferCancellation::cancel() {
    cancelled_.store(true);
    std::function<void()> callback;
    {
        const std::scoped_lock lock(mutex_);
        callback = cancel_active_;
    }
    if (callback)
        callback();
}
void AdjustmentTransferCancellation::setActive(std::function<void()> callback) {
    {
        const std::scoped_lock lock(mutex_);
        cancel_active_ = callback;
    }
    if (cancelled_.load() && callback)
        callback();
}
void AdjustmentTransferCancellation::clearActive() {
    const std::scoped_lock lock(mutex_);
    cancel_active_ = {};
}

AdjustmentTransferController::AdjustmentTransferController(
    AdjustmentTransferOperations operations,
    AdjustmentTransferEditor editor,
    QObject* parent
) : QObject(parent), operations_(std::move(operations)), editor_(std::move(editor)) {
    connect(&copy_watcher_, &QFutureWatcher<CopyResult>::finished, this, [this] {
        const auto result = copy_watcher_.result();
        clipboard_ = result.clipboard;
        error_ = result.error.isEmpty() ? QString{}
                                        : tr("Could not copy adjustments · %1").arg(result.error);
        copying_ = busy_ = false;
        emit changed();
    });
    connect(
        &prepare_watcher_,
        &QFutureWatcher<PrepareResult>::finished,
        this,
        &AdjustmentTransferController::finishPrepare
    );
    connect(
        &write_watcher_,
        &QFutureWatcher<WriteResult>::finished,
        this,
        &AdjustmentTransferController::finishWrite
    );
    connect(
        &preview_watcher_,
        &QFutureWatcher<PreviewResult>::finished,
        this,
        &AdjustmentTransferController::finishPreview
    );
    if (auto* app = QCoreApplication::instance())
        app->installEventFilter(this);
}

AdjustmentTransferController::~AdjustmentTransferController() {
    if (cancellation_)
        cancellation_->cancel();
    if (preview_cancellation_)
        preview_cancellation_->cancel();
    copy_watcher_.waitForFinished();
    prepare_watcher_.waitForFinished();
    write_watcher_.waitForFinished();
    preview_watcher_.waitForFinished();
    if (reserved_)
        editor_.release();
}

bool AdjustmentTransferController::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        emit changed();
    return QObject::eventFilter(watched, event);
}

int AdjustmentTransferController::completed() const {
    return static_cast<int>(std::count_if(rows_.cbegin(), rows_.cend(), [](const Row& row) {
        return row.state == QStringLiteral("applied") || row.state == QStringLiteral("unchanged")
               || row.state == QStringLiteral("undone");
    }));
}
int AdjustmentTransferController::failed() const {
    return static_cast<int>(std::count_if(rows_.cbegin(), rows_.cend(), [](const Row& row) {
        return row.state == QStringLiteral("failed") || row.state == QStringLiteral("cancelled");
    }));
}
bool AdjustmentTransferController::canRetry() const {
    return !busy_ && admissionEnabled() && !undoing_ && !batch_clipboard_.document.isEmpty()
           && failed() > 0;
}
QString AdjustmentTransferController::currentPhotoName() const {
    return active_row_ >= 0 ? rows_[active_row_].title : QString{};
}
QString AdjustmentTransferController::statusText() const {
    if (copying_)
        return tr("Copying adjustments…");
    if (!error_.isEmpty())
        return error_;
    if (previewing_)
        return tr("Updating previews · %1 / %2").arg(completed()).arg(total());
    if (busy_)
        return undoing_
                   ? tr("Undoing application · %1 / %2").arg(completed() + failed()).arg(total())
                   : tr("Applying adjustments · %1 / %2").arg(completed() + failed()).arg(total());
    if (!showing_copy_result_ && !rows_.isEmpty())
        return tr("%1 completed · %2 need attention").arg(completed()).arg(failed());
    if (hasClipboard())
        return tr("Copied %1 nodes from %2").arg(nodeCount()).arg(sourceName());
    return tr("Copy adjustments from an edited photo");
}
QString AdjustmentTransferController::issueText(AdjustmentTransferIssue issue) const {
    switch (issue) {
    case AdjustmentTransferIssue::None:
        return {};
    case AdjustmentTransferIssue::Cancelled:
        return tr("Cancelled · previous adjustments kept");
    case AdjustmentTransferIssue::NotFound:
        return tr("No matching subject · previous adjustments kept");
    case AdjustmentTransferIssue::Unavailable:
        return tr("Smart masks are unavailable · check AI settings and retry");
    case AdjustmentTransferIssue::UnsupportedMask:
        return tr("This mask needs a transferable selection or explicit position copying");
    case AdjustmentTransferIssue::Changed:
        return tr("This photo changed · newer adjustments kept");
    case AdjustmentTransferIssue::NodeLimit:
        return tr("Not enough room for these adjustment nodes");
    case AdjustmentTransferIssue::Failed:
        return tr("Could not apply · previous adjustments kept");
    }
    return {};
}
QVariantList AdjustmentTransferController::results() const {
    QVariantList values;
    for (const auto& row : rows_) {
        QString message = issueText(row.failure.issue);
        if (row.state == QStringLiteral("pending"))
            message = tr("Waiting");
        if (row.state == QStringLiteral("running"))
            message = tr("Preparing adjustments and masks…");
        if (row.state == QStringLiteral("applied"))
            message = tr("Applied");
        if (row.state == QStringLiteral("unchanged"))
            message = tr("Already applied");
        if (row.state == QStringLiteral("undone"))
            message = tr("Application undone");
        values.push_back(
            QVariantMap{
                {"photoId", row.target.photo_id},
                {"sourcePath", row.target.source_path},
                {"title", row.title},
                {"state", row.state},
                {"message", message},
                {"detail", row.failure.detail}
            }
        );
    }
    return values;
}

void AdjustmentTransferController::copyAdjustments(bool selected_only, bool allow_spatial_masks) {
    if (busy_ || !admissionEnabled())
        return;
    clipboard_ = {};
    ++clipboard_generation_;
    applied_.clear();
    error_.clear();
    showing_copy_result_ = true;
    try {
        auto source = editor_.capture(selected_only);
        source.allow_spatial_masks = allow_spatial_masks;
        busy_ = copying_ = true;
        copy_watcher_.setFuture(QtConcurrent::run([operation = operations_.copy, source] {
            CopyResult result;
            try {
                result.clipboard = operation(source);
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        }));
    } catch (const std::exception& error) {
        error_ = tr("Could not copy adjustments · %1").arg(QString::fromUtf8(error.what()));
    }
    emit changed();
}

void AdjustmentTransferController::applyToPhotos(const QVariantList& targets) {
    if (busy_ || !admissionEnabled() || !hasClipboard())
        return;
    QVector<Row> next_rows;
    QSet<QString> seen;
    for (const auto& value : targets) {
        const auto map = value.toMap();
        const auto id = map.value("photoId").toString();
        const auto path = map.value("sourcePath").toString();
        if (id.isEmpty() || path.isEmpty() || id == sourcePhotoId() || seen.contains(id))
            continue;
        // One batch retains bounded undo snapshots; larger selections must be split.
        if (next_rows.size() >= 1000) {
            error_ = tr("Select at most 1,000 photos per application.");
            emit changed();
            return;
        }
        seen.insert(id);
        auto title = map.value("title").toString();
        if (title.isEmpty())
            title = QFileInfo(path).fileName();
        next_rows.push_back({{id, path}, title});
    }
    if (next_rows.isEmpty())
        return;
    rows_ = std::move(next_rows);
    batch_clipboard_ = clipboard_;
    batch_generation_ = clipboard_generation_;
    batch_applied_ = applied_;
    queue_.clear();
    new_batch_pending_ = true;
    undoing_ = false;
    for (int i = 0; i < total(); ++i)
        queue_.push_back(i);
    startQueue();
}
void AdjustmentTransferController::pasteToCurrentPhoto() {
    const auto target = editor_.current();
    applyToPhotos({QVariantMap{{"photoId", target.photo_id}, {"sourcePath", target.source_path}}});
}
void AdjustmentTransferController::startQueue() {
    error_.clear();
    showing_copy_result_ = false;
    busy_ = true;
    cancellation_ = std::make_shared<AdjustmentTransferCancellation>();
    emit changed();
    QTimer::singleShot(0, this, &AdjustmentTransferController::next);
}
void AdjustmentTransferController::next() {
    if (cancellation_->cancelled()) {
        for (const auto index : queue_) {
            rows_[index].state = QStringLiteral("cancelled");
            rows_[index].failure = {AdjustmentTransferIssue::Cancelled, {}};
        }
        queue_.clear();
    }
    if (queue_.isEmpty()) {
        active_row_ = -1;
        busy_ = false;
        emit changed();
        return;
    }
    // Viewing a queued photo promotes it without restarting another photo's work.
    const auto visible = editor_.current().photo_id;
    const auto preferred = std::find_if(queue_.begin(), queue_.end(), [&](int index) {
        return rows_[index].target.photo_id == visible;
    });
    if (preferred != queue_.end())
        std::iter_swap(queue_.begin(), preferred);
    active_row_ = queue_.takeFirst();
    auto& row = rows_[active_row_];
    row.state = QStringLiteral("running");
    row.failure = {};
    auto receipt = undoing_ ? undo_receipts_.value(row.target.photo_id) : Receipt{};
    if (!undoing_) {
        const auto applied = batch_applied_.value(row.target.photo_id);
        receipt.after_head = applied.after_head;
        receipt.variant = applied.variant;
    }
    const bool has_receipt = undoing_ || batch_applied_.contains(row.target.photo_id);
    if (!has_receipt && batch_clipboard_.requires_inference
        && (!editor_.subject_masks_allowed || !editor_.subject_masks_allowed())) {
        finishRow(QStringLiteral("failed"), {AdjustmentTransferIssue::Unavailable, {}});
        return;
    }
    prepare_watcher_.setFuture(
        QtConcurrent::run([operations = operations_,
                           clipboard = batch_clipboard_,
                           target = row.target,
                           cancellation = cancellation_,
                           receipt,
                           has_receipt,
                           undoing = undoing_] {
            PrepareResult result;
            try {
                if (has_receipt) {
                    auto current = operations.load(target);
                    if (current.base_commit_id != receipt.after_head
                        || current.active_variant_id != receipt.variant) {
                        result.prepared.failure = {AdjustmentTransferIssue::Changed, {}};
                    } else if (undoing) {
                        result.prepared.before = std::move(current);
                        result.prepared.after = receipt.before.grade_stack;
                        result.prepared.content_anchor = receipt.before.base_commit_id;
                    } else {
                        result.unchanged = true;
                    }
                } else
                    result.prepared = operations.prepare(clipboard, target, cancellation);
            } catch (const std::exception& error) {
                result.prepared.failure = {
                    AdjustmentTransferIssue::Failed,
                    QString::fromUtf8(error.what())
                };
            }
            return result;
        })
    );
    emit changed();
}
void AdjustmentTransferController::finishPrepare() {
    // Consume the future's only result so a cancelled/rejected photo releases
    // its retained Rust plan immediately, instead of at the next prepare.
    auto result = prepare_watcher_.future().takeResult();
    if (cancellation_->cancelled()) {
        finishRow(QStringLiteral("cancelled"), {AdjustmentTransferIssue::Cancelled, {}});
    } else if (result.unchanged) {
        finishRow(QStringLiteral("unchanged"));
    } else if (result.prepared.failure.issue != AdjustmentTransferIssue::None) {
        finishRow(QStringLiteral("failed"), result.prepared.failure);
    } else {
        prepared_ = std::move(result.prepared);
        reservation_attempts_ = 0;
        reserveAndWrite();
    }
}
void AdjustmentTransferController::reserveAndWrite() {
    if (cancellation_->cancelled()) {
        finishRow(QStringLiteral("cancelled"), {AdjustmentTransferIssue::Cancelled, {}});
        return;
    }
    if (!undoing_ && batch_clipboard_.requires_inference
        && (!editor_.subject_masks_allowed || !editor_.subject_masks_allowed())) {
        finishRow(QStringLiteral("failed"), {AdjustmentTransferIssue::Unavailable, {}});
        return;
    }
    const auto admission = editor_.reserve(prepared_.before);
    if (admission == AdjustmentTransferAdmission::Busy && ++reservation_attempts_ < 100) {
        QTimer::singleShot(50, this, &AdjustmentTransferController::reserveAndWrite);
        return;
    }
    if (admission != AdjustmentTransferAdmission::Ready) {
        finishRow(QStringLiteral("failed"), {AdjustmentTransferIssue::Changed, {}});
        return;
    }
    reserved_ = true;
    write_watcher_.setFuture(
        QtConcurrent::run([operation = operations_.write, prepared = prepared_] {
            WriteResult result;
            try {
                result.state = operation(prepared);
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        })
    );
}
void AdjustmentTransferController::finishWrite() {
    auto result = write_watcher_.result();
    if (result.error.isEmpty()) {
        editor_.accept(result.state, prepared_.before);
        const auto id = rows_[active_row_].target.photo_id;
        if (undoing_) {
            undo_receipts_.remove(id);
            batch_applied_.remove(id);
            if (batch_generation_ == clipboard_generation_)
                applied_.remove(id);
        } else {
            if (new_batch_pending_) {
                undo_receipts_.clear();
                new_batch_pending_ = false;
            }
            auto before = prepared_.before;
            before.versions.clear();
            before.variants.clear();
            Receipt receipt{
                std::move(before),
                result.state.base_commit_id,
                result.state.active_variant_id
            };
            undo_receipts_.insert(id, receipt);
            const AppliedIdentity identity{receipt.after_head, receipt.variant};
            batch_applied_.insert(id, identity);
            if (batch_generation_ == clipboard_generation_)
                applied_.insert(id, identity);
        }
    }
    editor_.release();
    reserved_ = false;
    if (!result.error.isEmpty()) {
        finishRow(QStringLiteral("failed"), {AdjustmentTransferIssue::Changed, result.error});
    } else {
        // A cancellation arriving during an acknowledged CAS cannot erase its receipt.
        rows_[active_row_].state = undoing_ ? QStringLiteral("undone") : QStringLiteral("applied");
        if (operations_.preview && !cancellation_->cancelled()
            && editor_.current().photo_id != result.state.photo_id) {
            preview_state_ = std::move(result.state);
            previewing_ = true;
            emit changed();
            startPreview();
        } else {
            emit photosChanged();
            finishRow(rows_[active_row_].state);
        }
    }
}
void AdjustmentTransferController::startPreview() {
    if (cancellation_->cancelled()) {
        emit photosChanged();
        finishRow(rows_[active_row_].state);
        return;
    }
    // One display-sized render follows the acknowledged write, outside the
    // editor reservation. Foreground renders get the warm executor first.
    if (editor_.foreground_busy && editor_.foreground_busy()) {
        QTimer::singleShot(50, this, &AdjustmentTransferController::startPreview);
        return;
    }
    preview_cancellation_ = std::make_shared<AdjustmentTransferCancellation>();
    preview_watcher_.setFuture(
        QtConcurrent::run([operation = operations_.preview,
                           state = preview_state_,
                           cancellation = preview_cancellation_] {
            PreviewResult result;
            try {
                result.completed = operation(state, cancellation);
            } catch (const std::exception& error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        })
    );
}
void AdjustmentTransferController::finishPreview() {
    const auto result = preview_watcher_.result();
    if (!cancellation_->cancelled() && result.error.isEmpty() && !result.completed
        && preview_cancellation_->cancelled()) {
        QTimer::singleShot(50, this, &AdjustmentTransferController::startPreview);
        return;
    }
    emit photosChanged();
    const auto detail =
        result.error.isEmpty()
            ? QString{}
            : tr("Adjustments saved; preview could not be refreshed · %1").arg(result.error);
    finishRow(rows_[active_row_].state, {AdjustmentTransferIssue::None, detail});
}
void AdjustmentTransferController::foregroundWorkChanged() {
    if (previewing_ && preview_cancellation_ && editor_.foreground_busy
        && editor_.foreground_busy())
        preview_cancellation_->cancel();
}
void AdjustmentTransferController::finishRow(
    const QString& state,
    AdjustmentTransferFailure failure
) {
    rows_[active_row_].state = state;
    rows_[active_row_].failure = std::move(failure);
    prepared_ = {};
    preview_state_ = {};
    preview_cancellation_.reset();
    previewing_ = false;
    emit changed();
    QTimer::singleShot(0, this, &AdjustmentTransferController::next);
}
void AdjustmentTransferController::cancel() {
    if (busy_ && !copying_ && cancellation_)
        cancellation_->cancel();
    if (preview_cancellation_)
        preview_cancellation_->cancel();
}
void AdjustmentTransferController::setProfileSwitchPending(bool pending) {
    if (profile_switch_pending_ == pending)
        return;
    profile_switch_pending_ = pending;
    // The profile snapshot/restart waits for busy() to drain. No target write
    // can remain outside the same quiescence boundary as other profile jobs.
    if (pending)
        cancel();
    emit changed();
}
void AdjustmentTransferController::retryFailed() {
    if (!canRetry())
        return;
    queue_.clear();
    for (int i = 0; i < total(); ++i) {
        if (rows_[i].state != QStringLiteral("failed")
            && rows_[i].state != QStringLiteral("cancelled"))
            continue;
        rows_[i].state = QStringLiteral("pending");
        rows_[i].failure = {};
        queue_.push_back(i);
    }
    startQueue();
}
void AdjustmentTransferController::undoBatch() {
    if (!canUndo())
        return;
    QVector<Row> undo_rows;
    auto ids = undo_receipts_.keys();
    ids.sort();
    for (const auto& id : ids) {
        const auto& before = undo_receipts_[id].before;
        undo_rows.push_back({{id, before.source_path}, QFileInfo(before.source_path).fileName()});
    }
    if (undo_rows.isEmpty())
        return;
    rows_ = std::move(undo_rows);
    queue_.clear();
    undoing_ = true;
    for (int i = 0; i < total(); ++i)
        queue_.push_back(i);
    startQueue();
}
void AdjustmentTransferController::dismissResults() {
    // UI dismissal never discards acknowledged undo or failed-photo recovery.
}
