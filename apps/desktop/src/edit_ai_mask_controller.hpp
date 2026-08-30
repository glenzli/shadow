#pragma once

#include "desktop_backend.hpp"
#include "edit_ai_mask_prompt_state.hpp"

#include <QFutureWatcher>
#include <QTimer>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <optional>

class EditController;

enum class AiMaskSelectionKind : std::uint8_t {
    PromptedSubject,
    FaceRegion,
    SemanticQuery,
};

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
    [[nodiscard]] bool faceRegionMode() const noexcept;
    [[nodiscard]] bool semanticMode() const noexcept;
    [[nodiscard]] QString semanticQuery() const;
    [[nodiscard]] int faceRegion() const noexcept;
    [[nodiscard]] QVariantList people() const;
    [[nodiscard]] int selectedPerson() const noexcept;
    [[nodiscard]] int faceRegionMask() const noexcept;
    [[nodiscard]] bool canGenerate() const noexcept;
    [[nodiscard]] bool hasCandidate() const noexcept;
    [[nodiscard]] QString candidateSource() const;
    [[nodiscard]] QVariantList promptPoints() const;

    [[nodiscard]] bool beginPrompt(AiMaskSelectionKind kind);
    [[nodiscard]] bool beginSemantic(
        const QString& query,
        std::uint8_t maximum_regions,
        std::uint8_t score_threshold_percent,
        bool prefer_current_node
    );
    void setForegroundMode(bool foreground);
    void setFaceRegion(int region);
    void setSelectedPerson(int person_index);
    void toggleFaceRegion(int region, bool selected);
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
        std::uint64_t input_session_token = 0;
    };

    void finishExecution();
    void finishApply();
    [[nodiscard]] bool beginSelection(AiMaskSelectionKind kind, bool prefer_current_node);
    void requestGeneration();
    void tryStartPendingGeneration();
    void startGeneration();
    void publishStateChange(bool previous_busy, bool previously_locked);
    [[nodiscard]] bool contextIsCurrent() const noexcept;
    [[nodiscard]] bool hasForegroundPoint() const noexcept;
    [[nodiscard]] bool selectionReady() const noexcept;
    void retireInputSession() noexcept;
    void retireCandidate() noexcept;
    void discardProposal(std::uint64_t proposal_token) const noexcept;

    EditController& owner_;
    std::shared_ptr<DesktopBackend> backend_;
    shadow::desktop::EditAiMaskPromptState prompt_state_;
    QFutureWatcher<EditAiMaskExecutionResult> execution_watcher_;
    QFutureWatcher<EditAiMaskApplyResult> apply_watcher_;
    QTimer pending_generation_retry_;
    std::optional<CapturedContext> context_;
    std::uint64_t candidate_proposal_token_ = 0;
    std::uint64_t candidate_generation_ = 0;
    QString candidate_source_;
    bool active_ = false;
    bool foreground_mode_ = true;
    AiMaskSelectionKind selection_kind_ = AiMaskSelectionKind::PromptedSubject;
    QString semantic_query_;
    std::uint8_t semantic_maximum_regions_ = 4;
    std::uint8_t semantic_score_threshold_percent_ = 30;
    BackendFaceRegion face_region_ = BackendFaceRegion::Face;
    QVector<BackendSubjectMaskPerson> people_;
    int selected_person_ = -1;
    std::uint32_t face_region_mask_ = 1U;
    bool people_discovery_complete_ = false;
    BackendSubjectMaskKind active_request_kind_ = BackendSubjectMaskKind::PromptedSubject;
    bool generation_pending_ = false;
    bool apply_in_flight_ = false;
};
