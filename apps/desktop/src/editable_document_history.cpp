#include "editable_document_history.hpp"
#include "edit_controller.hpp"

#include <QJsonDocument>
#include <QtConcurrent>

EditableDocumentHistory::EditableDocumentHistory(
    std::shared_ptr<DesktopBackend> backend,
    EditController& editor
) : backend_(std::move(backend)), editor_(editor) {
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, &EditableDocumentHistory::finish);
    connect(&editor_, &EditController::sourceIdentityChanged, this, [this] {
        items_.clear();
        error_.clear();
        cursor_ = 0;
        emit changed();
    });
}
EditableDocumentHistory::~EditableDocumentHistory() {
    watcher_.waitForFinished();
}

void EditableDocumentHistory::refresh() {
    load(0, false);
}
void EditableDocumentHistory::loadMore() {
    if (cursor_ > 0)
        load(cursor_, true);
}
void EditableDocumentHistory::load(qint64 before, bool append) {
    if (busy() || !editor_.active() || editor_.stateBusy())
        return;
    const auto photo = editor_.photo_generation_;
    watcher_.setFuture(
        QtConcurrent::run([backend = backend_, id = editor_.photoId(), photo, before, append] {
            Result r;
            r.photo = photo;
            r.append = append;
            try {
                r.page = backend->documentHistoryPage(id, before);
            } catch (const std::exception& e) {
                r.error = QString::fromUtf8(e.what());
            }
            return r;
        })
    );
    emit changed();
}

void EditableDocumentHistory::restore(const QString& step) {
    bool valid = false;
    const auto id = step.toLongLong(&valid);
    if (!valid || busy() || !editor_.active() || editor_.stateBusy()
        || editor_.toolOperationRunning())
        return;
    editor_.finishDocumentGesture();
    editor_.document_history_loading_ = true;
    emit editor_.stateBusyChanged();
    const auto photo = editor_.photo_generation_, revision = editor_.working_revision_;
    watcher_.setFuture(
        QtConcurrent::run([backend = backend_,
                           photo,
                           revision,
                           id,
                           pid = editor_.photoId(),
                           source = editor_.sourcePath()] {
            Result r;
            r.restore = true;
            r.photo = photo;
            r.revision = revision;
            try {
                r.step = backend->documentHistoryStep(pid, id);
                backend->recordDocumentHistory(pid, source, 3, r.step);
                r.undo = backend->documentUndo(pid);
                r.page = backend->documentHistoryPage(pid);
            } catch (const std::exception& e) {
                r.error = QString::fromUtf8(e.what());
            }
            return r;
        })
    );
    emit changed();
}

void EditableDocumentHistory::finish() {
    auto result = watcher_.result();
    if (result.restore) {
        editor_.document_history_loading_ = false;
        emit editor_.stateBusyChanged();
    }
    if (result.photo != editor_.photo_generation_)
        return;
    error_ = result.error;
    if (result.restore && error_.isEmpty() && result.revision == editor_.working_revision_) {
        editor_.persistence_state_.requestAutosave();
        editor_.clearAutosaveFailure();
        ++editor_.working_revision_;
        editor_.setEditBaseCommitId(result.step.after.base_commit_id);
        editor_.setGradeStack(result.step.after.stack);
        editor_.history_.restore(std::move(result.undo));
        emit editor_.historyChanged();
        emit editor_.documentEdited();
        editor_.schedulePreview(0);
    }
    if (error_.isEmpty()) {
        const auto page = QJsonDocument::fromJson(result.page.toUtf8()).toVariant().toMap();
        cursor_ = page.value("nextCursor").toString().toLongLong();
        auto incoming = page.value("items").toList();
        for (auto& item : incoming) {
            auto value = item.toMap();
            value.insert("label", tr("Edit %1").arg(value.value("id").toString()));
            item = value;
        }
        if (result.append)
            items_ += incoming;
        else
            items_ = incoming;
    }
    emit changed();
}
