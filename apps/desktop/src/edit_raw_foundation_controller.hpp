#pragma once

#include "desktop_backend.hpp"
#include "edit_raw_foundation_state.hpp"
#include "localized_ui_message.hpp"

#include <QFutureWatcher>
#include <QMetaObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <memory>

class EditController;

struct EditRawFoundationProbeTaskResult final {
    BackendRawFoundationRuntimeStatus status;
    QString error;
    std::uint64_t context_generation = 0;
};

struct EditRawFoundationExecutionTaskResult final {
    BackendRawFoundationJobStatus status;
    QString error;
    QString photo_id;
    QString source_path;
    std::uint64_t job_token = 0;
    std::uint64_t context_generation = 0;
};

// Owns the long-running model probe/materialization lifecycle. The public
// EditController remains a thin QML projection; the only persistent mutation
// produced here is one ordinary, undoable Foundation enable transition after
// Rust reports a verified Ready terminal.
class EditRawFoundationController final {
  public:
    EditRawFoundationController(EditController& owner, std::shared_ptr<DesktopBackend> backend);
    ~EditRawFoundationController();

    EditRawFoundationController(const EditRawFoundationController&) = delete;
    EditRawFoundationController& operator=(const EditRawFoundationController&) = delete;

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool canStart() const noexcept;
    [[nodiscard]] bool canCancel() const noexcept;
    [[nodiscard]] QString phase() const;
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] QString statusText() const;

    void setEnabled(bool enabled);
    void start();
    void cancel();
    void resetContext();
    void retranslateUi();

  private:
    void requestProbe(bool start_after_probe);
    void finishProbe();
    void startJob();
    void pollJob();
    void finishExecution();
    void syncRecipeState();
    void maybeApplyReady();
    void publishChange();
    void setStatus(LocalizedUiMessage status);
    void retireTerminal(const BackendRawFoundationJobStatus& status) const noexcept;
    [[nodiscard]] bool contextIsCurrent(
        std::uint64_t context_generation,
        const QString& photo_id,
        const QString& source_path
    ) const noexcept;

    EditController& owner_;
    std::shared_ptr<DesktopBackend> backend_;
    shadow::desktop::EditRawFoundationState state_;
    QFutureWatcher<EditRawFoundationProbeTaskResult> probe_watcher_;
    QFutureWatcher<EditRawFoundationExecutionTaskResult> execution_watcher_;
    QTimer progress_timer_;
    QMetaObject::Connection source_identity_connection_;
    QMetaObject::Connection foundation_connection_;
    QMetaObject::Connection state_busy_connection_;
    LocalizedUiMessage status_message_;
    std::uint64_t request_sequence_ = 0;
    bool probe_requested_ = false;
    bool start_after_probe_ = false;
    bool start_when_execution_idle_ = false;
    bool pending_recipe_enable_ = false;
};
