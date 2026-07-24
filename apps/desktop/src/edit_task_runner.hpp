#pragma once

#include "desktop_backend.hpp"
#include "edit_preview_contract.hpp"

#include <QString>

#include <cstdint>
#include <memory>

// Results returned from worker threads. The controller owns UI state and
// acceptance; this module only owns background calls into DesktopBackend.
enum class EditStateTaskKind : std::uint8_t {
    Open,
    ResetIncompatibleRecipe,
    Save,
    Autosave,
    LoadDraft,
};

struct EditStateTaskResult final {
    BackendPhotoEditState state;
    QString error;
    quint64 photo_generation = 0;
    EditStateTaskKind kind = EditStateTaskKind::Open;
};

struct EditPreviewTaskResult final {
    BackendEditedPreview preview;
    QString error;
    EditPreviewGeneration generation;
};

struct EditDetailTaskResult final {
    BackendEditedDetailViewport viewport;
    QString error;
    EditDetailGeneration generation;
};

// A deliberately invisible idle task. It warms the same full-resolution
// source and center tile used by the interactive detail path, but never
// publishes pixels or changes the visible viewport.
struct EditDetailWarmupTaskResult final {
    QString error;
    quint64 photo_generation = 0;
    quint64 render_revision = 0;
    quint64 retained_bytes = 0;
};

namespace EditTaskRunner {

[[nodiscard]] EditStateTaskResult loadState(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    quint64 generation
);

[[nodiscard]] EditStateTaskResult resetIncompatibleRecipeState(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    quint64 generation
);

[[nodiscard]] EditStateTaskResult saveState(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    BackendGradeStack grade_stack,
    const QString& version_name,
    quint64 generation
);

[[nodiscard]] EditStateTaskResult autosaveState(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    BackendGradeStack grade_stack,
    quint64 generation
);

[[nodiscard]] EditStateTaskResult loadVersionDraftState(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& commit_id,
    quint64 generation
);

[[nodiscard]] EditPreviewTaskResult renderPreview(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    BackendGradeStack grade_stack,
    std::uint32_t max_edge,
    std::uint8_t jpeg_quality,
    EditPreviewGeneration generation
);

[[nodiscard]] EditDetailTaskResult renderDetail(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    BackendGradeStack grade_stack,
    std::uint64_t render_token,
    double center_x,
    double center_y,
    std::uint32_t viewport_width,
    std::uint32_t viewport_height,
    EditDetailGeneration generation
);

[[nodiscard]] EditDetailWarmupTaskResult warmDetailSource(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    BackendGradeStack grade_stack,
    std::uint64_t render_token,
    quint64 photo_generation,
    quint64 render_revision
);

} // namespace EditTaskRunner
