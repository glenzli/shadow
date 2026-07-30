#include <shadow/image/cxx_bridge.hpp>

#include "adjustment_render_wire.hpp"
#include "cxx_bridge_projection.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/sensor_clipping.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace shadow::bridge {

using namespace cxx_bridge_projection;

namespace {

[[nodiscard]] std::optional<std::uint32_t>
mask_coverage_target(const FfiAdjustmentRenderRequest& request) {
    if (!request.mask_coverage_requested) {
        if (request.mask_coverage_target_layer_index != 0U) {
            throw image::DecodeError(
                image::DecodeErrorCode::invalid_request,
                0,
                "unrequested edit-preview mask coverage must use the zero target sentinel"
            );
        }
        return std::nullopt;
    }
    return request.mask_coverage_target_layer_index;
}

void reject_mask_coverage_target(const FfiAdjustmentRenderRequest& request) {
    if (mask_coverage_target(request).has_value()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "this edit-preview entry point does not return paired mask coverage"
        );
    }
}

[[nodiscard]] image::CancellableEditPreviewResult<image::EditPreviewRgb8WithMaskCoverage>
render_adjustment_plan_rgb8_frame(
    const image::WarmEditPreviewSession& session,
    const FfiAdjustmentRenderRequest& request,
    const EditPreviewCancellationHandle& cancellation
) {
    if (request.max_edge != session.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto layers = adjustment_render_wire::adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    const auto liquify = adjustment_render_wire::photo_liquify(request.liquify);
    const auto target = mask_coverage_target(request);
    if (target.has_value() && !layers.has_value()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "mask coverage target requires an explicit adjustment-layer plan"
        );
    }
    if (layers.has_value()) {
        return session.render_rgb8_layers_with_mask_coverage_cancellable(
            *layers,
            target,
            cancellation.token(),
            geometry,
            liquify.has_value() ? &*liquify : nullptr
        );
    }

    auto rendered = session.render_rgb8_cancellable(
        adjustment_render_wire::adjustment_nodes(request.nodes),
        cancellation.token(),
        geometry,
        liquify.has_value() ? &*liquify : nullptr
    );
    if (rendered.cancelled()) {
        return {};
    }
    return {
        .completed = image::EditPreviewRgb8WithMaskCoverage{
            .preview = std::move(*rendered.completed),
            .mask_coverage = std::nullopt,
        },
    };
}

[[nodiscard]] image::CancellableEditPreviewResult<image::InteractiveEditPreviewFrame>
render_adjustment_plan_interactive_frame(
    const image::WarmEditPreviewSession& session,
    const FfiAdjustmentRenderRequest& request,
    const EditPreviewCancellationHandle& cancellation
) {
    if (request.max_edge != session.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto layers = adjustment_render_wire::adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    const auto liquify = adjustment_render_wire::photo_liquify(request.liquify);
    const auto target = mask_coverage_target(request);
    if (target.has_value() && !layers.has_value()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "mask coverage target requires an explicit adjustment-layer plan"
        );
    }
    if (layers.has_value()) {
        return session.render_interactive_frame_layers_with_mask_coverage_cancellable(
            *layers,
            target,
            cancellation.token(),
            geometry,
            liquify.has_value() ? &*liquify : nullptr
        );
    }
    return session.render_interactive_frame_cancellable(
        adjustment_render_wire::adjustment_nodes(request.nodes),
        cancellation.token(),
        geometry,
        liquify.has_value() ? &*liquify : nullptr
    );
}

} // namespace

DecodeHandle::DecodeHandle(
    std::unique_ptr<image::DecoderProvider> provider,
    std::unique_ptr<image::DecodeSession> session,
    std::shared_ptr<const image::OpticsProvider> optics_provider
) :
    provider_(std::move(provider)), session_(std::move(session)),
    optics_provider_(std::move(optics_provider)) {}

DecodeHandle::~DecodeHandle() = default;

void DecodeHandle::configure_optics(const FfiOpticsSettings& settings) {
    image::OpticsSettings configured{
        settings.schema_version,
        settings.enabled,
        settings.correct_distortion,
        settings.correct_tca,
        settings.correct_vignetting,
        settings.automatic_scale,
        settings.manual_distortion,
        settings.manual_tca_red_cyan,
        settings.manual_tca_blue_yellow,
        settings.manual_vignetting_amount,
        settings.manual_vignetting_midpoint,
        std::string(settings.camera_profile_maker),
        std::string(settings.camera_profile_model),
        std::string(settings.lens_profile_maker),
        std::string(settings.lens_profile_model),
    };
    // The signature function is the authoritative schema/combination validator
    // shared with cache identity construction.
    (void)image::optics_settings_signature(configured);
    optics_settings_ = configured;
}

FfiProviderSnapshot DecodeHandle::provider() const {
    const auto& info = provider_->info();
    FfiProviderSnapshot snapshot;
    snapshot.id = rust::String(info.id);
    snapshot.version = rust::String(info.version);
    snapshot.dng_sdk = info.dng_sdk;
    snapshot.rawspeed = info.rawspeed;
    snapshot.jpeg = info.jpeg;
    return snapshot;
}

FfiMetadataSnapshot DecodeHandle::metadata() const {
    const auto& metadata = session_->metadata();
    FfiMetadataSnapshot snapshot;
    snapshot.make = rust::String(metadata.make);
    snapshot.model = rust::String(metadata.model);
    snapshot.normalized_make = rust::String(metadata.normalized_make);
    snapshot.normalized_model = rust::String(metadata.normalized_model);
    snapshot.dng_version = rust::String(metadata.dng_version);
    snapshot.raw_count = metadata.raw_count;
    snapshot.raw_dimensions = dimensions(metadata.raw_dimensions);
    snapshot.image_dimensions = dimensions(metadata.image_dimensions);
    snapshot.margins = FfiMargins{
        metadata.margins.left,
        metadata.margins.top,
        metadata.margins.right,
        metadata.margins.bottom,
    };
    snapshot.orientation = metadata.orientation;
    snapshot.cfa_pattern = rust::String(metadata.cfa_pattern);
    snapshot.sensor_colors = metadata.sensor_colors;
    snapshot.sensor_bits = metadata.sensor_bits;
    snapshot.black_level = metadata.black_level;
    snapshot.white_level = metadata.white_level;
    snapshot.as_shot_neutral_r = metadata.as_shot_neutral[0];
    snapshot.as_shot_neutral_g1 = metadata.as_shot_neutral[1];
    snapshot.as_shot_neutral_b = metadata.as_shot_neutral[2];
    snapshot.as_shot_neutral_g2 = metadata.as_shot_neutral[3];
    snapshot.baseline_exposure = metadata.baseline_exposure;
    snapshot.iso_speed = metadata.iso_speed;
    snapshot.exposure_time_seconds = metadata.exposure_time_seconds;
    snapshot.aperture_f_number = metadata.aperture_f_number;
    snapshot.focal_length_mm = metadata.focal_length_mm;
    snapshot.captured_at_unix_seconds = metadata.captured_at_unix_seconds;
    snapshot.lens_make = rust::String(metadata.lens_make);
    snapshot.lens_model = rust::String(metadata.lens_model);
    snapshot.focal_length_35mm = metadata.focal_length_35mm;
    return snapshot;
}

FfiCapabilitySnapshot DecodeHandle::capabilities() const {
    const auto& capabilities = session_->capabilities();
    const auto& opcode_bytes = capabilities.pending_corrections.dng_opcode_list_bytes;
    FfiCapabilitySnapshot snapshot;
    snapshot.metadata = capabilities.metadata;
    snapshot.embedded_previews = capabilities.embedded_previews;
    snapshot.raw_frame = capabilities.raw_frame;
    snapshot.reference_rgb = capabilities.reference_rgb;
    snapshot.dng_opcode_list_1_bytes = opcode_bytes[0];
    snapshot.dng_opcode_list_2_bytes = opcode_bytes[1];
    snapshot.dng_opcode_list_3_bytes = opcode_bytes[2];
    snapshot.raw_development = cxx_bridge_projection::raw_development_capabilities(
        capabilities.raw_frame ? image::shadow_raw_frame_development_capabilities()
                               : capabilities.raw_development
    );
    return snapshot;
}

FfiRawDevelopmentCapabilities DecodeHandle::raw_development_capabilities() const {
    return cxx_bridge_projection::raw_development_capabilities(
        session_->capabilities().raw_frame ? image::shadow_raw_frame_development_capabilities()
                                           : session_->raw_development_capabilities()
    );
}

FfiRawDevelopmentPlanNegotiation
DecodeHandle::negotiate_raw_development_plan(const FfiRawDevelopmentPlan& plan) const {
    return raw_development_plan_negotiation(
        session_->capabilities().raw_frame
            ? image::negotiate_shadow_raw_frame_development_plan(raw_development_plan(plan))
            : session_->negotiate_raw_development_plan(raw_development_plan(plan))
    );
}

FfiRawDevelopmentReceipt DecodeHandle::raw_development_receipt() const {
    return cxx_bridge_projection::raw_development_receipt(raw_development_receipt_);
}

FfiRawPipelineReceipt DecodeHandle::raw_pipeline_receipt() const {
    return cxx_bridge_projection::raw_pipeline_receipt(raw_pipeline_receipt_);
}

rust::Vec<FfiPreviewSnapshot> DecodeHandle::previews() const {
    rust::Vec<FfiPreviewSnapshot> snapshots;
    snapshots.reserve(session_->previews().size());
    for (const auto& preview : session_->previews()) {
        snapshots.push_back(preview_snapshot(preview));
    }
    return snapshots;
}

FfiPreviewPayload DecodeHandle::decode_best_preview() {
    const auto selected = image::select_best_preview(session_->previews());
    FfiPreviewPayload result;
    if (!selected.has_value()) {
        result.present = false;
        return result;
    }

    auto payload = session_->decode_preview(*selected);
    result.present = true;
    result.descriptor = preview_snapshot(payload.descriptor);
    result.byte_order = byte_order(payload.byte_order);
    result.bytes.reserve(payload.bytes.size());
    for (const auto byte : payload.bytes) {
        result.bytes.push_back(byte);
    }
    return result;
}

FfiEncodedProxy DecodeHandle::render_reference_proxy(
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality
) const {
    const auto proxy =
        image::render_reference_proxy_jpeg(*session_, image::ProxyRequest{max_edge, jpeg_quality});
    return encoded_proxy(proxy);
}

FfiEncodedProxy
DecodeHandle::render_adjustment_plan(const FfiAdjustmentRenderRequest& request) const {
    reject_mask_coverage_target(request);
    const auto layers = adjustment_render_wire::adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    const auto liquify = adjustment_render_wire::photo_liquify(request.liquify);
    const auto preview = image::prepare_warm_edit_preview(
        *session_,
        request.max_edge,
        optics_provider_.get(),
        optics_settings_
    );
    const auto proxy = layers.has_value()
                           ? preview.render_jpeg_layers(
                                 *layers,
                                 request.jpeg_quality,
                                 geometry,
                                 liquify.has_value() ? &*liquify : nullptr
                             )
                           : preview.render_jpeg(
                                 adjustment_render_wire::adjustment_nodes(request.nodes),
                                 request.jpeg_quality,
                                 geometry,
                                 liquify.has_value() ? &*liquify : nullptr
                             );
    return encoded_proxy(proxy);
}

std::unique_ptr<EditPreviewHandle>
DecodeHandle::prepare_edit_preview(const std::uint32_t max_edge) const {
    auto prepared = image::prepare_warm_edit_preview(
        *session_,
        max_edge,
        optics_provider_.get(),
        optics_settings_
    );
    raw_development_receipt_ = prepared.raw_development_receipt();
    raw_pipeline_receipt_ = prepared.raw_pipeline_receipt();
    return std::make_unique<EditPreviewHandle>(std::move(prepared));
}

std::unique_ptr<EditPreviewHandle> DecodeHandle::prepare_edit_preview_with_raw_development_plan(
    const std::uint32_t max_edge,
    const FfiRawDevelopmentPlan& plan
) const {
    auto prepared = image::prepare_warm_edit_preview(
        *session_,
        max_edge,
        raw_development_plan(plan),
        optics_provider_.get(),
        optics_settings_
    );
    raw_development_receipt_ = prepared.raw_development_receipt();
    raw_pipeline_receipt_ = prepared.raw_pipeline_receipt();
    return std::make_unique<EditPreviewHandle>(std::move(prepared));
}

EditPreviewHandle::EditPreviewHandle(image::WarmEditPreviewSession session) :
    session_(std::move(session)) {}

EditPreviewHandle::~EditPreviewHandle() = default;

FfiDimensions EditPreviewHandle::dimensions() const noexcept {
    return cxx_bridge_projection::dimensions(session_.dimensions());
}

std::uint32_t EditPreviewHandle::max_edge() const noexcept {
    return session_.max_edge();
}

FfiOpticsReceipt EditPreviewHandle::optics_receipt() const {
    return cxx_bridge_projection::optics_receipt(session_.optics_receipt());
}

FfiRawDevelopmentReceipt EditPreviewHandle::raw_development_receipt() const {
    return cxx_bridge_projection::raw_development_receipt(session_.raw_development_receipt());
}

FfiRawPipelineReceipt EditPreviewHandle::raw_pipeline_receipt() const {
    return cxx_bridge_projection::raw_pipeline_receipt(session_.raw_pipeline_receipt());
}

FfiSensorClippingMask EditPreviewHandle::sensor_clipping_mask() const {
    const auto& mask = session_.sensor_clipping_mask();
    return mask.has_value() ? cxx_bridge_projection::sensor_clipping_mask(*mask)
                            : FfiSensorClippingMask{};
}

bool EditPreviewCancellationHandle::cancel() const noexcept {
    return source_.request_stop();
}

std::stop_token EditPreviewCancellationHandle::token() const noexcept {
    return source_.get_token();
}

std::shared_ptr<EditPreviewCancellationHandle> new_edit_preview_cancellation() {
    return std::make_shared<EditPreviewCancellationHandle>();
}

FfiEncodedProxy
EditPreviewHandle::render_adjustment_plan(const FfiAdjustmentRenderRequest& request) const {
    reject_mask_coverage_target(request);
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto layers = adjustment_render_wire::adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    const auto liquify = adjustment_render_wire::photo_liquify(request.liquify);
    return encoded_proxy(
        layers.has_value() ? session_.render_jpeg_layers(
                                 *layers,
                                 request.jpeg_quality,
                                 geometry,
                                 liquify.has_value() ? &*liquify : nullptr
                             )
                           : session_.render_jpeg(
                                 adjustment_render_wire::adjustment_nodes(request.nodes),
                                 request.jpeg_quality,
                                 geometry,
                                 liquify.has_value() ? &*liquify : nullptr
                             )
    );
}

FfiAnalyzedEditPreview EditPreviewHandle::render_adjustment_plan_with_analysis(
    const FfiAdjustmentRenderRequest& request
) const {
    reject_mask_coverage_target(request);
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto layers = adjustment_render_wire::adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    const auto liquify = adjustment_render_wire::photo_liquify(request.liquify);
    return analyzed_edit_preview(
        layers.has_value() ? session_.render_jpeg_with_analysis_layers(
                                 *layers,
                                 request.jpeg_quality,
                                 geometry,
                                 liquify.has_value() ? &*liquify : nullptr
                             )
                           : session_.render_jpeg_with_analysis(
                                 adjustment_render_wire::adjustment_nodes(request.nodes),
                                 request.jpeg_quality,
                                 geometry,
                                 liquify.has_value() ? &*liquify : nullptr
                             )
    );
}

FfiCancellableEncodedProxy EditPreviewHandle::render_adjustment_plan_cancellable(
    const FfiAdjustmentRenderRequest& request,
    const EditPreviewCancellationHandle& cancellation
) const {
    reject_mask_coverage_target(request);
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto layers = adjustment_render_wire::adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    const auto liquify = adjustment_render_wire::photo_liquify(request.liquify);
    if (layers.has_value()) {
        if (cancellation.token().stop_requested()) {
            return FfiCancellableEncodedProxy{
                .cancelled = true,
                .proxy = {},
                .mask_coverage = {},
            };
        }
        auto rendered = session_.render_jpeg_layers(
            *layers,
            request.jpeg_quality,
            geometry,
            liquify.has_value() ? &*liquify : nullptr
        );
        return FfiCancellableEncodedProxy{
            .cancelled = cancellation.token().stop_requested(),
            .proxy =
                cancellation.token().stop_requested() ? FfiEncodedProxy{} : encoded_proxy(rendered),
            .mask_coverage = {},
        };
    }
    const auto nodes = adjustment_render_wire::adjustment_nodes(request.nodes);
    auto rendered = session_.render_jpeg_cancellable(
        nodes,
        request.jpeg_quality,
        cancellation.token(),
        geometry,
        liquify.has_value() ? &*liquify : nullptr
    );
    if (rendered.cancelled()) {
        return FfiCancellableEncodedProxy{
            .cancelled = true,
            .proxy = {},
            .mask_coverage = {},
        };
    }
    return FfiCancellableEncodedProxy{
        .cancelled = false,
        .proxy = encoded_proxy(*rendered.completed),
        .mask_coverage = {},
    };
}

FfiCancellableEncodedProxy EditPreviewHandle::render_adjustment_plan_rgb8_cancellable(
    const FfiAdjustmentRenderRequest& request,
    const EditPreviewCancellationHandle& cancellation
) const {
    const auto rendered = render_adjustment_plan_rgb8_frame(session_, request, cancellation);
    if (rendered.cancelled()) {
        return FfiCancellableEncodedProxy{
            .cancelled = true,
            .proxy = {},
            .mask_coverage = {},
        };
    }
    return FfiCancellableEncodedProxy{
        .cancelled = false,
        .proxy = encoded_proxy(rendered.completed->preview),
        .mask_coverage = edit_preview_mask_coverage(rendered.completed->mask_coverage),
    };
}

std::unique_ptr<InteractiveEditPreviewFrameHandle>
EditPreviewHandle::render_adjustment_plan_owned_rgb8_cancellable(
    const FfiAdjustmentRenderRequest& request,
    const EditPreviewCancellationHandle& cancellation
) const {
    auto rendered = render_adjustment_plan_interactive_frame(session_, request, cancellation);
    if (rendered.cancelled()) {
        return {};
    }
    return std::make_unique<InteractiveEditPreviewFrameHandle>(std::move(*rendered.completed));
}

FfiCancellableAnalyzedEditPreview
EditPreviewHandle::render_adjustment_plan_with_analysis_cancellable(
    const FfiAdjustmentRenderRequest& request,
    const EditPreviewCancellationHandle& cancellation
) const {
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto layers = adjustment_render_wire::adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    const auto liquify = adjustment_render_wire::photo_liquify(request.liquify);
    const auto target = mask_coverage_target(request);
    if (target.has_value() && !layers.has_value()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "mask coverage target requires an explicit adjustment-layer plan"
        );
    }
    if (layers.has_value()) {
        auto rendered = session_.render_jpeg_with_analysis_layers_and_mask_coverage_cancellable(
            *layers,
            target,
            request.jpeg_quality,
            cancellation.token(),
            geometry,
            liquify.has_value() ? &*liquify : nullptr
        );
        if (rendered.cancelled()) {
            return FfiCancellableAnalyzedEditPreview{
                .cancelled = true,
                .preview = {},
                .mask_coverage = {},
            };
        }
        return FfiCancellableAnalyzedEditPreview{
            .cancelled = false,
            .preview = analyzed_edit_preview(rendered.completed->preview),
            .mask_coverage = edit_preview_mask_coverage(rendered.completed->mask_coverage),
        };
    }
    const auto nodes = adjustment_render_wire::adjustment_nodes(request.nodes);
    auto rendered = session_.render_jpeg_with_analysis_cancellable(
        nodes,
        request.jpeg_quality,
        cancellation.token(),
        geometry,
        liquify.has_value() ? &*liquify : nullptr
    );
    if (rendered.cancelled()) {
        return FfiCancellableAnalyzedEditPreview{
            .cancelled = true,
            .preview = {},
            .mask_coverage = {},
        };
    }
    return FfiCancellableAnalyzedEditPreview{
        .cancelled = false,
        .preview = analyzed_edit_preview(*rendered.completed),
        .mask_coverage = {},
    };
}

std::unique_ptr<FullEditDetailHandle> DecodeHandle::prepare_edit_detail() const {
    auto prepared =
        image::prepare_full_edit_detail(*session_, optics_provider_.get(), optics_settings_);
    raw_development_receipt_ = prepared.raw_development_receipt();
    raw_pipeline_receipt_ = prepared.raw_pipeline_receipt();
    return std::make_unique<FullEditDetailHandle>(std::move(prepared));
}

std::unique_ptr<FullEditDetailHandle> DecodeHandle::prepare_edit_detail_with_raw_development_plan(
    const FfiRawDevelopmentPlan& plan,
    const FfiDetailSessionRequirements& requirements
) const {
    auto prepared = image::prepare_full_edit_detail(
        *session_,
        raw_development_plan(plan),
        image::FullEditDetailSourceRequirements{
            .requires_cpu_replay = requirements.requires_cpu_replay,
        },
        optics_provider_.get(),
        optics_settings_
    );
    raw_development_receipt_ = prepared.raw_development_receipt();
    raw_pipeline_receipt_ = prepared.raw_pipeline_receipt();
    return std::make_unique<FullEditDetailHandle>(std::move(prepared));
}

FullEditDetailHandle::FullEditDetailHandle(image::FullEditDetailSession session) :
    session_(std::move(session)) {}

FullEditDetailHandle::~FullEditDetailHandle() = default;

FfiDimensions FullEditDetailHandle::dimensions() const noexcept {
    return cxx_bridge_projection::dimensions(session_.dimensions());
}

std::uint64_t FullEditDetailHandle::retained_bytes() const noexcept {
    return session_.retained_bytes();
}

bool FullEditDetailHandle::cpu_replay_available() const noexcept {
    return session_.cpu_replay_available();
}

FfiOpticsReceipt FullEditDetailHandle::optics_receipt() const {
    return cxx_bridge_projection::optics_receipt(session_.optics_receipt());
}

FfiRawDevelopmentReceipt FullEditDetailHandle::raw_development_receipt() const {
    return cxx_bridge_projection::raw_development_receipt(session_.raw_development_receipt());
}

FfiRawPipelineReceipt FullEditDetailHandle::raw_pipeline_receipt() const {
    return cxx_bridge_projection::raw_pipeline_receipt(session_.raw_pipeline_receipt());
}

FfiRenderedDetailTile FullEditDetailHandle::render_adjustment_plan_tile(
    const FfiAdjustmentDetailTileRequest& request
) const {
    const auto layers = adjustment_render_wire::adjustment_layers(request.nodes);
    const auto geometry = photo_geometry(request.geometry);
    const auto liquify = adjustment_render_wire::photo_liquify(request.liquify);
    return rendered_detail_tile(
        layers.has_value() ? session_.render_rgb8_layers(
                                 *layers,
                                 detail_tile_rect(request.rect),
                                 geometry,
                                 liquify.has_value() ? &*liquify : nullptr
                             )
                           : session_.render_rgb8(
                                 adjustment_render_wire::adjustment_nodes(request.nodes),
                                 detail_tile_rect(request.rect),
                                 geometry,
                                 liquify.has_value() ? &*liquify : nullptr
                             )
    );
}

} // namespace shadow::bridge
