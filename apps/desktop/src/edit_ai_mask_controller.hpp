#pragma once

#include "desktop_backend.hpp"
#include "edit_ai_mask_prompt_state.hpp"

#include <QFutureWatcher>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <optional>

class EditController;

struct EditAiMaskExecutionResult final {
    BackendSubjectMaskResult result;
    QString error;
};

struct EditAiMaskApplyResult final {
    BackendPhotoEditState state;
    QString error;
};

// Owns the provider job, apply transaction, and prompt-generation race. The
// public EditController remains a thin QML projection and delegates all AI
// authoring behavior here.
class EditAiMaskController final {
  public:
    EditAiMaskController(EditController& owner, std::shared_ptr<DesktopBackend> backend);
    ~EditAiMaskController();

    EditAiMaskController(const EditAiMaskController&) = delete;
    EditAiMaskController& operator=(const EditAiMaskController&) = delete;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool locksInteraction() const noexcept;
    [[nodiscard]] bool foregroundMode() const noexcept;
    [[nodiscard]] bool canGenerate() const noexcept;
    [[nodiscard]] bool hasCandidate() const noexcept;
    [[nodiscard]] QString candidateSource() const;
    [[nodiscard]] QVariantList promptPoints() const;

    [[nodiscard]] bool beginPrompt();
    void setForegroundMode(bool foreground);
    void appendPoint(double x, double y, bool foreground);
    void undoPoint();
    void clearPoints();
    void generate();
    void applyCandidate();
    void cancel();
    void resetContext();

  private:
    struct CapturedContext final {
        QString photo_id;
        QString source_path;
        QString target_grade_node_id;
        std::uint32_t target_grade_node_index = 0;
        BackendGradeStack grade_stack;
        std::uint64_t photo_generation = 0;
    };

    void finishExecution();
    void finishApply();
    void publishStateChange(bool previous_busy, bool previously_locked);
    [[nodiscard]] bool contextIsCurrent() const noexcept;
    void retireCandidate() noexcept;
    void discardProposal(std::uint64_t proposal_token) const noexcept;

    EditController& owner_;
    std::shared_ptr<DesktopBackend> backend_;
    shadow::desktop::EditAiMaskPromptState prompt_state_;
    QFutureWatcher<EditAiMaskExecutionResult> execution_watcher_;
    QFutureWatcher<EditAiMaskApplyResult> apply_watcher_;
    std::optional<CapturedContext> context_;
    std::uint64_t candidate_proposal_token_ = 0;
    std::uint64_t candidate_generation_ = 0;
    QString candidate_source_;
    bool active_ = false;
    bool foreground_mode_ = true;
    bool apply_in_flight_ = false;
};
