#include "backend/desktop_backend_private.hpp"
#include "backend/edit_settings_projection.hpp"
#include "backend/rust_owned_edit_preview_frame.hpp"
#include "backend/rust_qt_projection.hpp"
#include "preview_diagnostics.hpp"

#include <QImage>
#include <QSize>
#include <QVariantMap>

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using desktop_backend_projection::checked_qt_vector_size;
using desktop_backend_projection::edit_state;
using desktop_backend_projection::ffi_edit_preview_policy;
using desktop_backend_projection::ffi_grade_node;
using desktop_backend_projection::ffi_grade_stack;
using desktop_backend_projection::grade_node;
using desktop_backend_projection::grade_stack;
using desktop_backend_projection::qbytes;
using desktop_backend_projection::qcounts;
using desktop_backend_projection::qstring;
using desktop_backend_projection::shared_grade_node;

[[nodiscard]] BackendRecipeImportItemTerminal
recipe_import_terminal(const shadow::desktop::FfiRecipeImportItemTerminal terminal) {
    using Ffi = shadow::desktop::FfiRecipeImportItemTerminal;
    switch (terminal) {
    case Ffi::Pending:
        return BackendRecipeImportItemTerminal::Pending;
    case Ffi::Running:
        return BackendRecipeImportItemTerminal::Running;
    case Ffi::Staged:
        return BackendRecipeImportItemTerminal::Staged;
    case Ffi::Completed:
        return BackendRecipeImportItemTerminal::Completed;
    case Ffi::NotFound:
        return BackendRecipeImportItemTerminal::NotFound;
    case Ffi::Unavailable:
        return BackendRecipeImportItemTerminal::Unavailable;
    case Ffi::Cancelled:
        return BackendRecipeImportItemTerminal::Cancelled;
    case Ffi::Failed:
        return BackendRecipeImportItemTerminal::Failed;
    }
    throw std::runtime_error("unsupported semantic Recipe import terminal");
}

[[nodiscard]] BackendRecipeImportItem
recipe_import_item(const shadow::desktop::FfiRecipeImportItem& item) {
    return {
        .item_id = qstring(item.item_id),
        .grade_node_id = qstring(item.grade_node_id),
        .component_id = qstring(item.component_id),
        .operation = item.operation,
        .enabled = item.enabled,
        .expansion_percent = item.expansion_percent,
        .feather_percent = item.feather_percent,
        .leaf_invert = item.leaf_invert,
        .semantic_query = qstring(item.semantic_query),
        .semantic_maximum_regions = item.semantic_maximum_regions,
        .semantic_score_threshold_percent = item.semantic_score_threshold_percent,
        .terminal = recipe_import_terminal(item.terminal),
        .generation = item.generation,
        .progress_percent = item.progress_percent,
        .detail = qstring(item.detail),
        .proposal_token = item.proposal_token,
        .preview_width = item.preview_width,
        .preview_height = item.preview_height,
        .preview_samples = qbytes(item.preview_samples),
    };
}

[[nodiscard]] BackendRecipeImportPlan
recipe_import_plan(const shadow::desktop::FfiRecipeImportPlan& plan) {
    BackendRecipeImportPlan result{
        .plan_token = plan.plan_token,
        .generation = plan.generation,
        .label = qstring(plan.label),
    };
    result.items.reserve(checked_qt_vector_size(plan.items.size(), "semantic_recipe_import_items"));
    for (const auto& item : plan.items)
        result.items.push_back(recipe_import_item(item));
    result.nodes.reserve(checked_qt_vector_size(plan.nodes.size(), "semantic_recipe_import_nodes"));
    for (const auto& node : plan.nodes) {
        result.nodes.push_back({
            .grade_node_id = qstring(node.grade_node_id),
            .label = qstring(node.label),
            .semantic_leaf_count = node.semantic_leaf_count,
            .unsupported_managed_leaf_count = node.unsupported_managed_leaf_count,
            .excluded = node.excluded,
        });
    }
    return result;
}

} // namespace

BackendPhotoEditState
DesktopBackend::photoEditState(const QString& photo_id, const QString& source_path) const {
    return edit_state(
        impl_->session->photo_edit_state(photo_id.toStdString(), source_path.toStdString())
    );
}

QByteArray DesktopBackend::exportShadowRecipe(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack& grade_stack_value,
    const QString& label
) const {
    const auto settings = ffi_grade_stack(grade_stack_value);
    return qbytes(impl_->session->export_shadow_recipe_document(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        settings,
        label.toStdString()
    ));
}

BackendShadowRecipeImportPreview
DesktopBackend::previewShadowRecipe(const QByteArray& document) const {
    const auto bytes = rust::Slice<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(document.constData()),
        static_cast<std::size_t>(document.size())
    );
    const auto preview = shadow::desktop::preview_shadow_recipe_document(bytes);
    return {
        .label = qstring(preview.label),
        .portable_grade_stack = grade_stack(preview.portable_settings),
        .grade_node_count = preview.grade_node_count,
        .portable_mask_count = preview.portable_mask_count,
        .managed_mask_node_count = preview.managed_mask_node_count,
        .semantic_mask_intent_count = preview.semantic_mask_intent_count,
        .detached_shared_node_count = preview.detached_shared_node_count,
        .removed_lut_count = preview.removed_lut_count,
        .excluded_retouch_region_count = preview.excluded_retouch_region_count,
        .excluded_completion_region_count = preview.excluded_completion_region_count,
        .excluded_liquify_stroke_count = preview.excluded_liquify_stroke_count,
        .foundation_omitted = preview.foundation_omitted,
        .raw_denoise_omitted = preview.raw_denoise_omitted,
        .canvas_omitted = preview.canvas_omitted,
    };
}

BackendRecipeImportPlan DesktopBackend::prepareSemanticRecipeImport(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const BackendGradeStack& grade_stack_value,
    const QByteArray& document
) const {
    const auto settings = ffi_grade_stack(grade_stack_value);
    const auto bytes = rust::Slice<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(document.constData()),
        static_cast<std::size_t>(document.size())
    );
    return recipe_import_plan(impl_->session->prepare_semantic_recipe_import(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        expected_working_commit_id.toStdString(),
        settings,
        bytes
    ));
}

BackendRecipeImportPlan
DesktopBackend::semanticRecipeImportPlan(const std::uint64_t plan_token) const {
    return recipe_import_plan(impl_->session->semantic_recipe_import_plan(plan_token));
}

std::uint64_t DesktopBackend::beginSemanticRecipeImportItem(
    const std::uint64_t plan_token,
    const QString& item_id
) const {
    return impl_->session->begin_semantic_recipe_import_item(plan_token, item_id.toStdString());
}

BackendRecipeImportItem DesktopBackend::executeSemanticRecipeImportItem(
    const std::uint64_t plan_token,
    const QString& item_id,
    const std::uint64_t job_token
) const {
    return recipe_import_item(impl_->session->execute_semantic_recipe_import_item(
        plan_token,
        item_id.toStdString(),
        job_token
    ));
}

BackendRecipeImportItem DesktopBackend::cancelSemanticRecipeImportItem(
    const std::uint64_t plan_token,
    const QString& item_id,
    const std::uint64_t job_token
) const {
    return recipe_import_item(impl_->session->cancel_semantic_recipe_import_item(
        plan_token,
        item_id.toStdString(),
        job_token
    ));
}

BackendRecipeImportItem DesktopBackend::acceptSemanticRecipeImportItem(
    const std::uint64_t plan_token,
    const QString& item_id,
    const std::uint64_t proposal_token,
    const std::uint64_t generation
) const {
    return recipe_import_item(impl_->session->accept_semantic_recipe_import_item(
        plan_token,
        item_id.toStdString(),
        proposal_token,
        generation
    ));
}

BackendRecipeImportPlan DesktopBackend::excludeSemanticRecipeImportNode(
    const std::uint64_t plan_token,
    const QString& grade_node_id
) const {
    return recipe_import_plan(
        impl_->session->exclude_semantic_recipe_import_node(plan_token, grade_node_id.toStdString())
    );
}

BackendGradeStack DesktopBackend::finalizeSemanticRecipeImport(
    const std::uint64_t plan_token,
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const BackendGradeStack& grade_stack_value
) const {
    const auto settings = ffi_grade_stack(grade_stack_value);
    return grade_stack(impl_->session->finalize_semantic_recipe_import(
        plan_token,
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        expected_working_commit_id.toStdString(),
        settings
    ));
}

void DesktopBackend::closeSemanticRecipeImport(const std::uint64_t plan_token) const {
    impl_->session->close_semantic_recipe_import(plan_token);
}

BackendPhotoEditState DesktopBackend::resetIncompatiblePhotoEditHistory(
    const QString& photo_id,
    const QString& source_path
) const {
    return edit_state(impl_->session->reset_incompatible_photo_edit_history(
        photo_id.toStdString(),
        source_path.toStdString()
    ));
}

QVariantList
DesktopBackend::opticsProfileCandidates(const QString& photo_id, const QString& source_path) const {
    const auto candidates = impl_->session->optics_profile_candidates(
        photo_id.toStdString(),
        source_path.toStdString()
    );
    QVariantList result;
    result.reserve(checked_qt_vector_size(candidates.size(), "optics_profile_candidates"));
    for (const auto& candidate : candidates) {
        result.push_back(
            QVariantMap{
                {QStringLiteral("cameraMaker"), qstring(candidate.camera_maker)},
                {QStringLiteral("cameraModel"), qstring(candidate.camera_model)},
                {QStringLiteral("lensMaker"), qstring(candidate.lens_maker)},
                {QStringLiteral("lensModel"), qstring(candidate.lens_model)},
            }
        );
    }
    return result;
}

QVector<BackendSharedGradeNode> DesktopBackend::sharedGradeNodes() const {
    const auto shared = impl_->session->shared_grade_nodes();
    QVector<BackendSharedGradeNode> result;
    result.reserve(checked_qt_vector_size(shared.size(), "shared_grade_nodes"));
    for (const auto& node : shared) {
        result.push_back(shared_grade_node(node));
    }
    return result;
}

BackendSharedGradeNode DesktopBackend::publishSharedGradeNode(
    const QString& label,
    const BackendGradeNode& grade_node
) const {
    const auto ffi_node = ffi_grade_node(grade_node);
    return shared_grade_node(
        impl_->session->publish_shared_grade_node(label.toStdString(), ffi_node)
    );
}

BackendBatchGradeReceipt DesktopBackend::applySharedGradeNodeToPhotos(
    const QString& layer_id,
    const QVector<BackendBatchPhotoTarget>& targets
) const {
    rust::Vec<shadow::desktop::FfiBatchPhotoTarget> ffi_targets;
    ffi_targets.reserve(static_cast<std::size_t>(targets.size()));
    for (const auto& target : targets) {
        shadow::desktop::FfiBatchPhotoTarget ffi_target;
        ffi_target.photo_id = target.photo_id.toStdString();
        ffi_target.source_path = target.source_path.toStdString();
        ffi_targets.push_back(std::move(ffi_target));
    }
    const auto receipt = impl_->session->apply_shared_grade_node_to_photos(
        layer_id.toStdString(),
        std::move(ffi_targets)
    );
    BackendBatchGradeReceipt result{
        .requested = receipt.requested,
        .updated = receipt.updated,
        .unchanged = receipt.unchanged,
        .failed = receipt.failed,
    };
    result.errors.reserve(checked_qt_vector_size(receipt.errors.size(), "batch_grade_errors"));
    for (const auto& error : receipt.errors) {
        result.errors.push_back(qstring(error));
    }
    return result;
}

BackendGradeNode DesktopBackend::newBasicGradeNode(const QString& label) const {
    return grade_node(shadow::desktop::new_basic_grade_node(label.toStdString()));
}

BackendEditedPreview DesktopBackend::renderEditPreview(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack& grade_stack,
    const std::uint64_t render_token,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality,
    const EditPreviewPolicy policy,
    const std::optional<EditMaskCoverageRequest> mask_coverage_request
) const {
    shadow::desktop::FfiEditPreviewRequest request;
    std::string ffi_photo_id;
    std::string ffi_source_path;
    try {
        request.base_commit_id = base_commit_id.toStdString();
        request.settings = ffi_grade_stack(grade_stack);
        request.render_token = render_token;
        request.max_edge = max_edge;
        request.jpeg_quality = jpeg_quality;
        request.policy = ffi_edit_preview_policy(policy);
        request.use_working_recipe = edit_preview_kind(policy) == EditPreviewKind::Current;
        request.mask_coverage_requested = mask_coverage_request.has_value();
        if (mask_coverage_request.has_value()
            && mask_coverage_request->target_component_index < -1) {
            throw std::invalid_argument("mask coverage component target is invalid");
        }
        request.mask_coverage_target_layer_index =
            mask_coverage_request.has_value() ? mask_coverage_request->target_layer_index : 0U;
        request.mask_coverage_component_requested =
            mask_coverage_request.has_value() && mask_coverage_request->target_component_index >= 0;
        request.mask_coverage_target_component_index =
            request.mask_coverage_component_requested
                ? static_cast<std::uint32_t>(mask_coverage_request->target_component_index)
                : 0U;
        request.mask_selection_revision =
            mask_coverage_request.has_value() ? mask_coverage_request->selection_revision : 0U;
        ffi_photo_id = photo_id.toStdString();
        ffi_source_path = source_path.toStdString();
    } catch (...) {
        const std::exception_ptr construction_error = std::current_exception();
        try {
            if (impl_->session->claim_basic_edit_preview_terminal(render_token)
                == shadow::desktop::FfiEditPreviewTerminal::Cancelled) {
                return {
                    .terminal = EditPreviewTerminal::Cancelled,
                };
            }
        } catch (...) {
            // Preserve the actual request-construction failure. Registry
            // diagnostics cannot make a malformed request more actionable.
        }
        std::rethrow_exception(construction_error);
    }
    auto owned_payload =
        impl_->session->render_basic_edit_preview_owned(ffi_photo_id, ffi_source_path, request);
    const auto& payload = owned_payload->projection();
    if (payload.terminal == shadow::desktop::FfiEditPreviewTerminal::Cancelled) {
        return {
            .terminal = EditPreviewTerminal::Cancelled,
        };
    }
    if (payload.terminal != shadow::desktop::FfiEditPreviewTerminal::Completed) {
        throw std::runtime_error("edit preview returned an unknown terminal state");
    }
    const bool interactive = policy == EditPreviewPolicy::Interactive;
    if (owned_payload->interactive_frame_available() != interactive) {
        throw std::runtime_error("edit preview ownership is inconsistent with its explicit policy");
    }
    const QByteArray preview_bytes = interactive ? QByteArray{} : qbytes(payload.bytes);
    const QSize preview_dimensions(
        static_cast<int>(payload.width),
        static_cast<int>(payload.height)
    );
    if (payload.level_zero_width == 0U || payload.level_zero_height == 0U) {
        throw std::runtime_error("edit preview returned empty level-zero geometry");
    }
    const std::uint64_t expected_rgb8_stride = static_cast<std::uint64_t>(payload.width) * 3U;
    if ((interactive
         && (payload.row_stride_bytes != expected_rgb8_stride || !payload.bytes.empty()))
        || (!interactive && payload.row_stride_bytes != 0U)) {
        throw std::runtime_error(
            "edit preview returned a payload layout "
            "inconsistent with its explicit policy"
        );
    }
    const auto interactive_mask_coverage_samples =
        owned_payload->interactive_mask_coverage_samples();
    const QByteArray mask_coverage_samples =
        interactive ? QByteArray{} : qbytes(payload.mask_coverage_samples);
    const bool empty_mask_coverage_sentinel =
        payload.mask_coverage_version == 0U && payload.mask_coverage_target_layer_index == 0U
        && !payload.mask_coverage_component_selected
        && payload.mask_coverage_target_component_index == 0U
        && payload.mask_selection_revision == 0U && payload.mask_coverage_width == 0U
        && payload.mask_coverage_height == 0U && payload.mask_coverage_row_stride_bytes == 0U
        && mask_coverage_samples.isEmpty() && interactive_mask_coverage_samples.empty();
    if (!payload.mask_coverage_available) {
        if (!empty_mask_coverage_sentinel) {
            throw std::runtime_error("unavailable mask coverage returned non-empty sentinels");
        }
    } else {
        const std::uint64_t expected_mask_bytes =
            static_cast<std::uint64_t>(payload.mask_coverage_width)
            * static_cast<std::uint64_t>(payload.mask_coverage_height);
        if (!mask_coverage_request.has_value()
            || payload.mask_coverage_version != EDIT_MASK_COVERAGE_VERSION
            || payload.mask_coverage_target_layer_index != mask_coverage_request->target_layer_index
            || payload.mask_coverage_component_selected
                   != (mask_coverage_request->target_component_index >= 0)
            || (payload.mask_coverage_component_selected
                && payload.mask_coverage_target_component_index
                       != static_cast<std::uint32_t>(mask_coverage_request->target_component_index))
            || payload.mask_selection_revision != mask_coverage_request->selection_revision
            || payload.mask_coverage_width != payload.width
            || payload.mask_coverage_height != payload.height
            || payload.mask_coverage_row_stride_bytes != payload.mask_coverage_width
            || expected_mask_bytes
                   != static_cast<std::uint64_t>(
                       interactive ? interactive_mask_coverage_samples.size()
                                   : static_cast<std::size_t>(mask_coverage_samples.size())
                   )) {
            throw std::runtime_error(
                "edit preview returned mask coverage inconsistent with its request"
            );
        }
    }
    const PreviewSensorClippingMask sensor_clipping{
        .available = payload.sensor_clipping_available,
        .dimensions = QSize(
            static_cast<int>(payload.sensor_clipping_width),
            static_cast<int>(payload.sensor_clipping_height)
        ),
        .samples = qbytes(payload.sensor_clipping_mask),
        .highlight_pixel_count = payload.sensor_highlight_clipped_pixels,
        .shadow_pixel_count = payload.sensor_shadow_clipped_pixels,
    };
    if (payload.analysis_available != edit_preview_requires_analysis(policy)) {
        throw std::runtime_error(
            "edit preview returned analysis inconsistent with its explicit policy"
        );
    }
    BackendEditedPreview result{
        .bytes = preview_bytes,
        .row_stride_bytes = payload.row_stride_bytes,
        .analysis =
            {
                .available = payload.analysis_available,
                .version = qstring(payload.analysis_version),
                .red = qcounts(payload.red_histogram, "red_histogram"),
                .green = qcounts(payload.green_histogram, "green_histogram"),
                .blue = qcounts(payload.blue_histogram, "blue_histogram"),
                .luma = qcounts(payload.luma_histogram, "luma_histogram"),
                .below_zero_samples = qcounts(payload.below_zero_samples, "below_zero_samples"),
                .above_one_samples = qcounts(payload.above_one_samples, "above_one_samples"),
                .hdr_headroom_bins = qcounts(payload.hdr_headroom_bins, "hdr_headroom_bins"),
                .hdr_headroom_pixels = payload.hdr_headroom_pixels,
                .hdr_peak_headroom_ev = payload.hdr_peak_headroom_ev,
                .width = payload.analysis_width,
                .height = payload.analysis_height,
                .pixel_count = payload.pixel_count,
                .shadow_clipped_pixels = payload.shadow_clipped_pixels,
                .highlight_clipped_pixels = payload.highlight_clipped_pixels,
            },
        // Interactive frames intentionally avoid decoding their just-encoded
        // JPEG a second time merely to build a transient zebra raster.
        .display_zebra =
            edit_preview_requires_display_diagnostics(policy)
                ? make_clipping_zebra_overlay(preview_dimensions, preview_bytes, sensor_clipping)
                : QImage{},
        .mask_coverage =
            {
                .samples = mask_coverage_samples,
                .version = payload.mask_coverage_version,
                .target_layer_index = payload.mask_coverage_target_layer_index,
                .target_component_index =
                    payload.mask_coverage_component_selected
                        ? static_cast<std::int32_t>(payload.mask_coverage_target_component_index)
                        : -1,
                .selection_revision = payload.mask_selection_revision,
                .width = payload.mask_coverage_width,
                .height = payload.mask_coverage_height,
                .row_stride_bytes = payload.mask_coverage_row_stride_bytes,
                .available = payload.mask_coverage_available,
            },
        .optics =
            {
                .status = qstring(payload.optics_status),
                .provider_id = qstring(payload.optics_provider_id),
                .provider_version = qstring(payload.optics_provider_version),
                .camera_profile = qstring(payload.optics_camera_profile),
                .lens_profile = qstring(payload.optics_lens_profile),
                .distortion_available = payload.optics_distortion_available,
                .tca_available = payload.optics_tca_available,
                .vignetting_available = payload.optics_vignetting_available,
                .applied_distortion = payload.optics_applied_distortion,
                .applied_tca = payload.optics_applied_tca,
                .applied_vignetting = payload.optics_applied_vignetting,
                .vignetting_used_distance_fallback =
                    payload.optics_vignetting_used_distance_fallback,
                .applied_scaling = payload.optics_applied_scaling,
            },
        .width = payload.width,
        .height = payload.height,
        .level_zero_width = payload.level_zero_width,
        .level_zero_height = payload.level_zero_height,
        .terminal = EditPreviewTerminal::Completed,
    };
    if (interactive) {
        result.frame = makeRustOwnedEditPreviewFrame(std::move(owned_payload));
    }
    return result;
}

BackendRawWhiteBalancePickerResult DesktopBackend::pickRawWhiteBalance(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack& grade_stack,
    const std::uint32_t max_edge,
    const double normalized_x,
    const double normalized_y
) const {
    shadow::desktop::FfiEditPreviewRequest request;
    request.base_commit_id = base_commit_id.toStdString();
    request.settings = ffi_grade_stack(grade_stack);
    // Picker lookups do not participate in preview terminal arbitration: they
    // only consult the already-admitted immutable session.
    request.render_token = 0U;
    request.max_edge = max_edge;
    request.jpeg_quality = 90U;
    request.policy = shadow::desktop::FfiEditPreviewPolicy::Interactive;
    request.use_working_recipe = true;
    request.mask_coverage_requested = false;
    request.mask_coverage_target_layer_index = 0U;
    request.mask_coverage_component_requested = false;
    request.mask_coverage_target_component_index = 0U;
    request.mask_selection_revision = 0U;
    const auto result = impl_->session->pick_raw_white_balance(
        photo_id.toStdString(),
        source_path.toStdString(),
        request,
        normalized_x,
        normalized_y
    );
    return {
        .available = result.available,
        .temperature_kelvin = result.temperature_kelvin,
        .tint = result.tint,
    };
}

BackendRawWhiteBalancePickerResult DesktopBackend::autoRawWhiteBalance(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack& grade_stack,
    const std::uint32_t max_edge
) const {
    shadow::desktop::FfiEditPreviewRequest request;
    request.base_commit_id = base_commit_id.toStdString();
    request.settings = ffi_grade_stack(grade_stack);
    // The one-shot estimator only consults the already-admitted immutable CFA
    // session and does not participate in preview terminal arbitration.
    request.render_token = 0U;
    request.max_edge = max_edge;
    request.jpeg_quality = 90U;
    request.policy = shadow::desktop::FfiEditPreviewPolicy::Interactive;
    request.use_working_recipe = true;
    request.mask_coverage_requested = false;
    request.mask_coverage_target_layer_index = 0U;
    request.mask_coverage_component_requested = false;
    request.mask_coverage_target_component_index = 0U;
    request.mask_selection_revision = 0U;
    const auto result = impl_->session->auto_raw_white_balance(
        photo_id.toStdString(),
        source_path.toStdString(),
        request
    );
    return {
        .available = result.available,
        .temperature_kelvin = result.temperature_kelvin,
        .tint = result.tint,
    };
}

std::uint64_t DesktopBackend::beginEditPreviewRequest() const noexcept {
    return impl_->session->begin_basic_edit_preview();
}

bool DesktopBackend::cancelEditPreviewRequest(const std::uint64_t render_token) const noexcept {
    return impl_->session->cancel_basic_edit_preview(render_token);
}

std::uint64_t DesktopBackend::beginEditDetailRequest() const noexcept {
    return impl_->session->begin_basic_edit_detail();
}

BackendEditedDetailViewport DesktopBackend::renderEditDetailViewport(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack& grade_stack,
    const std::uint64_t render_token,
    const double center_x,
    const double center_y,
    const std::uint32_t viewport_width,
    const std::uint32_t viewport_height,
    const std::uint32_t tile_side,
    const bool use_working_recipe
) const {
    shadow::desktop::FfiEditDetailViewportRequest request;
    request.base_commit_id = base_commit_id.toStdString();
    request.settings = ffi_grade_stack(grade_stack);
    request.render_token = render_token;
    request.center_x = center_x;
    request.center_y = center_y;
    request.viewport_width = viewport_width;
    request.viewport_height = viewport_height;
    request.tile_side = tile_side;
    request.use_working_recipe = use_working_recipe;
    const auto payload = impl_->session->render_basic_edit_detail_viewport(
        photo_id.toStdString(),
        source_path.toStdString(),
        request
    );
    BackendEditedDetailViewport result;
    result.full_width = payload.full_width;
    result.full_height = payload.full_height;
    result.retained_bytes = payload.retained_bytes;
    result.tiles.reserve(static_cast<qsizetype>(payload.tiles.size()));
    for (const auto& tile : payload.tiles) {
        result.tiles.push_back({
            .bytes = qbytes(tile.bytes),
            .x = tile.x,
            .y = tile.y,
            .width = tile.width,
            .height = tile.height,
            .row_stride_bytes = tile.row_stride_bytes,
        });
    }
    return result;
}

BackendPhotoEditState DesktopBackend::saveEditVersion(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const QString& expected_variant_id,
    const BackendGradeStack& grade_stack,
    const QString& version_name
) const {
    const auto ffi = ffi_grade_stack(grade_stack);
    return edit_state(impl_->session->save_basic_edit_version(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        expected_working_commit_id.toStdString(),
        expected_variant_id.toStdString(),
        ffi,
        version_name.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::autosaveWorkingEdit(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const QString& expected_variant_id,
    const BackendGradeStack& grade_stack
) const {
    const auto ffi = ffi_grade_stack(grade_stack);
    return edit_state(impl_->session->autosave_basic_edit_working(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        expected_working_commit_id.toStdString(),
        expected_variant_id.toStdString(),
        ffi
    ));
}

BackendPhotoEditState DesktopBackend::createPhotoVariant(
    const QString& photo_id,
    const QString& source_path,
    const QString& name
) const {
    return edit_state(impl_->session->create_photo_variant(
        photo_id.toStdString(),
        source_path.toStdString(),
        name.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::renamePhotoVariant(
    const QString& photo_id,
    const QString& source_path,
    const QString& variant_id,
    const QString& name
) const {
    return edit_state(impl_->session->rename_photo_variant(
        photo_id.toStdString(),
        source_path.toStdString(),
        variant_id.toStdString(),
        name.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::activatePhotoVariant(
    const QString& photo_id,
    const QString& source_path,
    const QString& variant_id
) const {
    return edit_state(impl_->session->activate_photo_variant(
        photo_id.toStdString(),
        source_path.toStdString(),
        variant_id.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::removePhotoVariant(
    const QString& photo_id,
    const QString& source_path,
    const QString& variant_id
) const {
    return edit_state(impl_->session->remove_photo_variant(
        photo_id.toStdString(),
        source_path.toStdString(),
        variant_id.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::loadEditVersionDraft(
    const QString& photo_id,
    const QString& source_path,
    const QString& commit_id
) const {
    return edit_state(impl_->session->checkout_basic_edit_version(
        photo_id.toStdString(),
        source_path.toStdString(),
        commit_id.toStdString()
    ));
}
