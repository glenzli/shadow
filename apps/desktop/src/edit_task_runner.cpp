#include "edit_task_runner.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t EDIT_DETAIL_TILE_SIDE = 512;
constexpr std::uint32_t EDIT_LARGE_DETAIL_TILE_SIDE = 1'024;
constexpr std::uint64_t EDIT_DETAIL_MAX_PRESENTATION_BYTES =
    96U * 1'024U * 1'024U;

[[nodiscard]] BackendEditedDetailViewport compose_detail_viewport(
    BackendEditedDetailViewport viewport
) {
    if (viewport.full_width == 0 || viewport.full_height == 0 || viewport.tiles.isEmpty()) {
        throw std::runtime_error("full detail returned no RGB8 tiles");
    }
    std::uint32_t left = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t top = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;
    std::uint64_t tile_pixels = 0;
    for (qsizetype index = 0; index < viewport.tiles.size(); ++index) {
        const auto& tile = viewport.tiles.at(index);
        const std::uint64_t expected_stride = static_cast<std::uint64_t>(tile.width) * 3U;
        const std::uint64_t expected_bytes = expected_stride * tile.height;
        const std::uint64_t tile_right = static_cast<std::uint64_t>(tile.x) + tile.width;
        const std::uint64_t tile_bottom = static_cast<std::uint64_t>(tile.y) + tile.height;
        if (tile.width == 0 || tile.height == 0 || tile.row_stride_bytes != expected_stride
            || expected_bytes != static_cast<std::uint64_t>(tile.bytes.size())
            || tile_right > viewport.full_width || tile_bottom > viewport.full_height) {
            throw std::runtime_error("full detail returned an invalid RGB8 tile layout");
        }
        for (qsizetype prior_index = 0; prior_index < index; ++prior_index) {
            const auto& prior = viewport.tiles.at(prior_index);
            const bool overlaps = tile.x < prior.x + prior.width
                && prior.x < tile.x + tile.width && tile.y < prior.y + prior.height
                && prior.y < tile.y + tile.height;
            if (overlaps) {
                throw std::runtime_error("full detail returned overlapping RGB8 tiles");
            }
        }
        left = std::min(left, tile.x);
        top = std::min(top, tile.y);
        right = std::max(right, static_cast<std::uint32_t>(tile_right));
        bottom = std::max(bottom, static_cast<std::uint32_t>(tile_bottom));
        tile_pixels += static_cast<std::uint64_t>(tile.width) * tile.height;
    }

    const std::uint32_t presentation_width = right - left;
    const std::uint32_t presentation_height = bottom - top;
    const std::uint64_t presentation_pixels =
        static_cast<std::uint64_t>(presentation_width) * presentation_height;
    const std::uint64_t presentation_bytes = presentation_pixels * 3U;
    if (presentation_width == 0 || presentation_height == 0
        || tile_pixels != presentation_pixels
        || presentation_bytes
            > static_cast<std::uint64_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::runtime_error("full detail tiles do not cover one complete viewport");
    }
    if (presentation_bytes > EDIT_DETAIL_MAX_PRESENTATION_BYTES) {
        throw std::runtime_error("full detail viewport exceeds the 96 MiB RGB limit");
    }

    QByteArray composite;
    composite.resize(static_cast<qsizetype>(presentation_bytes));
    composite.fill('\0');
    for (const auto& tile : viewport.tiles) {
        for (std::uint32_t row = 0; row < tile.height; ++row) {
            const std::size_t destination_offset =
                (static_cast<std::size_t>(tile.y - top + row) * presentation_width
                 + (tile.x - left))
                * 3U;
            const std::size_t source_offset =
                static_cast<std::size_t>(row) * tile.row_stride_bytes;
            std::memcpy(
                composite.data() + destination_offset,
                tile.bytes.constData() + source_offset,
                static_cast<std::size_t>(tile.row_stride_bytes)
            );
        }
    }
    viewport.tiles = {{
        .bytes = std::move(composite),
        .x = left,
        .y = top,
        .width = presentation_width,
        .height = presentation_height,
        .row_stride_bytes = presentation_width * 3U,
    }};
    return viewport;
}

} // namespace

namespace EditTaskRunner {

EditStateTaskResult loadState(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Open;
    try {
        result.state = backend->photoEditState(photo_id, source_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

EditStateTaskResult resetIncompatibleRecipeState(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::ResetIncompatibleRecipe;
    try {
        result.state = backend->resetIncompatiblePhotoEditHistory(photo_id, source_path);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

EditStateTaskResult saveState(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const BackendGradeStack grade_stack,
    const QString& version_name,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Save;
    try {
        result.state = backend->saveEditVersion(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            grade_stack,
            version_name
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

EditStateTaskResult autosaveState(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const BackendGradeStack grade_stack,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::Autosave;
    try {
        result.state = backend->autosaveWorkingEdit(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            grade_stack
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

EditStateTaskResult loadVersionDraftState(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& commit_id,
    const quint64 generation
) {
    EditStateTaskResult result;
    result.photo_generation = generation;
    result.kind = EditStateTaskKind::LoadDraft;
    try {
        result.state = backend->loadEditVersionDraft(
            photo_id,
            source_path,
            commit_id
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

EditPreviewTaskResult renderPreview(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack grade_stack,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality,
    const EditPreviewGeneration generation
) {
    EditPreviewTaskResult result;
    result.generation = generation;
    try {
        result.preview = backend->renderEditPreview(
            photo_id,
            source_path,
            base_commit_id,
            grade_stack,
            max_edge,
            jpeg_quality,
            generation.policy
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

EditDetailTaskResult renderDetail(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack grade_stack,
    const std::uint64_t render_token,
    const double center_x,
    const double center_y,
    const std::uint32_t viewport_width,
    const std::uint32_t viewport_height,
    const EditDetailGeneration generation
) {
    EditDetailTaskResult result;
    result.generation = generation;
    try {
        const std::uint32_t tile_side = std::max(viewport_width, viewport_height) > 4'096U
            ? EDIT_LARGE_DETAIL_TILE_SIDE
            : EDIT_DETAIL_TILE_SIDE;
        result.viewport = compose_detail_viewport(
            backend->renderEditDetailViewport(
                photo_id,
                source_path,
                base_commit_id,
                grade_stack,
                render_token,
                center_x,
                center_y,
                viewport_width,
                viewport_height,
                tile_side,
                true
            )
        );
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

EditDetailWarmupTaskResult warmDetailSource(
    const std::shared_ptr<DesktopBackend>& backend,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack grade_stack,
    const std::uint64_t render_token,
    const quint64 photo_generation,
    const quint64 render_revision
) {
    EditDetailWarmupTaskResult result;
    result.photo_generation = photo_generation;
    result.render_revision = render_revision;
    try {
        // One native 512px tile is enough to force the provider-neutral
        // full-resolution source preparation and prime the center of the
        // bounded Recipe-tile cache. Do not compose or publish it: this is an
        // idle optimisation only, never a hidden viewport change.
        const BackendEditedDetailViewport viewport =
            backend->renderEditDetailViewport(
            photo_id,
            source_path,
            base_commit_id,
            grade_stack,
            render_token,
            0.5,
            0.5,
            EDIT_DETAIL_TILE_SIDE,
            EDIT_DETAIL_TILE_SIDE,
            EDIT_DETAIL_TILE_SIDE,
            true
        );
        result.retained_bytes = viewport.retained_bytes;
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

} // namespace EditTaskRunner
