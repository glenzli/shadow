#pragma once

#include "desktop_backend.hpp"

#include <QFutureWatcher>
#include <QTimer>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <optional>

class EditController;

struct EditImageCompletionExecutionResult final {
    BackendImageCompletionResult result;
    QString error;
};

struct EditImageCompletionApplyResult final {
    BackendPhotoEditState state;
    QString error;
};

// Owns transient brush authoring, the cancellable Infer Runtime proposal, and
// the separate apply transaction. Accepted regions live in BackendGradeStack;
// this controller never becomes a second persistence subsystem.
class EditAiCompletionController final {
  public:
    EditAiCompletionController(EditController& owner, std::shared_ptr<DesktopBackend> backend);
    ~EditAiCompletionController();

    EditAiCompletionController(const EditAiCompletionController&) = delete;
    EditAiCompletionController& operator=(const EditAiCompletionController&) = delete;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool locksInteraction() const noexcept;
    [[nodiscard]] bool canGenerate() const noexcept;
    [[nodiscard]] bool hasCandidate() const noexcept;
    [[nodiscard]] QString candidateSource() const;
    [[nodiscard]] QVariantList brushPoints() const;
    [[nodiscard]] double brushRadius() const noexcept;
    [[nodiscard]] double selectionExpansion() const noexcept;
    [[nodiscard]] bool eraseMode() const noexcept;

    [[nodiscard]] bool begin();
    [[nodiscard]] bool refreshRegion(int index);
    [[nodiscard]] std::uint32_t beginStroke();
    void appendPoint(double x, double y, std::uint32_t stroke_id);
    void undoStroke();
    void clearSelection();
    void generate();
    void retry();
    void applyCandidate();
    void cancel();
    void setBrushRadius(double radius);
    void setSelectionExpansion(double expansion);
    void setEraseMode(bool erase);
    void resetContext();

  private:
    struct Context final {
        QString photo_id;
        QString source_path;
        std::uint64_t photo_generation = 0;
    };

    void tryStartPendingGeneration();
    void startGeneration();
    void finishExecution();
    void finishApply();
    void retireCandidate() noexcept;
    void discardProposal(std::uint64_t token) const noexcept;
    void publishStateChange();
    [[nodiscard]] bool contextIsCurrent() const noexcept;
    [[nodiscard]] bool hasPaintedPoint() const noexcept;
    [[nodiscard]] double
    effectiveRadius(const BackendImageCompletionBrushPoint& point) const noexcept;
    [[nodiscard]] QVector<BackendImageCompletionBrushPoint> effectivePoints() const;

    EditController& owner_;
    std::shared_ptr<DesktopBackend> backend_;
    QFutureWatcher<EditImageCompletionExecutionResult> execution_watcher_;
    QFutureWatcher<EditImageCompletionApplyResult> apply_watcher_;
    QTimer pending_generation_retry_;
    std::optional<Context> context_;
    QVector<BackendImageCompletionBrushPoint> points_;
    BackendGradeStack submitted_grade_stack_;
    QString submitted_base_commit_id_;
    std::uint64_t active_job_token_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t candidate_proposal_token_ = 0;
    std::uint64_t candidate_generation_ = 0;
    std::uint64_t applying_proposal_token_ = 0;
    QString candidate_source_;
    std::uint32_t next_stroke_id_ = 0;
    int refresh_region_index_ = -1;
    double brush_radius_ = 0.04;
    double selection_expansion_ = 0.0;
    bool erase_mode_ = false;
    bool active_ = false;
    bool generation_pending_ = false;
    bool apply_in_flight_ = false;
};
