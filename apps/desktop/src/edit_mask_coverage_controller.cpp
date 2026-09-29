#include "edit_controller.hpp"

#include <QMetaObject>

#include <limits>
#include <optional>
#include <utility>

bool EditController::maskToolActive() const noexcept {
    return mask_tool_active_;
}

QString EditController::maskCoverageSource() const {
    return mask_coverage_source_;
}

bool EditController::maskCoverageShowsSelectedComponent() const noexcept {
    return mask_coverage_shows_selected_component_;
}

void EditController::setMaskCoverageShowsSelectedComponent(const bool selected_component) {
    if (mask_coverage_shows_selected_component_ == selected_component) {
        return;
    }
    mask_coverage_shows_selected_component_ = selected_component;
    emit maskCoverageModeChanged();
    if (mask_tool_active_) {
        invalidateMaskCoverage();
        scheduleMaskCoverageRefresh();
    }
}

void EditController::setMaskToolActive(const bool active) {
    if (mask_tool_active_ == active) {
        return;
    }
    mask_tool_active_ = active;
    invalidateMaskCoverage();
    emit maskToolActiveChanged();
    if (mask_tool_active_) {
        scheduleMaskCoverageRefresh();
    }
}

void EditController::handleMaskSelectionChanged() {
    if (!mask_tool_active_) {
        return;
    }
    invalidateMaskCoverage();
    mask_coverage_refresh_pending_ = true;

    // Grade-stack mutations emit the selection signal before they schedule
    // their own preview. Defer once so that existing render can carry the new
    // target without manufacturing a duplicate preview generation.
    QMetaObject::invokeMethod(
        this,
        [this]() {
            if (!mask_tool_active_ || !mask_coverage_refresh_pending_) {
                return;
            }
            if (preview_debounce_.isActive() || preview_queued_) {
                return;
            }
            scheduleMaskCoverageRefresh();
        },
        Qt::QueuedConnection
    );
}

void EditController::handleMaskParametersChanged() {
    if (mask_tool_active_ && !currentMaskCoverageRequest(grade_stack_).has_value()) {
        invalidateMaskCoverage();
    }
}

void EditController::handleMaskSourceIdentityChanged() {
    const bool was_active = mask_tool_active_;
    mask_tool_active_ = false;
    invalidateMaskCoverage();
    if (was_active) {
        emit maskToolActiveChanged();
    }
}

void EditController::invalidateMaskCoverage() {
    ++mask_selection_revision_;
    mask_coverage_refresh_pending_ = false;
    preview_store_->clearMaskCoverage();
    if (!mask_coverage_source_.isEmpty()) {
        mask_coverage_source_.clear();
        emit maskCoverageSourceChanged();
    }
}

void EditController::handleSelectedLocalMaskMutation() {
    if (mask_tool_active_) {
        invalidateMaskCoverage();
    }
}

void EditController::scheduleMaskCoverageRefresh() {
    if (!currentMaskCoverageRequest(grade_stack_).has_value()) {
        mask_coverage_refresh_pending_ = false;
        return;
    }
    mask_coverage_refresh_pending_ = true;
    ++render_revision_;
    if (current_rendering_ || before_rendering_) {
        preview_queued_ = true;
        cancelActivePreview(true);
        return;
    }
    preview_debounce_.start(0);
}

std::optional<EditMaskCoverageRequest>
EditController::currentMaskCoverageRequest(const BackendGradeStack& grade_stack) const {
    const int node_count = static_cast<int>(grade_stack.grade_nodes.size());
    if (!mask_tool_active_ || selected_grade_node_index_ < 0
        || selected_grade_node_index_ >= node_count) {
        return std::nullopt;
    }
    const auto& target = grade_stack.grade_nodes.at(selected_grade_node_index_);
    if (target.local_mask_components.isEmpty()) {
        return std::nullopt;
    }
    // Recipe canonicalizes an enabled sole Base to its leaf. Its selected
    // coverage is exactly the final coverage, and needs no component lookup.
    const bool single_leaf =
        target.local_mask_components.size() == 1 && target.local_mask_components.front().enabled
        && target.local_mask_components.front().operation == 0 && !target.local_mask_invert;
    const std::int32_t component_index = mask_coverage_shows_selected_component_ && !single_leaf
                                             ? selected_local_mask_component_index_
                                             : -1;
    if (component_index >= target.local_mask_components.size()) {
        return std::nullopt;
    }
    return EditMaskCoverageRequest{
        .target_layer_index = static_cast<std::uint32_t>(selected_grade_node_index_),
        .target_component_index = component_index,
        .selection_revision = mask_selection_revision_,
    };
}

MaskCoverageGeneration
EditController::maskCoverageGeneration(const EditPreviewGeneration& preview_generation) const {
    const EditMaskCoverageRequest request =
        preview_generation.mask_coverage_request.value_or(EditMaskCoverageRequest{});
    return {
        .photo = preview_generation.photo,
        .recipe_revision = preview_generation.recipe_revision,
        .target_layer_index = request.target_layer_index,
        .target_component_index = request.target_component_index,
        .selection_revision = request.selection_revision,
        .paired_preview_generation = preview_generation.current_revision,
    };
}

void EditController::publishMaskCoverage(
    BackendMaskCoverage coverage,
    const EditPreviewGeneration& preview_generation
) {
    const auto current_request = currentMaskCoverageRequest(grade_stack_);
    if (!preview_generation.mask_coverage_request.has_value() || !current_request.has_value()
        || *preview_generation.mask_coverage_request != *current_request || !coverage.available
        || coverage.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || coverage.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        preview_store_->clearMaskCoverage();
        if (!mask_coverage_source_.isEmpty()) {
            mask_coverage_source_.clear();
            emit maskCoverageSourceChanged();
        }
        return;
    }

    const MaskCoverageGeneration generation = maskCoverageGeneration(preview_generation);
    const bool published = preview_store_->publishMaskCoverage(
        EditMaskCoveragePayload{
            .samples = std::move(coverage.samples),
            .dimensions =
                QSize(static_cast<int>(coverage.width), static_cast<int>(coverage.height)),
            .row_stride_bytes = coverage.row_stride_bytes,
            .version = coverage.version,
            .target_layer_index = coverage.target_layer_index,
            .target_component_index = coverage.target_component_index,
            .selection_revision = coverage.selection_revision,
        },
        generation
    );
    if (!published) {
        preview_store_->clearMaskCoverage();
        if (!mask_coverage_source_.isEmpty()) {
            mask_coverage_source_.clear();
            emit maskCoverageSourceChanged();
        }
        return;
    }

    mask_coverage_source_ = QStringLiteral(
                                "image://shadow-edit/scope/mask/current"
                                "?photo=%1&recipe=%2&target=%3&component=%4"
                                "&selection=%5&preview=%6"
    )
                                .arg(generation.photo)
                                .arg(generation.recipe_revision)
                                .arg(generation.target_layer_index)
                                .arg(generation.target_component_index)
                                .arg(generation.selection_revision)
                                .arg(generation.paired_preview_generation);
    emit maskCoverageSourceChanged();
}
