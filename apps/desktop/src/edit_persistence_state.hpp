#pragma once

#include "localized_ui_message.hpp"

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

// The newest requested photo replaces any older deferred target while the
// current photo is being made durable.
struct PendingPhotoOpen final {
    QString photo_id;
    QString representation_id;
    QString source_path;
    QString title;
    QString provisional_preview_source;
};

// Owns autosave pacing/recovery state and the user intents serialized behind
// the single persistence task slot. The editor facade remains responsible for
// projecting signal changes and coordinating render shutdown.
class EditPersistenceState final {
  public:
    using AutosaveTimeoutCallback = std::function<void()>;

    EditPersistenceState(QObject& context, AutosaveTimeoutCallback autosave_timeout_callback);

    EditPersistenceState(const EditPersistenceState&) = delete;
    EditPersistenceState& operator=(const EditPersistenceState&) = delete;
    EditPersistenceState(EditPersistenceState&&) = delete;
    EditPersistenceState& operator=(EditPersistenceState&&) = delete;

    [[nodiscard]] bool autosaveRequested() const noexcept;
    bool requestAutosave() noexcept;
    [[nodiscard]] bool clearAutosaveRequest() noexcept;
    [[nodiscard]] bool autosaveDebounceActive() const noexcept;
    void scheduleAutosave(int delay_ms);
    void stopAutosaveDebounce();

    [[nodiscard]] quint64 autosaveSnapshotRevision() const noexcept;
    void captureAutosaveSnapshot(quint64 revision) noexcept;
    void resetAutosaveSnapshot() noexcept;

    [[nodiscard]] bool autosaveFailed() const noexcept;
    [[nodiscard]] const LocalizedUiMessage& autosaveFailure() const noexcept;
    [[nodiscard]] bool setAutosaveFailure(LocalizedUiMessage error);
    [[nodiscard]] bool clearAutosaveFailure();

    void queuePhotoOpen(PendingPhotoOpen pending);
    [[nodiscard]] bool hasPendingPhotoOpen() const noexcept;
    [[nodiscard]] std::optional<PendingPhotoOpen> takePendingPhotoOpen();
    void clearPendingPhotoOpen() noexcept;

    void queueVersionSave(QString name);
    [[nodiscard]] bool hasPendingVersionSave() const noexcept;
    [[nodiscard]] std::optional<QString> takePendingVersionSave();

    void queueVersionLoad(QString commit_id);
    [[nodiscard]] bool hasPendingVersionLoad() const noexcept;
    [[nodiscard]] std::optional<QString> takePendingVersionLoad();
    [[nodiscard]] bool clearPendingVersionLoad() noexcept;
    [[nodiscard]] bool clearPendingVersionActions() noexcept;

    [[nodiscard]] bool closeAfterAutosave() const noexcept;
    void requestApplicationClose() noexcept;
    void cancelApplicationClose() noexcept;
    [[nodiscard]] bool closePhotoAfterAutosave() const noexcept;
    void requestPhotoClose() noexcept;
    void clearPhotoClose() noexcept;

    [[nodiscard]] bool hasDeferredCompletionAction() const noexcept;

  private:
    QTimer autosave_debounce_;
    std::optional<PendingPhotoOpen> pending_photo_open_;
    std::optional<QString> pending_version_save_name_;
    std::optional<QString> pending_version_load_commit_id_;
    LocalizedUiMessage autosave_failure_;
    quint64 autosave_snapshot_revision_ = 0;
    bool autosave_requested_ = false;
    bool close_after_autosave_ = false;
    bool close_photo_after_autosave_ = false;
};
