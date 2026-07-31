#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/decoder_metadata.hpp>
#include <shadow/image/decoder_session.hpp>
#include <shadow/image/display_output.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/optics.hpp>
#include <shadow/image/photo_geometry.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/reference_pixels.hpp>
#include <shadow/image/source_rendering.hpp>
#include <shadow/image/warm_edit_preview.hpp>
#include <shadow/image/working_rgb.hpp>

#include "../raw/raw_preview_rebinding.hpp"
#include "developed_source_raster.hpp"
#include "edit_preview_rendering.hpp"
#include "jpeg_proxy_encoding.hpp"
#include "proxy_render_request_validation.hpp"
#include "warm_edit_gpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace shadow::image {

namespace {

using edit_preview_detail::analyze_edit_preview;
using edit_preview_detail::prepare_edit_preview_layer_pixels;
using edit_preview_detail::prepare_edit_preview_pixels;
using edit_preview_detail::PreparedEditPreviewPixels;

[[nodiscard]] EncodedProxy rgb8_proxy(PreparedEditPreviewPixels prepared) {
    const std::uint64_t expected_bytes = prepared.dimensions.pixel_count() * 3U;
    if (prepared.dimensions.width == 0U || prepared.dimensions.height == 0U
        || expected_bytes != prepared.rgb.size()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "warm edit preview produced an invalid tightly packed RGB8 layout"
        );
    }
    return EncodedProxy{
        .dimensions = prepared.dimensions,
        .format = PreviewFormat::bitmap,
        .bits_per_channel = 8U,
        .channels = 3U,
        .bytes = std::move(prepared.rgb),
    };
}

void validate_warm_edit_max_edge(const std::uint32_t max_edge) {
    if (max_edge == 0U || max_edge > maximum_warm_edit_preview_edge) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "warm edit preview max edge must be in 1..=4096"
        );
    }
}

// Packed-provider RGB optics stays on its original u16 path. For an interactive preview, reduce
// the source before entering the provider and round-trip only the bounded proxy through that API.
// This avoids a 45 MP geometry remap just to display 1200 px, while full-detail sessions still
// run on the complete native reference. JPEG/HEIF remain excluded so they cannot be silently
// double-corrected.
[[nodiscard]] PixelBuffer working_to_linear_reference(const FloatRgbImage& source) {
    if (source.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || source.transfer_function != TransferFunction::linear
        || (source.reference != ImageReference::scene_referred
            && source.reference != ImageReference::display_referred)
        || source.dimensions.width == 0U || source.dimensions.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "preview optics conversion requires standardized linear interleaved RGB float pixels"
        );
    }
    const std::size_t expected_samples =
        proxy_detail::checked_interleaved_rgb_sample_count(source.dimensions);
    if (source.samples.size() != expected_samples) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "preview optics conversion found an invalid float RGB layout"
        );
    }
    PixelBuffer output;
    output.dimensions = source.dimensions;
    output.bits_per_channel = 16U;
    output.channels = 3U;
    output.row_stride_bytes = source.dimensions.width * 3U * sizeof(std::uint16_t);
    output.primaries = RgbPrimaries::srgb_rec709_d65;
    output.transfer_function = RgbTransferFunction::linear;
    output.reference = source.reference == ImageReference::scene_referred
                           ? RgbBufferReference::processed_raw
                           : RgbBufferReference::decoded_raster;
    output.samples.resize(expected_samples);
    for (std::size_t index = 0U; index < expected_samples; ++index) {
        output.samples[index] = static_cast<std::uint16_t>(std::clamp(
            std::llround(
                std::clamp(static_cast<double>(source.samples[index]), 0.0, 1.0) * 65'535.0
            ),
            0LL,
            65'535LL
        ));
    }
    return output;
}

// Shadow-owned RawFrame development never crosses the packed provider-RGB boundary. This tiny
// adapter intentionally preserves every finite scene-linear float, including values above one
// and small negative gamut components, for providers that explicitly advertise float support.
[[nodiscard]] SceneLinearRgbFrame working_to_scene_linear_reference(const FloatRgbImage& source) {
    if (source.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || source.transfer_function != TransferFunction::linear
        || source.reference != ImageReference::scene_referred || source.dimensions.width == 0U
        || source.dimensions.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "scene-linear preview optics requires standardized scene-referred RGB float pixels"
        );
    }
    const std::size_t expected_samples =
        proxy_detail::checked_interleaved_rgb_sample_count(source.dimensions);
    const std::size_t expected_stride =
        static_cast<std::size_t>(source.dimensions.width) * 3U * sizeof(float);
    if (source.row_stride_bytes != expected_stride || source.samples.size() != expected_samples) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "scene-linear preview optics found an invalid float RGB layout"
        );
    }
    SceneLinearRgbFrame output{
        .dimensions = source.dimensions,
        .row_stride_bytes = expected_stride,
        .samples = source.samples,
    };
    if (!output.valid()) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "scene-linear preview optics found non-finite float RGB samples"
        );
    }
    return output;
}

struct PreparedWarmEditProxy final {
    FloatRgbImage working_proxy;
    RawDevelopmentReceipt raw_development_receipt;
    RawPipelineReceipt raw_pipeline_receipt;
    OpticsProfileReceipt optics_receipt;
    std::optional<SensorClippingMask> sensor_clipping_mask;
    std::shared_ptr<const raw_pipeline_detail::RawPreviewRebindingSource> raw_rebinding_source;
};

// LibRaw's `sizes.flip` describes the output raster orientation. Its processed RGB is already
// rotated/flipped into that coordinate system, so a 90-degree orientation must also swap the
// level-zero dimensions used to translate native-pixel radii into a warm preview. Treat unknown
// values conservatively as unrotated metadata; the known dcraw/LibRaw 5/6 values are the two
// transposed cases seen in current providers.
[[nodiscard]] Dimensions oriented_full_dimensions(const AssetMetadata& metadata) noexcept {
    Dimensions dimensions = metadata.image_dimensions;
    if (metadata.orientation == 5 || metadata.orientation == 6) {
        std::swap(dimensions.width, dimensions.height);
    }
    return dimensions;
}

[[nodiscard]] PreparedWarmEditProxy finish_warm_edit_proxy(
    const AssetMetadata& metadata,
    const std::uint32_t max_edge,
    DevelopedSourceReference developed,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    DevelopedSourcePixels preview_reference = std::move(developed.source);
    proxy_detail::validate_developed_source(preview_reference);
    // The float working proxy intentionally contains only pixels and scale metadata. Retain the
    // decoder's receipt separately before the RGB conversion so a prepared session can report
    // the exact RAW-development request that created its source raster.
    RawDevelopmentReceipt raw_development_receipt = std::move(developed.raw_development_receipt);
    std::optional<SensorClippingMask> sensor_clipping_mask =
        std::move(developed.sensor_clipping_mask);
    const SourceRenderingReceipt source_rendering = std::visit(
        [&](const auto& value) {
            return resolve_source_rendering(value, metadata, developed.pipeline_receipt);
        },
        preview_reference
    );
    const Dimensions target =
        proxy_dimensions(proxy_detail::developed_source_dimensions(preview_reference), max_edge);
    FloatRgbImage working_proxy =
        proxy_detail::resize_developed_source_to_working(preview_reference, target);
    // LibRaw may use a half-size demosaic above. Detail-and-effects radii remain expressed in
    // native level-zero pixels, so preserve the relationship to the *oriented* full output
    // dimensions rather than accidentally doubling one axis for a rotated camera frame.
    const Dimensions full_dimensions = oriented_full_dimensions(metadata);
    if (full_dimensions.width > 0U && full_dimensions.height > 0U) {
        working_proxy.level_zero_to_raster_scale_x =
            static_cast<double>(target.width) / static_cast<double>(full_dimensions.width);
        working_proxy.level_zero_to_raster_scale_y =
            static_cast<double>(target.height) / static_cast<double>(full_dimensions.height);
    }

    OpticsProfileReceipt receipt;
    if (optics_provider == nullptr) {
        receipt.status = OpticsProfileStatus::disabled;
        receipt.provider_id = "none";
        receipt.provider_version = "none";
    } else if (std::holds_alternative<PixelBuffer>(preview_reference)) {
        auto corrected = optics_provider->correct_reference_rgb(
            working_to_linear_reference(working_proxy),
            metadata,
            optics_settings
        );
        receipt = std::move(corrected.receipt);
        if (corrected.corrected_reference_rgb.has_value()) {
            const double level_zero_scale_x = working_proxy.level_zero_to_raster_scale_x;
            const double level_zero_scale_y = working_proxy.level_zero_to_raster_scale_y;
            const Dimensions corrected_dimensions = corrected.corrected_reference_rgb->dimensions;
            DevelopedSourcePixels corrected_source = std::move(*corrected.corrected_reference_rgb);
            working_proxy = proxy_detail::resize_developed_source_to_working(
                corrected_source,
                corrected_dimensions
            );
            working_proxy.level_zero_to_raster_scale_x = level_zero_scale_x;
            working_proxy.level_zero_to_raster_scale_y = level_zero_scale_y;
        }
    } else {
        auto corrected = optics_provider->correct_scene_linear_reference(
            working_to_scene_linear_reference(working_proxy),
            metadata,
            optics_settings
        );
        receipt = std::move(corrected.receipt);
        if (corrected.corrected_scene_linear_rgb.has_value()) {
            const double level_zero_scale_x = working_proxy.level_zero_to_raster_scale_x;
            const double level_zero_scale_y = working_proxy.level_zero_to_raster_scale_y;
            const Dimensions corrected_dimensions =
                corrected.corrected_scene_linear_rgb->dimensions;
            DevelopedSourcePixels corrected_source =
                std::move(*corrected.corrected_scene_linear_rgb);
            working_proxy = proxy_detail::resize_developed_source_to_working(
                corrected_source,
                corrected_dimensions
            );
            working_proxy.level_zero_to_raster_scale_x = level_zero_scale_x;
            working_proxy.level_zero_to_raster_scale_y = level_zero_scale_y;
        }
    }
    apply_source_rendering(working_proxy, source_rendering);
    // Optical providers currently retain preview raster geometry. If an adapter ever returns a
    // different extent, a pre-warp sensor map would be misleading; omit it instead of stretching
    // it or reopening the RAW source just for diagnostics.
    if (sensor_clipping_mask.has_value()
        && sensor_clipping_mask->dimensions != working_proxy.dimensions) {
        sensor_clipping_mask.reset();
    }
    return {
        .working_proxy = std::move(working_proxy),
        .raw_development_receipt = std::move(raw_development_receipt),
        .raw_pipeline_receipt = std::move(developed.pipeline_receipt),
        .optics_receipt = std::move(receipt),
        .sensor_clipping_mask = std::move(sensor_clipping_mask),
        .raw_rebinding_source = nullptr,
    };
}

[[nodiscard]] PreparedWarmEditProxy prepare_warm_edit_proxy_from_preview_reference(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    const RawPipelinePolicy policy = raw_pipeline_policy_from_environment();
    if (auto rebindable = raw_pipeline_detail::try_prepare_raw_preview_rebinding(
            session,
            raw_development_plan,
            max_edge,
            policy,
            default_camera_profile_catalog()
        )) {
        auto prepared = finish_warm_edit_proxy(
            session.metadata(),
            max_edge,
            std::move(rebindable->developed),
            optics_provider,
            optics_settings
        );
        prepared.raw_rebinding_source = std::move(rebindable->source);
        return prepared;
    }
    return finish_warm_edit_proxy(
        session.metadata(),
        max_edge,
        develop_source_reference(session, raw_development_plan, max_edge, policy),
        optics_provider,
        optics_settings
    );
}

[[nodiscard]] PreparedWarmEditProxy prepare_warm_edit_proxy_from_foundation_reference(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    auto rebindable = raw_pipeline_detail::prepare_raw_foundation_preview_rebinding(
        session,
        raw_development_plan,
        foundation,
        max_edge,
        raw_pipeline_policy_from_environment(),
        default_camera_profile_catalog()
    );
    auto prepared = finish_warm_edit_proxy(
        session.metadata(),
        max_edge,
        std::move(rebindable.developed),
        optics_provider,
        optics_settings
    );
    prepared.raw_rebinding_source = std::move(rebindable.source);
    return prepared;
}

} // namespace

bool EditPreviewExecutionReceipt::valid() const noexcept {
    const auto valid_adjustment_backend = [](const EditPreviewBackend backend,
                                             const std::uint32_t version) {
        switch (backend) {
        case EditPreviewBackend::cpu:
            return version == edit_preview_cpu_adjustment_backend_version;
        case EditPreviewBackend::metal:
            return version == edit_preview_metal_adjustment_backend_version;
        }
        return false;
    };
    const auto valid_display_backend = [](const EditPreviewBackend backend,
                                          const std::uint32_t version) {
        switch (backend) {
        case EditPreviewBackend::cpu:
            return version == edit_preview_cpu_display_backend_version;
        case EditPreviewBackend::metal:
            return version == edit_preview_metal_display_backend_version;
        }
        return false;
    };
    return schema_version == edit_preview_execution_receipt_schema_version
           && valid_adjustment_backend(adjustment_backend, adjustment_backend_version)
           && adjustment_execution_contract_version == edit_execution_plan_identity_version
           && valid_display_backend(display_backend, display_backend_version)
           && display_output_contract_version == display_srgb8_output_transform_version
           && (!adjustment_fell_back || adjustment_backend == EditPreviewBackend::cpu)
           && (!display_fell_back || display_backend == EditPreviewBackend::cpu)
           && (!presentation_fell_back || display_backend == EditPreviewBackend::metal)
           && (!fused_pipeline || display_backend == EditPreviewBackend::metal)
           && (!fused_pipeline || (!adjustment_fell_back && !display_fell_back))
           && ((adjustment_fell_back || display_fell_back || presentation_fell_back)
               == !diagnostic.empty());
}

std::string edit_preview_execution_receipt_identity(const EditPreviewExecutionReceipt& receipt) {
    if (!receipt.valid()) {
        throw std::invalid_argument("edit-preview execution receipt is invalid");
    }
    const auto backend_identity = [](const EditPreviewBackend backend) {
        switch (backend) {
        case EditPreviewBackend::cpu:
            return std::string_view{"cpu"};
        case EditPreviewBackend::metal:
            return std::string_view{"metal"};
        }
        throw std::invalid_argument("edit-preview execution backend is invalid");
    };
    return "shadow-edit-preview-execution-v1;adjustment="
           + std::string(backend_identity(receipt.adjustment_backend)) + "-v"
           + std::to_string(receipt.adjustment_backend_version)
           + ";plan=" + std::to_string(receipt.adjustment_execution_contract_version)
           + ";display=" + std::string(backend_identity(receipt.display_backend)) + "-v"
           + std::to_string(receipt.display_backend_version)
           + ";display-contract=" + std::to_string(receipt.display_output_contract_version)
           + ";route=" + (receipt.fused_pipeline ? "fused" : "staged");
}

std::string edit_preview_generator_implementation_identity() {
    // This identity names the implementations the current generator can actually select, not a
    // local device. Runtime availability and fallback diagnostics remain on each receipt.
    return "shadow-edit-preview-generator-v1;plan="
           + std::to_string(edit_execution_plan_identity_version) + ";adjustment-cpu="
           + std::string(adjustment_backend_identity(AdjustmentBackend::cpu)) + ";adjustment-metal="
           + std::string(adjustment_backend_identity(AdjustmentBackend::metal)) + ";display-cpu="
           + std::string(display_output_backend_identity(DisplayOutputBackend::cpu))
           + ";display-metal="
           + std::string(display_output_backend_identity(DisplayOutputBackend::metal))
           + ";warm-fused-metal=v1;features=resident-source,double-slot,"
             "immutable-color-resources,technical-detail,texture,clarity,optics,"
             "adjustment,display"
           + ";display-contract=" + std::to_string(display_srgb8_output_transform_version)
           + ";jpeg-444=" + std::to_string(edit_preview_jpeg_444_contract_version);
}

WarmEditPreviewSession::WarmEditPreviewSession(
    FloatRgbImage working_proxy,
    const std::uint32_t max_edge,
    RawDevelopmentReceipt raw_development_receipt,
    RawPipelineReceipt raw_pipeline_receipt,
    OpticsProfileReceipt optics_receipt,
    std::optional<SensorClippingMask> sensor_clipping_mask,
    std::shared_ptr<const raw_pipeline_detail::RawPreviewRebindingSource> raw_rebinding_source,
    std::shared_ptr<const OpticsProvider> retained_optics_provider,
    OpticsSettings retained_optics_settings
) :
    working_proxy_(std::move(working_proxy)), max_edge_(max_edge),
    raw_development_receipt_(std::move(raw_development_receipt)),
    raw_pipeline_receipt_(std::move(raw_pipeline_receipt)),
    optics_receipt_(std::move(optics_receipt)),
    sensor_clipping_mask_(std::move(sensor_clipping_mask)),
    raw_rebinding_source_(std::move(raw_rebinding_source)),
    retained_optics_provider_(std::move(retained_optics_provider)),
    retained_optics_settings_(std::move(retained_optics_settings)) {
    auto gpu = detail::prepare_warm_edit_gpu_session(working_proxy_);
    warm_gpu_session_ = std::move(gpu.session);
    warm_gpu_diagnostic_ = std::move(gpu.diagnostic);
}

Dimensions WarmEditPreviewSession::dimensions() const noexcept {
    return working_proxy_.dimensions;
}

std::uint32_t WarmEditPreviewSession::max_edge() const noexcept {
    return max_edge_;
}

const RawDevelopmentReceipt& WarmEditPreviewSession::raw_development_receipt() const noexcept {
    return raw_development_receipt_;
}

const RawPipelineReceipt& WarmEditPreviewSession::raw_pipeline_receipt() const noexcept {
    return raw_pipeline_receipt_;
}

const OpticsProfileReceipt& WarmEditPreviewSession::optics_receipt() const noexcept {
    return optics_receipt_;
}

const std::optional<SensorClippingMask>&
WarmEditPreviewSession::sensor_clipping_mask() const noexcept {
    return sensor_clipping_mask_;
}

WarmEditPreviewGpuStats WarmEditPreviewSession::gpu_stats() const noexcept {
    return warm_gpu_session_ ? warm_gpu_session_->stats() : WarmEditPreviewGpuStats{};
}

bool WarmEditPreviewSession::supports_raw_development_rebinding() const noexcept {
    return raw_rebinding_source_ != nullptr;
}

WarmEditPreviewSession WarmEditPreviewSession::rebind_raw_development_plan(
    const RawDevelopmentPlan& raw_development_plan
) const {
    if (raw_rebinding_source_ == nullptr) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "this edit preview does not retain a rebindable RAW camera-space source"
        );
    }
    auto prepared = finish_warm_edit_proxy(
        raw_rebinding_source_->metadata(),
        max_edge_,
        raw_rebinding_source_->bind(raw_development_plan),
        retained_optics_provider_.get(),
        retained_optics_settings_
    );
    return WarmEditPreviewSession(
        std::move(prepared.working_proxy),
        max_edge_,
        std::move(prepared.raw_development_receipt),
        std::move(prepared.raw_pipeline_receipt),
        std::move(prepared.optics_receipt),
        std::move(prepared.sensor_clipping_mask),
        raw_rebinding_source_,
        retained_optics_provider_,
        retained_optics_settings_
    );
}

EncodedProxy WarmEditPreviewSession::render_rgb8(
    const std::span<const AdjustmentNode> nodes,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    auto rendered = render_rgb8_cancellable(nodes, {}, geometry, liquify);
    if (rendered.cancelled()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable RGB8 warm preview was unexpectedly cancelled"
        );
    }
    return std::move(*rendered.completed);
}

EncodedProxy WarmEditPreviewSession::render_rgb8_layers(
    const std::span<const AdjustmentLayer> layers,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    auto rendered = render_rgb8_layers_cancellable(layers, {}, geometry, liquify);
    if (rendered.cancelled()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable layered RGB8 warm preview was unexpectedly cancelled"
        );
    }
    return std::move(*rendered.completed);
}

EncodedProxy WarmEditPreviewSession::render_jpeg(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    auto rendered = render_jpeg_cancellable(nodes, jpeg_quality, {}, geometry, liquify);
    if (rendered.cancelled()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable warm preview was unexpectedly cancelled"
        );
    }
    return std::move(*rendered.completed);
}

AnalyzedEditPreview WarmEditPreviewSession::render_jpeg_with_analysis(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    auto rendered =
        render_jpeg_with_analysis_cancellable(nodes, jpeg_quality, {}, geometry, liquify);
    if (rendered.cancelled()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable analyzed warm preview was unexpectedly cancelled"
        );
    }
    return std::move(*rendered.completed);
}

EncodedProxy WarmEditPreviewSession::render_jpeg_layers(
    const std::span<const AdjustmentLayer> layers,
    const std::uint8_t jpeg_quality,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    proxy_detail::validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_layer_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        layers,
        geometry,
        liquify,
        false,
        {},
        std::nullopt,
        detail::WarmEditGpuOutputIntent::host_rgb8
    );
    if (!prepared.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable local-mask warm preview was unexpectedly cancelled"
        );
    }
    return EncodedProxy{
        .dimensions = prepared->dimensions,
        .bytes = proxy_detail::encode_proxy_jpeg(prepared->rgb, prepared->dimensions, jpeg_quality),
    };
}

AnalyzedEditPreview WarmEditPreviewSession::render_jpeg_with_analysis_layers(
    const std::span<const AdjustmentLayer> layers,
    const std::uint8_t jpeg_quality,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    proxy_detail::validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_layer_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        layers,
        geometry,
        liquify,
        true,
        {},
        std::nullopt,
        detail::WarmEditGpuOutputIntent::host_rgb8
    );
    if (!prepared.has_value() || !prepared->edited.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "local-mask analyzed warm preview did not retain its scene-linear result"
        );
    }
    auto analysis = analyze_edit_preview(*prepared->edited, prepared->rgb, {});
    if (!analysis.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "non-cancellable local-mask preview analysis was unexpectedly cancelled"
        );
    }
    return AnalyzedEditPreview{
        .proxy =
            EncodedProxy{
                .dimensions = prepared->dimensions,
                .bytes = proxy_detail::encode_proxy_jpeg(
                    prepared->rgb,
                    prepared->dimensions,
                    jpeg_quality
                ),
            },
        .analysis = std::move(*analysis),
        .execution = std::move(prepared->execution),
    };
}

CancellableEditPreviewResult<EncodedProxy> WarmEditPreviewSession::render_rgb8_cancellable(
    const std::span<const AdjustmentNode> nodes,
    const std::stop_token cancellation,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    auto prepared = prepare_edit_preview_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        nodes,
        geometry,
        liquify,
        false,
        cancellation,
        detail::WarmEditGpuOutputIntent::host_rgb8
    );
    if (!prepared.has_value()) {
        return {};
    }
    return {
        .completed = rgb8_proxy(std::move(*prepared)),
    };
}

CancellableEditPreviewResult<InteractiveEditPreviewFrame>
WarmEditPreviewSession::render_interactive_frame_cancellable(
    const std::span<const AdjustmentNode> nodes,
    const std::stop_token cancellation,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    auto prepared = prepare_edit_preview_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        nodes,
        geometry,
        liquify,
        false,
        cancellation,
        detail::WarmEditGpuOutputIntent::metal_presentation_surface
    );
    if (!prepared.has_value()) {
        return {};
    }
    auto mask_coverage = std::move(prepared->mask_coverage);
    auto fallback = std::move(prepared->presentation_fallback_diagnostic);
    if (prepared->presentation_surface) {
        if (!prepared->rgb.empty() || !fallback.empty()) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "native presentation frame mixed Metal and host fallback storage"
            );
        }
        return {
            .completed = InteractiveEditPreviewFrame(
                prepared->dimensions,
                std::move(prepared->presentation_surface),
                std::move(mask_coverage)
            ),
        };
    }
    return {
        .completed = InteractiveEditPreviewFrame(
            rgb8_proxy(std::move(*prepared)),
            std::move(mask_coverage),
            std::move(fallback)
        ),
    };
}

CancellableEditPreviewResult<EncodedProxy> WarmEditPreviewSession::render_rgb8_layers_cancellable(
    const std::span<const AdjustmentLayer> layers,
    const std::stop_token cancellation,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    auto prepared = prepare_edit_preview_layer_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        layers,
        geometry,
        liquify,
        false,
        cancellation,
        std::nullopt,
        detail::WarmEditGpuOutputIntent::host_rgb8
    );
    if (!prepared.has_value()) {
        return {};
    }
    return {
        .completed = rgb8_proxy(std::move(*prepared)),
    };
}

CancellableEditPreviewResult<EditPreviewRgb8WithMaskCoverage>
WarmEditPreviewSession::render_rgb8_layers_with_mask_coverage_cancellable(
    const std::span<const AdjustmentLayer> layers,
    const std::optional<std::uint32_t> target_layer_index,
    const std::stop_token cancellation,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    auto prepared = prepare_edit_preview_layer_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        layers,
        geometry,
        liquify,
        false,
        cancellation,
        target_layer_index,
        detail::WarmEditGpuOutputIntent::host_rgb8
    );
    if (!prepared.has_value()) {
        return {};
    }
    auto mask_coverage = std::move(prepared->mask_coverage);
    return {
        .completed = EditPreviewRgb8WithMaskCoverage{
            .preview = rgb8_proxy(std::move(*prepared)),
            .mask_coverage = std::move(mask_coverage),
        },
    };
}

CancellableEditPreviewResult<InteractiveEditPreviewFrame>
WarmEditPreviewSession::render_interactive_frame_layers_with_mask_coverage_cancellable(
    const std::span<const AdjustmentLayer> layers,
    const std::optional<std::uint32_t> target_layer_index,
    const std::stop_token cancellation,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    auto prepared = prepare_edit_preview_layer_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        layers,
        geometry,
        liquify,
        false,
        cancellation,
        target_layer_index,
        detail::WarmEditGpuOutputIntent::metal_presentation_surface
    );
    if (!prepared.has_value()) {
        return {};
    }
    auto mask_coverage = std::move(prepared->mask_coverage);
    auto fallback = std::move(prepared->presentation_fallback_diagnostic);
    if (prepared->presentation_surface) {
        if (!prepared->rgb.empty() || !fallback.empty()) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "layered native presentation frame mixed Metal and host fallback storage"
            );
        }
        return {
            .completed = InteractiveEditPreviewFrame(
                prepared->dimensions,
                std::move(prepared->presentation_surface),
                std::move(mask_coverage)
            ),
        };
    }
    return {
        .completed = InteractiveEditPreviewFrame(
            rgb8_proxy(std::move(*prepared)),
            std::move(mask_coverage),
            std::move(fallback)
        ),
    };
}

CancellableEditPreviewResult<EncodedProxy> WarmEditPreviewSession::render_jpeg_cancellable(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality,
    const std::stop_token cancellation,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    proxy_detail::validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        nodes,
        geometry,
        liquify,
        false,
        cancellation,
        detail::WarmEditGpuOutputIntent::host_rgb8
    );
    if (!prepared.has_value()) {
        return {};
    }
    auto encoded = proxy_detail::encode_proxy_jpeg_cancellable(
        prepared->rgb,
        prepared->dimensions,
        jpeg_quality,
        cancellation
    );
    if (!encoded.has_value()) {
        return {};
    }
    return {
        .completed = EncodedProxy{
            .dimensions = prepared->dimensions,
            .bytes = std::move(*encoded),
        },
    };
}

CancellableEditPreviewResult<AnalyzedEditPreview>
WarmEditPreviewSession::render_jpeg_with_analysis_cancellable(
    const std::span<const AdjustmentNode> nodes,
    const std::uint8_t jpeg_quality,
    const std::stop_token cancellation,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    proxy_detail::validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        nodes,
        geometry,
        liquify,
        true,
        cancellation,
        detail::WarmEditGpuOutputIntent::host_rgb8
    );
    if (!prepared.has_value()) {
        return {};
    }
    if (!prepared->edited.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "analyzed warm preview did not retain its scene-linear result"
        );
    }
    auto analysis = analyze_edit_preview(*prepared->edited, prepared->rgb, cancellation);
    if (!analysis.has_value()) {
        return {};
    }
    auto encoded = proxy_detail::encode_proxy_jpeg_cancellable(
        prepared->rgb,
        prepared->dimensions,
        jpeg_quality,
        cancellation
    );
    if (!encoded.has_value()) {
        return {};
    }

    EncodedProxy proxy;
    proxy.dimensions = prepared->dimensions;
    proxy.bytes = std::move(*encoded);
    return {
        .completed = AnalyzedEditPreview{
            .proxy = std::move(proxy),
            .analysis = std::move(*analysis),
            .execution = std::move(prepared->execution),
        },
    };
}

CancellableEditPreviewResult<AnalyzedEditPreviewWithMaskCoverage>
WarmEditPreviewSession::render_jpeg_with_analysis_layers_and_mask_coverage_cancellable(
    const std::span<const AdjustmentLayer> layers,
    const std::optional<std::uint32_t> target_layer_index,
    const std::uint8_t jpeg_quality,
    const std::stop_token cancellation,
    const PhotoGeometry& geometry,
    const PhotoLiquify* liquify
) const {
    proxy_detail::validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_layer_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        layers,
        geometry,
        liquify,
        true,
        cancellation,
        target_layer_index,
        detail::WarmEditGpuOutputIntent::host_rgb8
    );
    if (!prepared.has_value()) {
        return {};
    }
    if (!prepared->edited.has_value()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "mask-coverage analysis did not retain its paired scene-linear frame"
        );
    }
    auto analysis = analyze_edit_preview(*prepared->edited, prepared->rgb, cancellation);
    if (!analysis.has_value()) {
        return {};
    }
    auto encoded = proxy_detail::encode_proxy_jpeg_cancellable(
        prepared->rgb,
        prepared->dimensions,
        jpeg_quality,
        cancellation
    );
    if (!encoded.has_value()) {
        return {};
    }
    AnalyzedEditPreview preview{
        .proxy =
            EncodedProxy{
                .dimensions = prepared->dimensions,
                .bytes = std::move(*encoded),
            },
        .analysis = std::move(*analysis),
        .execution = std::move(prepared->execution),
    };
    if (cancellation.stop_requested()) {
        return {};
    }
    return {
        .completed = AnalyzedEditPreviewWithMaskCoverage{
            .preview = std::move(preview),
            .mask_coverage = std::move(prepared->mask_coverage),
        },
    };
}

WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    return prepare_warm_edit_preview(
        session,
        max_edge,
        preview_raw_development_plan(),
        optics_provider,
        optics_settings
    );
}

WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    validate_warm_edit_max_edge(max_edge);
    proxy_detail::validate_raw_development_plan_intent(
        raw_development_plan,
        RawDevelopmentIntent::preview,
        "warm edit preview"
    );
    auto prepared = prepare_warm_edit_proxy_from_preview_reference(
        session,
        max_edge,
        raw_development_plan,
        optics_provider,
        optics_settings
    );
    return WarmEditPreviewSession(
        std::move(prepared.working_proxy),
        max_edge,
        std::move(prepared.raw_development_receipt),
        std::move(prepared.raw_pipeline_receipt),
        std::move(prepared.optics_receipt),
        std::move(prepared.sensor_clipping_mask),
        optics_provider == nullptr ? std::move(prepared.raw_rebinding_source) : nullptr
    );
}

WarmEditPreviewSession prepare_warm_edit_preview(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    validate_warm_edit_max_edge(max_edge);
    proxy_detail::validate_raw_development_plan_intent(
        raw_development_plan,
        RawDevelopmentIntent::preview,
        "AI RAW foundation warm edit preview"
    );
    auto prepared = prepare_warm_edit_proxy_from_foundation_reference(
        session,
        max_edge,
        raw_development_plan,
        foundation,
        optics_provider,
        optics_settings
    );
    return WarmEditPreviewSession(
        std::move(prepared.working_proxy),
        max_edge,
        std::move(prepared.raw_development_receipt),
        std::move(prepared.raw_pipeline_receipt),
        std::move(prepared.optics_receipt),
        std::move(prepared.sensor_clipping_mask),
        optics_provider == nullptr ? std::move(prepared.raw_rebinding_source) : nullptr
    );
}

WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    std::shared_ptr<const OpticsProvider> optics_provider,
    const OpticsSettings& optics_settings
) {
    validate_warm_edit_max_edge(max_edge);
    proxy_detail::validate_raw_development_plan_intent(
        raw_development_plan,
        RawDevelopmentIntent::preview,
        "rebindable warm edit preview"
    );
    auto prepared = prepare_warm_edit_proxy_from_preview_reference(
        session,
        max_edge,
        raw_development_plan,
        optics_provider.get(),
        optics_settings
    );
    return WarmEditPreviewSession(
        std::move(prepared.working_proxy),
        max_edge,
        std::move(prepared.raw_development_receipt),
        std::move(prepared.raw_pipeline_receipt),
        std::move(prepared.optics_receipt),
        std::move(prepared.sensor_clipping_mask),
        std::move(prepared.raw_rebinding_source),
        std::move(optics_provider),
        optics_settings
    );
}

WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
    const DecodeSession& session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    std::shared_ptr<const OpticsProvider> optics_provider,
    const OpticsSettings& optics_settings
) {
    validate_warm_edit_max_edge(max_edge);
    proxy_detail::validate_raw_development_plan_intent(
        raw_development_plan,
        RawDevelopmentIntent::preview,
        "rebindable AI RAW foundation warm edit preview"
    );
    auto prepared = prepare_warm_edit_proxy_from_foundation_reference(
        session,
        max_edge,
        raw_development_plan,
        foundation,
        optics_provider.get(),
        optics_settings
    );
    return WarmEditPreviewSession(
        std::move(prepared.working_proxy),
        max_edge,
        std::move(prepared.raw_development_receipt),
        std::move(prepared.raw_pipeline_receipt),
        std::move(prepared.optics_receipt),
        std::move(prepared.sensor_clipping_mask),
        std::move(prepared.raw_rebinding_source),
        std::move(optics_provider),
        optics_settings
    );
}

} // namespace shadow::image
