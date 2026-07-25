#include "edit_controller.hpp"

#include <QtConcurrent>

#include <utility>

namespace {

// Catalog state is an independent controller state machine: opening, autosave,
// recovery, version checkout and deferred photo/window transitions all share
// one compare-and-swap lifecycle and must remain auditable together.
constexpr int EDIT_AUTOSAVE_DEBOUNCE_MS = 700;

[[nodiscard]] bool incompatible_development_recipe(const QString& error) noexcept {
    return error.startsWith(QStringLiteral("incompatible development Recipe:"));
}


[[nodiscard]] LocalizedUiMessage
edit_message(const char *const source,
             const std::initializer_list<LocalizedUiArgument> arguments = {}) {
  return {"EditController", source, arguments};
}

} // namespace

bool EditController::openPhoto(
    const QString& photo_id,
    const QString& representation_id,
    const QString& source_path,
    const QString& title,
    const QString& provisional_preview_source
) {
    if (photo_id.isEmpty() || representation_id.isEmpty() || source_path.isEmpty()) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "The selected Review item has no editable original source")));
        return false;
    }
    if (active_ && (photo_id != photo_id_ || representation_id != representation_id_
                    || source_path != source_path_)) {
        cancelActivePreview(true);
    }
    if (state_running_) {
        if (active_) {
            // Do not make a fast Library selection race a background state
            // operation. The newest target wins and is opened as soon as the
            // current operation reaches a safe controller boundary. Autosave
            // may need to chain once more if the user changed controls while
            // its snapshot was in flight.
            pending_photo_open_ = PendingPhotoOpen{
                .photo_id = photo_id,
                .representation_id = representation_id,
                .source_path = source_path,
                .title = title,
                .provisional_preview_source = provisional_preview_source,
            };
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController", "Preparing the selected photo…"
            )));
            return true;
        }
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Finish the current version operation first")));
        return false;
    }
    if (active_ && photo_id == photo_id_
        && representation_id == representation_id_
        && source_path == source_path_) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "This photo is already open in Precision")));
        return true;
    }
    if (dirty_ && active_) {
        // Shadow's working ref is an autosave, not a manually committed version. Queue the
        // selected photo, force the pending working snapshot now, and resume this exact open
        // request once persistence succeeds. This is intentionally non-blocking for browsing.
        pending_photo_open_ = PendingPhotoOpen{
            .photo_id = photo_id,
            .representation_id = representation_id,
            .source_path = source_path,
            .title = title,
            .provisional_preview_source = provisional_preview_source,
        };
        autosave_debounce_.stop();
        if (autosaveFailed()) {
            // Do not silently retry a known permanent error on every library
            // selection. The shell can now offer retry, stay here, or an
            // explicit discard-and-open recovery action.
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController",
                "Autosave failed · resolve it before replacing this photo's working changes"
            )));
            emit photoSwitchSaveFailed();
            // The selection has been accepted and is queued behind the
            // recovery choice surfaced by the shell. Returning success keeps
            // Main.qml from also reporting a generic "could not open" error.
            return true;
        }
        if (!autosave_requested_) {
            autosave_requested_ = true;
            emit autosavePendingChanged();
        }
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Saving current adjustments before opening the selected photo…"
        )));
        startAutosave();
        return true;
    }

    ++photo_generation_;
    ++render_revision_;
    active_parameter_gestures_.clear();
    working_revision_ = 0;
    autosave_snapshot_revision_ = 0;
    settled_render_revision_ = 0;
    preview_debounce_.stop();
    autosave_debounce_.stop();
    if (autosave_requested_) {
        autosave_requested_ = false;
        emit autosavePendingChanged();
    }
    resetDetailState();
    preview_queued_ = false;
    before_requested_ = false;
    clearHistograms();
    if (!optics_receipt_.isEmpty()) {
        optics_receipt_.clear();
        emit opticsReceiptChanged();
    }
    photo_id_ = photo_id;
    representation_id_ = representation_id;
    source_path_ = source_path;
    title_ = title;
    if (provisional_preview_source_ != provisional_preview_source) {
        provisional_preview_source_ = provisional_preview_source;
        emit provisionalPreviewSourceChanged();
    }
    versions_.replace({});
    base_commit_id_.clear();
    durable_working_commit_id_.clear();
    setVersionDraft(false);
    committed_grade_stack_ = {};
    clearSessionHistory();
    setGradeStack({});
    if (!preview_source_.isEmpty()) {
        preview_source_.clear();
        emit previewSourceChanged();
    }
    if (!before_preview_source_.isEmpty()) {
        before_preview_source_.clear();
        emit beforePreviewSourceChanged();
    }
    if (!before_error_message_.isEmpty()) {
    before_error_message_.clear();
        emit beforeErrorTextChanged();
    }
    if (!recipe_recovery_message_.isEmpty()) {
        recipe_recovery_message_.clear();
        emit recipeRecoveryChanged();
    }
    preview_store_->clearAll(render_revision_, photo_generation_);
    if (!active_) {
        active_ = true;
        emit activeChanged();
        emit gradeNodeActionsChanged();
    }
    emit titleChanged();
    emit sourcePathChanged();
    emit sourceIdentityChanged();
    state_task_kind_ = EditStateTaskKind::Open;
    setStateRunning(true);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Loading non-destructive edit history…")));
    state_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::loadState,
        backend_,
        photo_id_,
        source_path_,
        photo_generation_
    ));
    return true;
}

void EditController::closePhoto() {
    cancelActivePreview(true);
    if (state_running_) {
        // A return to Library is allowed while an initial open or an autosave
        // is in flight. A later photo selection can install a fresh pending
        // target; otherwise the completed task will close this session.
        pending_photo_open_.reset();
        close_photo_after_autosave_ = true;
        return;
    }
    // Recovery belongs to the Precision session, not to the photo in the
    // Library. Clear it before the inactive early-return as an open failure
    // deliberately marks the editor inactive while leaving recovery visible.
    // Otherwise "Return to Review" changes the workspace behind a modal popup
    // which can no longer be dismissed.
    if (!recipe_recovery_message_.isEmpty()) {
        recipe_recovery_message_.clear();
        emit recipeRecoveryChanged();
    }
    if (dirty_) {
        if (!autosave_requested_) {
            // Merely previewing an older named Version is a transient draft,
            // not an edit. Closing it must not silently replace `working`.
            revertEdits();
        } else {
            close_photo_after_autosave_ = true;
            autosave_debounce_.stop();
            startAutosave();
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController", "Saving current adjustments before closing Precision…")));
            return;
        }
    }
    if (!active_) {
        return;
    }
    pending_photo_open_.reset();
    setPointColorPickerActive(false);
    setRetouchPickerActive(false);
    setWhiteBalancePickerActive(false);
    clearSessionHistory();
    resetDetailState();
    clearHistograms();
    if (!optics_receipt_.isEmpty()) {
        optics_receipt_.clear();
        emit opticsReceiptChanged();
    }
    if (!provisional_preview_source_.isEmpty()) {
        provisional_preview_source_.clear();
        emit provisionalPreviewSourceChanged();
    }
    // Precision is a session, not a hidden second Library. Drop its published
    // sources on exit so reopening from a different grid item can never show
    // the previous image while the next state request is loading.
    if (!preview_source_.isEmpty()) {
        preview_source_.clear();
        emit previewSourceChanged();
    }
    if (!before_preview_source_.isEmpty()) {
        before_preview_source_.clear();
        emit beforePreviewSourceChanged();
    }
    if (!before_error_message_.isEmpty()) {
        before_error_message_.clear();
        emit beforeErrorTextChanged();
    }
    if (!photo_id_.isEmpty() || !representation_id_.isEmpty()) {
        photo_id_.clear();
        representation_id_.clear();
        emit sourceIdentityChanged();
    }
    if (!source_path_.isEmpty()) {
        source_path_.clear();
        emit sourcePathChanged();
    }
    if (!title_.isEmpty()) {
        title_.clear();
        emit titleChanged();
    }
    active_ = false;
    emit activeChanged();
    emit gradeNodeActionsChanged();
    emit historyChanged();
}

void EditController::resetIncompatibleRecipe() {
    if (!recipeRecoveryRequired() || state_running_ || photo_id_.isEmpty()
        || source_path_.isEmpty()) {
        return;
    }
    state_task_kind_ = EditStateTaskKind::ResetIncompatibleRecipe;
    setStateRunning(true);
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Resetting this photo’s development edits…"
    )));
    state_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::resetIncompatibleRecipeState,
        backend_,
        photo_id_,
        source_path_,
        photo_generation_
    ));
}


void EditController::saveVersion(const QString& version_name) {
    const QString name = version_name.trimmed();
    if (!active_ || state_running_) {
        return;
    }
    if (name.isEmpty()) {
    setStatusMessage(edit_message(
        QT_TRANSLATE_NOOP("EditController", "Enter a name for this version")));
        return;
    }
    history_.finishGesture(grade_stack_);
    emit historyChanged();
    state_task_kind_ = EditStateTaskKind::Save;
    setStateRunning(true);
  setStatusMessage(edit_message(
      QT_TRANSLATE_NOOP("EditController", "Creating Library version “%1”…"),
      {name}));
    state_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::saveState,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        durable_working_commit_id_,
        grade_stack_,
        name,
        photo_generation_
    ));
}

void EditController::loadVersionDraft(const QString& commit_id) {
    if (!active_ || state_running_ || commit_id.isEmpty()) {
        return;
    }
    if (dirty_) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Saving current adjustments before loading another version")));
        autosave_debounce_.stop();
        startAutosave();
        return;
    }
    state_task_kind_ = EditStateTaskKind::LoadDraft;
    setStateRunning(true);
  setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
      "EditController", "Loading saved version into working changes…")));
    state_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::loadVersionDraftState,
        backend_,
        photo_id_,
        source_path_,
        commit_id,
        photo_generation_
    ));
}

void EditController::retryAutosave() {
    if (!active_ || !dirty_ || !autosave_requested_ || state_running_) {
        return;
    }
    startAutosave();
}

void EditController::cancelPendingPhotoOpen() {
    pending_photo_open_.reset();
}

bool EditController::discardFailedAutosaveAndOpenPendingPhoto() {
    if (!autosaveFailed() || state_running_ || !pending_photo_open_.has_value()) {
        return false;
    }
    // This is reached only from the explicit destructive recovery action in
    // Main.qml. The durable `working` snapshot remains untouched; only the
    // unpersisted in-memory draft is discarded.
    autosave_debounce_.stop();
    clearAutosaveFailure();
    if (autosave_requested_) {
        autosave_requested_ = false;
        emit autosavePendingChanged();
    }
    setDirty(false);
    return openPendingPhoto();
}

bool EditController::prepareToClose() {
    autosave_debounce_.stop();
    preview_debounce_.stop();
    detail_debounce_.stop();
    preview_queued_ = false;
    before_requested_ = false;
    detail_queued_ = false;
    cancelActivePreview(true);
    // A close must not race a queued photo selection. The existing session
    // still gets its durable working snapshot, but no new Precision session is
    // started on the way out.
    pending_photo_open_.reset();
    close_after_autosave_ = true;
    if (state_running_) {
        return false;
    }
    if (active_ && dirty_ && autosave_requested_) {
        // An autosave failure is sticky until the user explicitly retries it.
        // Retrying it implicitly from every native close event used to trap the
        // window in an endless "save failed -> try to quit -> save failed"
        // loop. Keep the working draft intact and let the shell offer the
        // deliberate choices: retry, keep editing, or quit without the last
        // unsaved working snapshot.
        if (autosaveFailed()) {
            close_after_autosave_ = false;
            emit closeSaveFailed();
            return false;
        }
        startAutosave();
        return false;
    }
    if (current_rendering_ || before_rendering_ || detail_rendering_) {
        return false;
    }
    close_after_autosave_ = false;
    return true;
}

void EditController::finishStateTask() {
    EditStateTaskResult result = state_watcher_.result();
    setStateRunning(false);
    if (result.photo_generation != photo_generation_) {
        maybeFinishDeferredApplicationClose();
        return;
    }
    if (!result.error.isEmpty()) {
        if (result.kind == EditStateTaskKind::Open && active_) {
            active_ = false;
            emit activeChanged();
            emit gradeNodeActionsChanged();
        }
        const bool newer_draft_exists = result.kind == EditStateTaskKind::Autosave
            && active_ && dirty_ && autosave_requested_
            && working_revision_ != autosave_snapshot_revision_;
        if (newer_draft_exists) {
            // This task was saving an older slider snapshot. It may legitimately lose a
            // compare-and-swap race while the user has already made a newer edit, so give that
            // newer snapshot one clean attempt before reporting a durable save failure.
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController", "Saving newer adjustments locally…"
            )));
            if (pending_photo_open_.has_value() || close_photo_after_autosave_
                || close_after_autosave_) {
                startAutosave();
            } else {
                scheduleAutosave();
            }
            return;
        }
        if (result.kind == EditStateTaskKind::Autosave) {
            const LocalizedUiMessage failure = edit_message(
                QT_TRANSLATE_NOOP("EditController", "Autosave failed · %1"),
                {result.error}
            );
            setAutosaveFailure(failure);
            setStatusMessage(failure);
        } else if (result.kind == EditStateTaskKind::Open
                   && incompatible_development_recipe(result.error)) {
            recipe_recovery_message_ = edit_message(QT_TRANSLATE_NOOP(
                "EditController",
                "This photo uses an earlier development edit recipe that this build cannot read. Resetting removes only this photo’s edit history; the original file, Library metadata, ratings, flags, and albums are unchanged."
            ));
            emit recipeRecoveryChanged();
            setStatusMessage(recipe_recovery_message_);
        } else if (result.kind == EditStateTaskKind::ResetIncompatibleRecipe) {
            recipe_recovery_message_ = edit_message(QT_TRANSLATE_NOOP(
                "EditController",
                "Could not reset this photo’s old development edits · %1"
            ), {result.error});
            emit recipeRecoveryChanged();
            setStatusMessage(recipe_recovery_message_);
        } else if (result.kind == EditStateTaskKind::Open) {
            setStatusMessage(edit_message(
                QT_TRANSLATE_NOOP("EditController", "Could not open this photo · %1"),
                {result.error}
            ));
        } else {
            setStatusMessage(edit_message(
                QT_TRANSLATE_NOOP("EditController", "Version operation failed · %1"),
                {result.error}
            ));
        }
        if (close_after_autosave_) {
            close_after_autosave_ = false;
            emit closeSaveFailed();
        }
        if (result.kind == EditStateTaskKind::Autosave && pending_photo_open_.has_value()) {
            emit photoSwitchSaveFailed();
        }
        close_photo_after_autosave_ = false;
        if (result.kind == EditStateTaskKind::Autosave) {
            // A persistent Catalog or decoder error must not look like an endless save.
            // Keep the draft intact and retry only after the user explicitly asks, or edits
            // again and therefore supplies a newer working snapshot.
            autosave_debounce_.stop();
            emit autosavePendingChanged();
        }
        if (result.kind == EditStateTaskKind::Open && openPendingPhoto()) {
            return;
        }
        if (preview_queued_) {
            preview_debounce_.start(0);
        }
        maybeStartBeforePreview();
        maybeStartDetailRender();
        return;
    }
    bool autosave_needs_follow_up = false;
    if (result.kind == EditStateTaskKind::Autosave) {
        autosave_needs_follow_up = applyAutosavedState(std::move(result.state));
        if (autosave_needs_follow_up) {
            setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
                "EditController", "Saving newer adjustments locally…")));
            if (pending_photo_open_.has_value() || close_photo_after_autosave_
                || close_after_autosave_) {
                startAutosave();
            } else {
                scheduleAutosave();
            }
            return;
        }
    } else {
        applyState(std::move(result.state));
    }
    if (result.kind == EditStateTaskKind::ResetIncompatibleRecipe) {
        if (!recipe_recovery_message_.isEmpty()) {
            recipe_recovery_message_.clear();
            emit recipeRecoveryChanged();
        }
        if (!active_) {
            active_ = true;
            emit activeChanged();
            emit gradeNodeActionsChanged();
        }
    }
    if (openPendingPhoto()) {
        return;
    }
    switch (result.kind) {
    case EditStateTaskKind::Open:
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Edit history ready · rendering preview")));
        if (!close_after_autosave_) {
            schedulePreview(0);
        }
        break;
    case EditStateTaskKind::ResetIncompatibleRecipe:
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Old development edits reset · rendering the current recipe")));
        if (!close_after_autosave_) {
            schedulePreview(0);
        }
        break;
    case EditStateTaskKind::Save:
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Library version created · the previous state remains available")));
        break;
    case EditStateTaskKind::Autosave:
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Current adjustments saved locally")));
        break;
    case EditStateTaskKind::LoadDraft:
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController",
            "Named version loaded as a draft · adjust it to create a new working state")));
        if (!close_after_autosave_) {
            schedulePreview(0);
        }
        break;
    }
    if (!close_after_autosave_ && preview_queued_ && !preview_debounce_.isActive()) {
        preview_debounce_.start(0);
    }
    if (!close_after_autosave_) {
        maybeStartBeforePreview();
        maybeStartDetailRender();
    }
    if (close_photo_after_autosave_) {
        close_photo_after_autosave_ = false;
        closePhoto();
    }
    maybeFinishDeferredApplicationClose();
}

bool EditController::openPendingPhoto() {
    if (close_after_autosave_ || !pending_photo_open_.has_value()) {
        return false;
    }
    const PendingPhotoOpen pending = std::move(*pending_photo_open_);
    pending_photo_open_.reset();
    close_photo_after_autosave_ = false;
    return openPhoto(
        pending.photo_id,
        pending.representation_id,
        pending.source_path,
        pending.title,
        pending.provisional_preview_source
    );
}

void EditController::maybeFinishDeferredApplicationClose() {
    if (!close_after_autosave_ || state_running_ || current_rendering_
        || before_rendering_ || detail_rendering_) {
        return;
    }
    close_after_autosave_ = false;
    emit closeReady();
}

void EditController::applyState(BackendPhotoEditState state) {
    if (state.photo_id != photo_id_ || state.source_path != source_path_) {
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController",
        "Catalog returned edit state for a different photo")));
        return;
    }
    setVersionDraft(state.is_version_draft);
    autosave_debounce_.stop();
    clearAutosaveFailure();
    if (autosave_requested_) {
        autosave_requested_ = false;
        emit autosavePendingChanged();
    }
    if (!version_draft_) {
        committed_grade_stack_ = state.grade_stack;
        durable_working_commit_id_ = state.base_commit_id;
    }
    base_commit_id_ = std::move(state.base_commit_id);
    setGradeStack(std::move(state.grade_stack));
    working_revision_ = 0;
    autosave_snapshot_revision_ = 0;
    clearSessionHistory();
    versions_.replace(std::move(state.versions));
}


void EditController::setDirty(const bool dirty) {
    if (dirty_ == dirty) {
        if (dirty && autosave_requested_ && !state_running_) {
            scheduleAutosave();
        }
        return;
    }
    dirty_ = dirty;
    emit dirtyChanged();
    if (dirty_ && autosave_requested_ && !state_running_) {
        scheduleAutosave();
    } else if (!dirty_) {
        autosave_debounce_.stop();
    }
}

void EditController::setAutosaveFailure(LocalizedUiMessage error) {
    if (autosave_error_message_ == error) {
        return;
    }
    autosave_error_message_ = std::move(error);
    emit autosaveFailedChanged();
    emit autosaveErrorTextChanged();
    emit autosavePendingChanged();
}

void EditController::clearAutosaveFailure() {
    if (autosave_error_message_.isEmpty()) {
        return;
    }
    autosave_error_message_.clear();
    emit autosaveFailedChanged();
    emit autosaveErrorTextChanged();
    emit autosavePendingChanged();
}

void EditController::scheduleAutosave() {
    if (!active_ || !dirty_ || !autosave_requested_ || state_running_) {
        return;
    }
    autosave_debounce_.start(EDIT_AUTOSAVE_DEBOUNCE_MS);
    emit autosavePendingChanged();
}

void EditController::startAutosave() {
    autosave_debounce_.stop();
    if (!active_ || !dirty_ || !autosave_requested_ || state_running_) {
        return;
    }
    clearAutosaveFailure();
    history_.finishGesture(grade_stack_);
    emit historyChanged();
    autosave_snapshot_revision_ = working_revision_;
    state_task_kind_ = EditStateTaskKind::Autosave;
    setStateRunning(true);
    emit autosavePendingChanged();
    setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
        "EditController", "Saving current adjustments locally…")));
    state_watcher_.setFuture(QtConcurrent::run(
        EditTaskRunner::autosaveState,
        backend_,
        photo_id_,
        source_path_,
        base_commit_id_,
        durable_working_commit_id_,
        grade_stack_,
        photo_generation_
    ));
}

bool EditController::applyAutosavedState(BackendPhotoEditState state) {
    if (state.photo_id != photo_id_ || state.source_path != source_path_) {
        setStatusMessage(edit_message(QT_TRANSLATE_NOOP(
            "EditController", "Catalog returned autosave state for a different photo")));
        return false;
    }
    const bool changed_after_snapshot = working_revision_ != autosave_snapshot_revision_;
    const BackendGradeStack saved_stack = std::move(state.grade_stack);
    if (!changed_after_snapshot && grade_stack_ != saved_stack) {
        // The visible stack is the one that was persisted. A mismatch means
        // the Catalog had to canonicalize or recover it, so accept that
        // authoritative representation only when no newer local edit exists.
        setGradeStack(saved_stack);
    }
    setVersionDraft(false);
    clearAutosaveFailure();
    base_commit_id_ = state.base_commit_id;
    durable_working_commit_id_ = state.base_commit_id;
    committed_grade_stack_ = saved_stack;
    versions_.replace(std::move(state.versions));
    autosave_requested_ = changed_after_snapshot;
    setDirty(changed_after_snapshot);
    emit autosavePendingChanged();
    return changed_after_snapshot;
}

void EditController::setVersionDraft(const bool draft) {
    if (version_draft_ == draft) {
        return;
    }
    version_draft_ = draft;
    emit versionDraftChanged();
}

void EditController::setStateRunning(const bool running) {
    if (state_running_ == running) {
        return;
    }
    const bool previous_busy = busy();
    const bool previously_locked = interactionLocked();
    state_running_ = running;
    if (!running) {
        state_task_kind_ = EditStateTaskKind::Open;
    }
    if (previously_locked != interactionLocked()) {
        emit stateBusyChanged();
    }
    emit autosavePendingChanged();
    emit gradeNodeActionsChanged();
    emitBusyChange(previous_busy);
}
