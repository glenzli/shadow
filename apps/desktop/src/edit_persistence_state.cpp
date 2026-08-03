#include "edit_persistence_state.hpp"

#include <QtGlobal>

#include <utility>

EditPersistenceState::EditPersistenceState(
    QObject& context,
    AutosaveTimeoutCallback autosave_timeout_callback
) {
    autosave_debounce_.setSingleShot(true);
    QObject::connect(
        &autosave_debounce_,
        &QTimer::timeout,
        &context,
        [callback = std::move(autosave_timeout_callback)] { callback(); }
    );
}

bool EditPersistenceState::autosaveRequested() const noexcept {
    return autosave_requested_;
}

bool EditPersistenceState::requestAutosave() noexcept {
    if (autosave_requested_) {
        return false;
    }
    autosave_requested_ = true;
    return true;
}

bool EditPersistenceState::clearAutosaveRequest() noexcept {
    if (!autosave_requested_) {
        return false;
    }
    autosave_requested_ = false;
    return true;
}

bool EditPersistenceState::autosaveDebounceActive() const noexcept {
    return autosave_debounce_.isActive();
}

void EditPersistenceState::scheduleAutosave(const int delay_ms) {
    autosave_debounce_.start(delay_ms);
}

void EditPersistenceState::stopAutosaveDebounce() {
    autosave_debounce_.stop();
}

quint64 EditPersistenceState::autosaveSnapshotRevision() const noexcept {
    return autosave_snapshot_revision_;
}

void EditPersistenceState::captureAutosaveSnapshot(const quint64 revision) noexcept {
    autosave_snapshot_revision_ = revision;
}

void EditPersistenceState::resetAutosaveSnapshot() noexcept {
    autosave_snapshot_revision_ = 0;
}

bool EditPersistenceState::autosaveFailed() const noexcept {
    return !autosave_failure_.isEmpty();
}

const LocalizedUiMessage& EditPersistenceState::autosaveFailure() const noexcept {
    return autosave_failure_;
}

bool EditPersistenceState::setAutosaveFailure(LocalizedUiMessage error) {
    if (autosave_failure_ == error) {
        return false;
    }
    autosave_failure_ = std::move(error);
    return true;
}

bool EditPersistenceState::clearAutosaveFailure() {
    if (autosave_failure_.isEmpty()) {
        return false;
    }
    autosave_failure_.clear();
    return true;
}

void EditPersistenceState::queuePhotoOpen(PendingPhotoOpen pending) {
    pending_photo_open_ = std::move(pending);
}

bool EditPersistenceState::hasPendingPhotoOpen() const noexcept {
    return pending_photo_open_.has_value();
}

std::optional<PendingPhotoOpen> EditPersistenceState::takePendingPhotoOpen() {
    std::optional<PendingPhotoOpen> pending = std::move(pending_photo_open_);
    pending_photo_open_.reset();
    return pending;
}

void EditPersistenceState::clearPendingPhotoOpen() noexcept {
    pending_photo_open_.reset();
}

void EditPersistenceState::queueVersionSave(QString name) {
    Q_ASSERT(!pending_version_load_commit_id_.has_value());
    pending_version_save_name_ = std::move(name);
}

bool EditPersistenceState::hasPendingVersionSave() const noexcept {
    return pending_version_save_name_.has_value();
}

std::optional<QString> EditPersistenceState::takePendingVersionSave() {
    std::optional<QString> pending = std::move(pending_version_save_name_);
    pending_version_save_name_.reset();
    return pending;
}

void EditPersistenceState::queueVersionLoad(QString commit_id) {
    Q_ASSERT(!pending_version_save_name_.has_value());
    pending_version_load_commit_id_ = std::move(commit_id);
}

bool EditPersistenceState::hasPendingVersionLoad() const noexcept {
    return pending_version_load_commit_id_.has_value();
}

std::optional<QString> EditPersistenceState::takePendingVersionLoad() {
    std::optional<QString> pending = std::move(pending_version_load_commit_id_);
    pending_version_load_commit_id_.reset();
    return pending;
}

bool EditPersistenceState::clearPendingVersionLoad() noexcept {
    if (!pending_version_load_commit_id_.has_value()) {
        return false;
    }
    pending_version_load_commit_id_.reset();
    return true;
}

bool EditPersistenceState::clearPendingVersionActions() noexcept {
    const bool had_pending_action = pending_version_save_name_.has_value()
                                    || pending_version_load_commit_id_.has_value();
    pending_version_save_name_.reset();
    pending_version_load_commit_id_.reset();
    return had_pending_action;
}

bool EditPersistenceState::closeAfterAutosave() const noexcept {
    return close_after_autosave_;
}

void EditPersistenceState::requestApplicationClose() noexcept {
    close_after_autosave_ = true;
}

void EditPersistenceState::cancelApplicationClose() noexcept {
    close_after_autosave_ = false;
}

bool EditPersistenceState::closePhotoAfterAutosave() const noexcept {
    return close_photo_after_autosave_;
}

void EditPersistenceState::requestPhotoClose() noexcept {
    close_photo_after_autosave_ = true;
}

void EditPersistenceState::clearPhotoClose() noexcept {
    close_photo_after_autosave_ = false;
}

bool EditPersistenceState::hasDeferredCompletionAction() const noexcept {
    return pending_photo_open_.has_value() || pending_version_save_name_.has_value()
           || pending_version_load_commit_id_.has_value() || close_photo_after_autosave_
           || close_after_autosave_;
}
