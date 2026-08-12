#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"

#include <shadow/image/auto_geometry.hpp>

#include <QFutureWatcher>
#include <QString>

#include <cstdint>

class EditController;

struct EditAutoGeometryTaskResult final {
    shadow::image::AutoGeometryProposal proposal;
    QString error;
    QString photo_id;
    std::uint64_t photo_generation = 0U;
    std::uint64_t render_revision = 0U;
    std::uint64_t request_sequence = 0U;
    BackendPhotoGeometry source_geometry;
};

/// Owns deterministic geometry analysis, stale-result rejection, transient
/// proposal preview, and the one-step accept/cancel lifecycle. The proposal is
/// never copied into the authoritative Grade Stack until explicit acceptance.
class EditAutoGeometryController final {
  public:
    explicit EditAutoGeometryController(EditController& owner);
    ~EditAutoGeometryController();

    EditAutoGeometryController(const EditAutoGeometryController&) = delete;
    EditAutoGeometryController& operator=(const EditAutoGeometryController&) = delete;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool canAnalyze() const noexcept;
    [[nodiscard]] bool hasProposal() const noexcept;
    [[nodiscard]] bool previewing() const noexcept;
    [[nodiscard]] int confidencePercent() const noexcept;
    [[nodiscard]] double suggestedStraightenDegrees() const noexcept;
    [[nodiscard]] double suggestedPerspectiveVertical() const noexcept;
    [[nodiscard]] double suggestedPerspectiveHorizontal() const noexcept;
    [[nodiscard]] int supportingLines() const noexcept;
    [[nodiscard]] QString statusText() const;

    void analyze(int mode);
    void accept();
    void cancel();
    void resetContext();
    void retranslateUi();
    [[nodiscard]] bool applyPreviewOverride(BackendPhotoGeometry& geometry) const noexcept;

  private:
    void finishAnalysis();
    void handlePreviewSettled(std::uint64_t render_revision);
    void handlePreviewFailed(std::uint64_t render_revision, const QString& error);
    void publishChange(bool previous_owner_busy);
    [[nodiscard]] bool
    resultContextIsCurrent(const EditAutoGeometryTaskResult& result) const noexcept;

    EditController& owner_;
    QFutureWatcher<EditAutoGeometryTaskResult> watcher_;
    shadow::image::AutoGeometryProposal proposal_;
    BackendPhotoGeometry preview_geometry_;
    LocalizedUiMessage status_message_;
    std::uint64_t request_sequence_ = 0U;
    std::uint64_t proposal_render_revision_ = 0U;
    bool has_proposal_ = false;
    bool preview_ready_ = false;

    friend class EditController;
};
