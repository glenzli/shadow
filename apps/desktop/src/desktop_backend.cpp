#include "desktop_backend.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

[[nodiscard]] QString qstring(const rust::String& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(length));
}

[[nodiscard]] QByteArray qbytes(const rust::Vec<std::uint8_t>& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QByteArray(
        reinterpret_cast<const char*>(value.data()),
        static_cast<qsizetype>(length)
    );
}

[[nodiscard]] qsizetype checked_qt_vector_size(
    const std::size_t size,
    const char* const field
) {
    if (size > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::length_error(std::string("desktop bridge vector is too large: ") + field);
    }
    return static_cast<qsizetype>(size);
}

[[nodiscard]] shadow::desktop::FfiBasicEditParameters ffi_parameters(
    const BackendBasicEditParameters& source
) {
    return {
        .exposure_stops = source.exposure_stops,
        .contrast_factor = source.contrast_factor,
        .red_channel_gain = source.red_channel_gain,
        .green_channel_gain = source.green_channel_gain,
        .blue_channel_gain = source.blue_channel_gain,
        .saturation_factor = source.saturation_factor,
    };
}

[[nodiscard]] BackendBasicEditParameters edit_parameters(
    const shadow::desktop::FfiBasicEditParameters& source
) {
    return {
        .exposure_stops = source.exposure_stops,
        .contrast_factor = source.contrast_factor,
        .red_channel_gain = source.red_channel_gain,
        .green_channel_gain = source.green_channel_gain,
        .blue_channel_gain = source.blue_channel_gain,
        .saturation_factor = source.saturation_factor,
    };
}

[[nodiscard]] shadow::desktop::FfiBasicEditLayer ffi_layer(
    const BackendBasicEditLayer& source
) {
    shadow::desktop::FfiBasicEditLayer layer;
    layer.layer_id = source.layer_id.toStdString();
    layer.label = source.label.toStdString();
    layer.exposure_node_id = source.exposure_node_id.toStdString();
    layer.contrast_node_id = source.contrast_node_id.toStdString();
    layer.tone_curve_node_id = source.tone_curve_node_id.toStdString();
    layer.channel_gain_node_id = source.channel_gain_node_id.toStdString();
    layer.saturation_node_id = source.saturation_node_id.toStdString();
    layer.basic = ffi_parameters(source.basic);
    layer.enabled = source.enabled;
    layer.has_tone_curve = source.has_tone_curve;
    layer.tone_curve_points.reserve(
        static_cast<std::size_t>(source.tone_curve_points.size())
    );
    for (const auto& point : source.tone_curve_points) {
        layer.tone_curve_points.push_back({.x = point.x, .y = point.y});
    }
    return layer;
}

[[nodiscard]] BackendBasicEditLayer edit_layer(
    const shadow::desktop::FfiBasicEditLayer& source
) {
    BackendBasicEditLayer layer;
    layer.layer_id = qstring(source.layer_id);
    layer.label = qstring(source.label);
    layer.exposure_node_id = qstring(source.exposure_node_id);
    layer.contrast_node_id = qstring(source.contrast_node_id);
    layer.tone_curve_node_id = qstring(source.tone_curve_node_id);
    layer.channel_gain_node_id = qstring(source.channel_gain_node_id);
    layer.saturation_node_id = qstring(source.saturation_node_id);
    layer.basic = edit_parameters(source.basic);
    layer.enabled = source.enabled;
    layer.has_tone_curve = source.has_tone_curve;
    layer.tone_curve_points.reserve(
        checked_qt_vector_size(source.tone_curve_points.size(), "tone_curve_points")
    );
    for (const auto& point : source.tone_curve_points) {
        layer.tone_curve_points.push_back({.x = point.x, .y = point.y});
    }
    return layer;
}

[[nodiscard]] shadow::desktop::FfiEditSettings ffi_settings(
    const BackendEditSettings& source
) {
    shadow::desktop::FfiEditSettings settings;
    settings.layers.reserve(static_cast<std::size_t>(source.layers.size()));
    for (const auto& layer : source.layers) {
        settings.layers.push_back(ffi_layer(layer));
    }
    return settings;
}

[[nodiscard]] BackendEditSettings edit_settings(
    const shadow::desktop::FfiEditSettings& source
) {
    BackendEditSettings settings;
    settings.layers.reserve(checked_qt_vector_size(source.layers.size(), "layers"));
    for (const auto& layer : source.layers) {
        settings.layers.push_back(edit_layer(layer));
    }
    return settings;
}

[[nodiscard]] BackendPhotoEditState edit_state(
    const shadow::desktop::FfiPhotoEditState& source
) {
    BackendPhotoEditState state;
    state.photo_id = qstring(source.photo_id);
    state.source_path = qstring(source.source_path);
    state.working_commit_id = qstring(source.working_commit_id);
    state.recipe_id = qstring(source.recipe_id);
    state.settings = edit_settings(source.settings);
    state.has_working_version = source.has_working_version;
    state.versions.reserve(checked_qt_vector_size(source.versions.size(), "versions"));
    for (const auto& version : source.versions) {
        BackendEditVersion converted;
        converted.commit_id = qstring(version.commit_id);
        converted.name = qstring(version.name);
        converted.created_at_ms = version.created_at_ms;
        converted.is_working = version.is_working;
        converted.is_root = version.is_root;
        converted.recipe_schema_changed = version.recipe_schema_changed;
        converted.layers_added = version.layers_added;
        converted.layers_removed = version.layers_removed;
        converted.layers_moved = version.layers_moved;
        converted.layers_modified = version.layers_modified;
        converted.nodes_added = version.nodes_added;
        converted.nodes_removed = version.nodes_removed;
        converted.nodes_modified = version.nodes_modified;
        converted.node_parameter_blocks_changed = version.node_parameter_blocks_changed;
        converted.changed_basic_parameter_count = version.changed_basic_parameter_count;
        converted.has_other_changes = version.has_other_changes;
        converted.parent_commit_ids.reserve(
            checked_qt_vector_size(version.parent_commit_ids.size(), "parent_commit_ids")
        );
        for (const auto& parent : version.parent_commit_ids) {
            converted.parent_commit_ids.push_back(qstring(parent));
        }
        const auto changed_parameter_size = checked_qt_vector_size(
            version.changed_basic_parameters.size(),
            "changed_basic_parameters"
        );
        if (static_cast<std::uint64_t>(changed_parameter_size)
            != static_cast<std::uint64_t>(version.changed_basic_parameter_count)) {
            throw std::runtime_error(
                "desktop bridge returned an inconsistent changed-basic-parameter count"
            );
        }
        converted.changed_basic_parameters.reserve(changed_parameter_size);
        for (const auto& parameter : version.changed_basic_parameters) {
            converted.changed_basic_parameters.push_back(qstring(parameter));
        }
        state.versions.push_back(std::move(converted));
    }
    return state;
}

[[nodiscard]] shadow::desktop::FfiPairwiseOutcome ffi_outcome(
    const BackendPairwiseOutcome outcome
) {
    switch (outcome) {
    case BackendPairwiseOutcome::LeftPreferred:
        return shadow::desktop::FfiPairwiseOutcome::LeftPreferred;
    case BackendPairwiseOutcome::RightPreferred:
        return shadow::desktop::FfiPairwiseOutcome::RightPreferred;
    case BackendPairwiseOutcome::KeepBoth:
        return shadow::desktop::FfiPairwiseOutcome::KeepBoth;
    case BackendPairwiseOutcome::KeepNeither:
        return shadow::desktop::FfiPairwiseOutcome::KeepNeither;
    case BackendPairwiseOutcome::CannotCompare:
        return shadow::desktop::FfiPairwiseOutcome::CannotCompare;
    }
    throw std::invalid_argument("unknown pairwise outcome");
}

[[nodiscard]] BackendReviewDecisionFlag decision_flag(
    const shadow::desktop::FfiDecisionFlag flag
) {
    switch (flag) {
    case shadow::desktop::FfiDecisionFlag::Unflagged:
        return BackendReviewDecisionFlag::Unflagged;
    case shadow::desktop::FfiDecisionFlag::Picked:
        return BackendReviewDecisionFlag::Picked;
    case shadow::desktop::FfiDecisionFlag::Rejected:
        return BackendReviewDecisionFlag::Rejected;
    }
    throw std::invalid_argument("unknown Review decision flag");
}

[[nodiscard]] shadow::desktop::FfiDecisionFlag ffi_decision_flag(
    const BackendReviewDecisionFlag flag
) {
    switch (flag) {
    case BackendReviewDecisionFlag::Unflagged:
        return shadow::desktop::FfiDecisionFlag::Unflagged;
    case BackendReviewDecisionFlag::Picked:
        return shadow::desktop::FfiDecisionFlag::Picked;
    case BackendReviewDecisionFlag::Rejected:
        return shadow::desktop::FfiDecisionFlag::Rejected;
    }
    throw std::invalid_argument("unknown Review decision flag");
}

} // namespace

struct DesktopBackend::Impl final {
    explicit Impl(rust::Box<shadow::desktop::DesktopSession> value)
        : session(std::move(value)) {}

    rust::Box<shadow::desktop::DesktopSession> session;
};

DesktopBackend::DesktopBackend(const QString& catalog_path, const QString& cache_root)
    : impl_(std::make_unique<Impl>(shadow::desktop::open_desktop_session(
          catalog_path.toStdString(),
          cache_root.toStdString()
      ))) {}

DesktopBackend::~DesktopBackend() = default;

BackendScanReport DesktopBackend::scanFolder(const QString& folder_path) const {
    const auto source = impl_->session->scan_folder(folder_path.toStdString());
    return {
        .folder_path = qstring(source.folder_path),
        .files_seen = source.files_seen,
        .supported_files = source.supported_files,
        .decode_queued = source.decode_inspections_queued,
        .issue_count = source.issue_count,
    };
}

BackendReviewPage DesktopBackend::reviewPage(
    const QString& cursor_path,
    const QString& cursor_representation_id,
    const std::uint32_t limit
) const {
    const auto source = impl_->session->review_page(
        cursor_path.toStdString(),
        cursor_representation_id.toStdString(),
        limit
    );
    BackendReviewPage page;
    page.total_items = source.total_items;
    page.has_more = source.has_more;
    page.next_cursor_path = qstring(source.next_cursor_path);
    page.next_cursor_representation_id = qstring(source.next_cursor_representation_id);
    page.items.reserve(static_cast<qsizetype>(source.items.size()));
    for (const auto& item : source.items) {
        page.items.push_back({
            .photo_id = qstring(item.photo_id),
            .representation_id = qstring(item.representation_id),
            .visual_handle = qstring(item.visual_handle),
            .decision_head_sequence = item.decision_head_sequence,
            .decision_flag = decision_flag(item.decision_flag),
            .decision_rating = item.decision_rating,
            .title = qstring(item.title),
            .source_path = qstring(item.source_path),
            .visual_role = qstring(item.visual_role),
            .visual_width = item.visual_width,
            .visual_height = item.visual_height,
            .has_visual = item.has_visual,
            .has_technical_observation = item.has_technical_observation,
            .technical_input_width = item.technical_input_width,
            .technical_input_height = item.technical_input_height,
            .technical_preprocessing_version = qstring(
                item.technical_preprocessing_version
            ),
            .technical_implementation_version = qstring(
                item.technical_implementation_version
            ),
            .mean_luma = item.mean_luma,
            .p01_luma = item.p01_luma,
            .p50_luma = item.p50_luma,
            .p99_luma = item.p99_luma,
            .near_black_fraction = item.near_black_fraction,
            .near_white_fraction = item.near_white_fraction,
            .laplacian_variance = item.laplacian_variance,
            .edge_energy = item.edge_energy,
        });
    }
    return page;
}

BackendReviewVisual DesktopBackend::loadReviewVisual(const QString& ticket) const {
    const auto payload = impl_->session->load_review_visual(ticket.toStdString());
    return {
        .bytes = qbytes(payload.bytes),
        .requires_frame_receipt = payload.requires_frame_receipt,
    };
}

BackendReviewComparisonPresentation DesktopBackend::prepareReviewComparison(
    const QString& left_visual_handle,
    const QString& right_visual_handle
) const {
    const auto presentation = impl_->session->prepare_review_comparison(
        left_visual_handle.toStdString(),
        right_visual_handle.toStdString()
    );
    return {
        .presentation_id = qstring(presentation.presentation_id),
        .left_request_ticket = qstring(presentation.left_request_ticket),
        .right_request_ticket = qstring(presentation.right_request_ticket),
    };
}

void DesktopBackend::reportReviewVisualFrame(
    const QString& ticket,
    const QString& decoder_version,
    const std::uint32_t requested_width,
    const std::uint32_t requested_height,
    const std::uint32_t decoded_width,
    const std::uint32_t decoded_height,
    const QString& pixel_hash_hex
) const {
    impl_->session->record_review_visual_frame(
        ticket.toStdString(),
        decoder_version.toStdString(),
        requested_width,
        requested_height,
        decoded_width,
        decoded_height,
        pixel_hash_hex.toStdString()
    );
}

void DesktopBackend::confirmReviewComparisonReady(
    const QString& presentation_id,
    const QString& left_request_ticket,
    const QString& right_request_ticket
) const {
    impl_->session->confirm_review_comparison_ready(
        presentation_id.toStdString(),
        left_request_ticket.toStdString(),
        right_request_ticket.toStdString()
    );
}

void DesktopBackend::cancelReviewComparison(const QString& presentation_id) const {
    impl_->session->cancel_review_comparison(presentation_id.toStdString());
}

BackendFeedbackReceipt DesktopBackend::recordReviewComparison(
    const QString& presentation_id,
    const BackendPairwiseOutcome outcome
) const {
    const auto receipt = impl_->session->record_review_comparison(
        presentation_id.toStdString(),
        ffi_outcome(outcome)
    );
    return {
        .event_id = qstring(receipt.event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
    };
}

BackendForgetReceipt DesktopBackend::forgetReviewFeedback(
    const QString& event_id
) const {
    const auto receipt = impl_->session->forget_review_feedback(event_id.toStdString());
    return {
        .fact_id = qstring(receipt.fact_id),
        .target_event_id = qstring(receipt.target_event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
    };
}

BackendReviewDecisionState DesktopBackend::reviewPhotoDecisionState(
    const QString& photo_id
) const {
    const auto state = impl_->session->review_photo_decision_state(photo_id.toStdString());
    return {
        .photo_id = qstring(state.photo_id),
        .head_sequence = state.head_sequence,
        .flag = decision_flag(state.flag),
        .rating = state.rating,
    };
}

BackendReviewDecisionMutationReceipt DesktopBackend::setReviewPhotoDecision(
    const QString& photo_id,
    const std::uint64_t expected_head_sequence,
    const BackendReviewDecisionFlag desired_flag,
    const std::uint8_t desired_rating
) const {
    const auto receipt = impl_->session->set_review_photo_decision(
        photo_id.toStdString(),
        expected_head_sequence,
        ffi_decision_flag(desired_flag),
        desired_rating
    );
    const QString returned_photo_id = qstring(receipt.photo_id);
    return {
        .event_id = qstring(receipt.event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
        .before = {
            .photo_id = returned_photo_id,
            .head_sequence = receipt.before_head_sequence,
            .flag = decision_flag(receipt.before_flag),
            .rating = receipt.before_rating,
        },
        .after = {
            .photo_id = returned_photo_id,
            .head_sequence = receipt.sequence,
            .flag = decision_flag(receipt.after_flag),
            .rating = receipt.after_rating,
        },
    };
}

BackendPhotoEditState DesktopBackend::photoEditState(
    const QString& photo_id,
    const QString& source_path
) const {
    return edit_state(impl_->session->photo_edit_state(
        photo_id.toStdString(),
        source_path.toStdString()
    ));
}

BackendBasicEditLayer DesktopBackend::newBasicEditLayer(const QString& label) const {
    return edit_layer(shadow::desktop::new_basic_edit_layer(label.toStdString()));
}

BackendEditedPreview DesktopBackend::renderEditPreview(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendEditSettings& settings,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality,
    const bool use_working_recipe
) const {
    shadow::desktop::FfiEditPreviewRequest request;
    request.base_commit_id = base_commit_id.toStdString();
    request.settings = ffi_settings(settings);
    request.max_edge = max_edge;
    request.jpeg_quality = jpeg_quality;
    request.use_working_recipe = use_working_recipe;
    const auto payload = impl_->session->render_basic_edit_preview(
        photo_id.toStdString(),
        source_path.toStdString(),
        request
    );
    return {
        .bytes = qbytes(payload.bytes),
        .width = payload.width,
        .height = payload.height,
    };
}

BackendPhotoEditState DesktopBackend::saveEditVersion(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendEditSettings& settings,
    const QString& version_name
) const {
    const auto ffi = ffi_settings(settings);
    return edit_state(impl_->session->save_basic_edit_version(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        ffi,
        version_name.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::checkoutEditVersion(
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
