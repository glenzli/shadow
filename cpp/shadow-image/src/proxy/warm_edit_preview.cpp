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

#include "../optics/metal_scene_linear_region_optics.hpp"
#include "../optics/scene_linear_region_optics.hpp"
#include "../raw/raw_frame_source_preparation.hpp"
#include "../raw/raw_preview_rebinding.hpp"
#include "developed_source_raster.hpp"
#include "edit_preview_rendering.hpp"
#include "full_edit_detail_metal_source.hpp"
#include "jpeg_proxy_encoding.hpp"
#include "proxy_render_request_validation.hpp"
#include "warm_edit_gpu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

namespace detail {

class MetalSceneLinearRegionWarmPreviewAccess final {
  public:
    [[nodiscard]] static void* buffer(const MetalSceneLinearRegionLease& lease) noexcept {
        return lease.native_buffer_handle();
    }

    [[nodiscard]] static void* device(const MetalSceneLinearRegionLease& lease) noexcept {
        return lease.native_device_handle();
    }

    [[nodiscard]] static void* queue(const MetalSceneLinearRegionLease& lease) noexcept {
        return lease.native_queue_handle();
    }
};

} // namespace detail

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

[[nodiscard]] WorkingRgbSpace linear_srgb_working_space() {
    return WorkingRgbSpace{
        .id = "srgb-d65-linear",
        .primaries =
            {
                Chromaticity{0.6400, 0.3300},
                Chromaticity{0.3000, 0.6000},
                Chromaticity{0.1500, 0.0600},
            },
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] FloatRgbImage
resident_working_proxy(const detail::MetalRawPreviewResidentOutput& output) {
    return FloatRgbImage{
        .dimensions = output.dimensions(),
        .row_stride_bytes = output.row_stride_bytes(),
        .pixel_format = FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = TransferFunction::linear,
        .reference = ImageReference::scene_referred,
        .working_space = linear_srgb_working_space(),
        .level_zero_to_raster_scale_x = 1.0,
        .level_zero_to_raster_scale_y = 1.0,
        .samples = {},
    };
}

[[nodiscard]] FloatRgbImage
resident_working_proxy(const Dimensions dimensions, const std::size_t row_stride_bytes) {
    return FloatRgbImage{
        .dimensions = dimensions,
        .row_stride_bytes = row_stride_bytes,
        .pixel_format = FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = TransferFunction::linear,
        .reference = ImageReference::scene_referred,
        .working_space = linear_srgb_working_space(),
        .level_zero_to_raster_scale_x = 1.0,
        .level_zero_to_raster_scale_y = 1.0,
        .samples = {},
    };
}

[[nodiscard]] bool interactive_timing_enabled() noexcept {
    const char* value = std::getenv("SHADOW_INTERACTIVE_TIMING");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

void log_warm_rebind_timing(
    const bool enabled,
    const char* const stage,
    const std::chrono::steady_clock::time_point started
) noexcept {
    if (!enabled) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    );
    std::fprintf(
        stderr,
        "shadow.interactive-timing component=warm-preview-rebind stage=%s elapsed_ms=%lld\n",
        stage,
        static_cast<long long>(elapsed.count())
    );
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
    std::optional<HighlightChromaRiskMap> highlight_chroma_risk_map;
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
    std::optional<HighlightChromaRiskMap> highlight_chroma_risk_map =
        std::move(developed.highlight_chroma_risk_map);
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
    // different extent, a pre-warp sensor mask would be misleading; omit it instead of stretching
    // it or reopening the RAW source just for diagnostics.
    if (sensor_clipping_mask.has_value()
        && sensor_clipping_mask->dimensions != working_proxy.dimensions) {
        sensor_clipping_mask.reset();
    }
    if (highlight_chroma_risk_map.has_value()
        && highlight_chroma_risk_map->dimensions != working_proxy.dimensions) {
        highlight_chroma_risk_map.reset();
    }
    return {
        .working_proxy = std::move(working_proxy),
        .raw_development_receipt = std::move(raw_development_receipt),
        .raw_pipeline_receipt = std::move(developed.pipeline_receipt),
        .optics_receipt = std::move(receipt),
        .sensor_clipping_mask = std::move(sensor_clipping_mask),
        .highlight_chroma_risk_map = std::move(highlight_chroma_risk_map),
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

[[nodiscard]] PreparedWarmEditProxy prepare_warm_edit_proxy_from_preview_reference(
    const AssetMetadata& metadata,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    RawFrame staged_frame,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    const RawPipelinePolicy policy = raw_pipeline_policy_from_environment();
    if (policy.mode == RawPipelineMode::require_provider_processed) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "staged RAW preview development is disabled by the RAW pipeline policy"
        );
    }
    auto rebindable = raw_pipeline_detail::prepare_raw_preview_rebinding(
        raw_pipeline_detail::prepare_raw_frame_source(
            metadata,
            std::move(staged_frame),
            raw_development_plan,
            max_edge,
            default_camera_profile_catalog()
        )
    );
    auto prepared = finish_warm_edit_proxy(
        metadata,
        max_edge,
        std::move(rebindable.developed),
        optics_provider,
        optics_settings
    );
    prepared.raw_rebinding_source = std::move(rebindable.source);
    return prepared;
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

[[nodiscard]] PreparedWarmEditProxy prepare_warm_edit_proxy_from_foundation_reference(
    const DecodeSession& metadata_session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    RawFrame staged_frame,
    const OpticsProvider* optics_provider,
    const OpticsSettings& optics_settings
) {
    auto rebindable = raw_pipeline_detail::prepare_raw_foundation_preview_rebinding(
        metadata_session,
        std::move(staged_frame),
        raw_development_plan,
        foundation,
        max_edge,
        raw_pipeline_policy_from_environment(),
        default_camera_profile_catalog()
    );
    auto prepared = finish_warm_edit_proxy(
        metadata_session.metadata(),
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
           + ";warm-denoise-range=v2;warm-fused-metal=v1;features=resident-source,double-slot,"
             "immutable-color-resources,technical-detail,texture,clarity,optics,"
             "adjustment,display"
           + ";warm-retouch-donor=20260920.1;display-contract="
           + std::to_string(display_srgb8_output_transform_version)
           + ";jpeg-444=" + std::to_string(edit_preview_jpeg_444_contract_version);
}

WarmEditPreviewSession::WarmEditPreviewSession(
    FloatRgbImage working_proxy,
    const std::uint32_t max_edge,
    RawDevelopmentReceipt raw_development_receipt,
    RawPipelineReceipt raw_pipeline_receipt,
    OpticsProfileReceipt optics_receipt,
    std::optional<SensorClippingMask> sensor_clipping_mask,
    std::optional<HighlightChromaRiskMap> highlight_chroma_risk_map,
    std::shared_ptr<const raw_pipeline_detail::RawPreviewRebindingSource> raw_rebinding_source,
    std::shared_ptr<const OpticsProvider> retained_optics_provider,
    OpticsSettings retained_optics_settings,
    std::shared_ptr<detail::WarmEditGpuSession> adopted_warm_gpu_session
) :
    working_proxy_(std::move(working_proxy)), max_edge_(max_edge),
    raw_development_receipt_(std::move(raw_development_receipt)),
    raw_pipeline_receipt_(std::move(raw_pipeline_receipt)),
    optics_receipt_(std::move(optics_receipt)),
    sensor_clipping_mask_(std::move(sensor_clipping_mask)),
    highlight_chroma_risk_map_(std::move(highlight_chroma_risk_map)),
    raw_rebinding_source_(std::move(raw_rebinding_source)),
    retained_optics_provider_(std::move(retained_optics_provider)),
    retained_optics_settings_(std::move(retained_optics_settings)) {
    if (adopted_warm_gpu_session) {
        warm_gpu_session_ = std::move(adopted_warm_gpu_session);
    } else {
        auto gpu = detail::prepare_warm_edit_gpu_session(
            working_proxy_,
            sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
            highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr
        );
        warm_gpu_session_ = std::move(gpu.session);
        warm_gpu_diagnostic_ = std::move(gpu.diagnostic);
    }
}

Dimensions WarmEditPreviewSession::dimensions() const noexcept {
    return working_proxy_.dimensions;
}

Dimensions WarmEditPreviewSession::level_zero_dimensions() const noexcept {
    const auto level_zero_axis = [](const std::uint32_t raster_extent, const double scale) {
        if (raster_extent == 0U || !std::isfinite(scale) || scale <= 0.0) {
            return raster_extent;
        }
        const double extent = static_cast<double>(raster_extent) / scale;
        if (!std::isfinite(extent) || extent < 1.0
            || extent > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            return raster_extent;
        }
        return static_cast<std::uint32_t>(std::llround(extent));
    };
    return {
        .width = level_zero_axis(
            working_proxy_.dimensions.width,
            working_proxy_.level_zero_to_raster_scale_x
        ),
        .height = level_zero_axis(
            working_proxy_.dimensions.height,
            working_proxy_.level_zero_to_raster_scale_y
        ),
    };
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

const std::optional<HighlightChromaRiskMap>&
WarmEditPreviewSession::highlight_chroma_risk_map() const noexcept {
    return highlight_chroma_risk_map_;
}

WarmEditPreviewGpuStats WarmEditPreviewSession::gpu_stats() const noexcept {
    return warm_gpu_session_ ? warm_gpu_session_->stats() : WarmEditPreviewGpuStats{};
}

bool WarmEditPreviewSession::supports_raw_development_rebinding() const noexcept {
    return raw_rebinding_source_ != nullptr;
}

bool WarmEditPreviewSession::supports_raw_white_balance_picker() const noexcept {
    return raw_rebinding_source_ != nullptr
           && raw_rebinding_source_->supports_raw_white_balance_picker();
}

std::optional<RawWhiteBalancePresentation> WarmEditPreviewSession::pick_raw_white_balance(
    const double normalized_x,
    const double normalized_y
) const noexcept {
    return raw_rebinding_source_ != nullptr
               ? raw_rebinding_source_->pick_raw_white_balance(normalized_x, normalized_y)
               : std::nullopt;
}

std::optional<RawWhiteBalancePresentation>
WarmEditPreviewSession::auto_raw_white_balance() const noexcept {
    return raw_rebinding_source_ != nullptr ? raw_rebinding_source_->auto_raw_white_balance()
                                            : std::nullopt;
}

raw_pipeline_detail::RawPreviewRebindingTelemetry
WarmEditPreviewSession::raw_rebinding_telemetry() const noexcept {
    return raw_rebinding_source_ != nullptr ? raw_rebinding_source_->telemetry()
                                            : raw_pipeline_detail::RawPreviewRebindingTelemetry{};
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
    const bool timing_enabled = interactive_timing_enabled();
    const auto timing_started = std::chrono::steady_clock::now();
    // Ordinary RAW rebinds retain the same denoised CFA plane. Keep the reconstructed fp32
    // preview resident through any device-eligible optics evidence and the canonical source
    // rendering stage. Unsupported optics remain on the exact materialized route below.
    if (retained_optics_provider_ != nullptr) {
        if (auto resident = raw_rebinding_source_->try_bind_metal_resident(raw_development_plan)) {
            log_warm_rebind_timing(timing_enabled, "resident-rebind-ready", timing_started);
            try {
                const Dimensions dimensions = resident->output.dimensions();
                auto optics = detail::prepare_scene_linear_region_optics(
                    retained_optics_provider_.get(),
                    dimensions,
                    raw_rebinding_source_->metadata(),
                    retained_optics_settings_
                );
                if (!optics.device_resident_eligible()) {
                    log_warm_rebind_timing(
                        timing_enabled,
                        "resident-optics-declined",
                        timing_started
                    );
                } else {
                    const GeometryPixelRect full_region{
                        .x = 0U,
                        .y = 0U,
                        .width = dimensions.width,
                        .height = dimensions.height,
                    };
                    auto corrected = detail::apply_metal_scene_linear_preview_optics(
                        resident->output,
                        optics,
                        optics.prepare_region(full_region)
                    );
                    log_warm_rebind_timing(timing_enabled, "resident-optics-ready", timing_started);
                    const SourceRenderingReceipt source_rendering = resolve_source_rendering(
                        raw_rebinding_source_->metadata(),
                        resident->pipeline_receipt
                    );
                    const auto source_rendering_attempt =
                        detail::apply_source_rendering_in_place_metal(
                            detail::MetalSceneLinearRegionWarmPreviewAccess::device(corrected),
                            detail::MetalSceneLinearRegionWarmPreviewAccess::queue(corrected),
                            detail::MetalSceneLinearRegionWarmPreviewAccess::buffer(corrected),
                            corrected.dimensions(),
                            source_rendering
                        );
                    if (source_rendering_attempt.applied) {
                        FloatRgbImage layout = resident_working_proxy(
                            corrected.dimensions(),
                            static_cast<std::size_t>(corrected.dimensions().width) * 3U
                                * sizeof(float)
                        );
                        auto warm = detail::prepare_warm_edit_gpu_session(
                            detail::WarmEditGpuAdoptedSource{
                                .dimensions = corrected.dimensions(),
                                .row_stride_bytes = layout.row_stride_bytes,
                                .native_device_handle =
                                    detail::MetalSceneLinearRegionWarmPreviewAccess::device(
                                        corrected
                                    ),
                                .native_buffer_handle =
                                    detail::MetalSceneLinearRegionWarmPreviewAccess::buffer(
                                        corrected
                                    ),
                                .source_buffer_bytes = corrected.retained_bytes(),
                                .external_resident_bytes =
                                    resident->output.external_resident_bytes(),
                                .resident_allowance_bytes =
                                    resident->output.resident_allowance_bytes(),
                                .working_space = layout.working_space,
                                .level_zero_to_raster_scale_x = layout.level_zero_to_raster_scale_x,
                                .level_zero_to_raster_scale_y = layout.level_zero_to_raster_scale_y,
                                .sensor_clipping_mask = resident->sensor_clipping_mask.has_value()
                                                            ? &*resident->sensor_clipping_mask
                                                            : nullptr,
                                .highlight_chroma_risk_map =
                                    resident->highlight_chroma_risk_map.has_value()
                                        ? &*resident->highlight_chroma_risk_map
                                        : nullptr,
                            }
                        );
                        if (warm.session) {
                            log_warm_rebind_timing(
                                timing_enabled,
                                "resident-warm-session-ready",
                                timing_started
                            );
                            return WarmEditPreviewSession(
                                layout,
                                max_edge_,
                                std::move(resident->raw_development_receipt),
                                std::move(resident->pipeline_receipt),
                                optics.receipt(),
                                std::move(resident->sensor_clipping_mask),
                                std::move(resident->highlight_chroma_risk_map),
                                raw_rebinding_source_,
                                retained_optics_provider_,
                                retained_optics_settings_,
                                std::move(warm.session)
                            );
                        }
                    }
                    log_warm_rebind_timing(
                        timing_enabled,
                        "resident-optics-warm-session-declined",
                        timing_started
                    );
                }
            } catch (const DecodeError& error) {
                // Device optics is an optional continuation. Preserve the existing precise host
                // implementation whenever its prepared full-preview evidence cannot be admitted.
                log_warm_rebind_timing(timing_enabled, "resident-optics-fallback", timing_started);
                if (timing_enabled) {
                    std::fprintf(
                        stderr,
                        "shadow.interactive-timing component=warm-preview-rebind "
                        "stage=resident-optics-diagnostic reason=\"%.240s\"\n",
                        error.what()
                    );
                }
            }
        } else {
            log_warm_rebind_timing(timing_enabled, "resident-rebind-declined", timing_started);
        }
    } else {
        if (auto resident = raw_rebinding_source_->try_bind_metal_resident(raw_development_plan)) {
            log_warm_rebind_timing(timing_enabled, "resident-rebind-ready", timing_started);
            const SourceRenderingReceipt source_rendering = resolve_source_rendering(
                raw_rebinding_source_->metadata(),
                resident->pipeline_receipt
            );
            const auto source_rendering_attempt = detail::apply_source_rendering_in_place_metal(
                resident->output.native_device_handle(),
                resident->output.native_queue_handle(),
                resident->output.native_buffer_handle(),
                resident->output.dimensions(),
                source_rendering
            );
            if (source_rendering_attempt.applied) {
                log_warm_rebind_timing(
                    timing_enabled,
                    "resident-source-rendering-ready",
                    timing_started
                );
                FloatRgbImage layout = resident_working_proxy(resident->output);
                auto warm = detail::prepare_warm_edit_gpu_session(
                    detail::WarmEditGpuAdoptedSource{
                        .dimensions = resident->output.dimensions(),
                        .row_stride_bytes = resident->output.row_stride_bytes(),
                        .native_device_handle = resident->output.native_device_handle(),
                        .native_buffer_handle = resident->output.native_buffer_handle(),
                        .source_buffer_bytes = resident->output.output_bytes(),
                        .external_resident_bytes = resident->output.external_resident_bytes(),
                        .resident_allowance_bytes = resident->output.resident_allowance_bytes(),
                        .working_space = layout.working_space,
                        .level_zero_to_raster_scale_x = layout.level_zero_to_raster_scale_x,
                        .level_zero_to_raster_scale_y = layout.level_zero_to_raster_scale_y,
                        .sensor_clipping_mask = resident->sensor_clipping_mask.has_value()
                                                    ? &*resident->sensor_clipping_mask
                                                    : nullptr,
                        .highlight_chroma_risk_map = resident->highlight_chroma_risk_map.has_value()
                                                         ? &*resident->highlight_chroma_risk_map
                                                         : nullptr,
                    }
                );
                if (warm.session) {
                    log_warm_rebind_timing(
                        timing_enabled,
                        "resident-warm-session-ready",
                        timing_started
                    );
                    return WarmEditPreviewSession(
                        layout,
                        max_edge_,
                        std::move(resident->raw_development_receipt),
                        std::move(resident->pipeline_receipt),
                        OpticsProfileReceipt{},
                        std::move(resident->sensor_clipping_mask),
                        std::move(resident->highlight_chroma_risk_map),
                        raw_rebinding_source_,
                        retained_optics_provider_,
                        retained_optics_settings_,
                        std::move(warm.session)
                    );
                }
                log_warm_rebind_timing(
                    timing_enabled,
                    "resident-warm-session-declined",
                    timing_started
                );
            } else {
                log_warm_rebind_timing(
                    timing_enabled,
                    "resident-source-rendering-declined",
                    timing_started
                );
            }
        } else {
            log_warm_rebind_timing(timing_enabled, "resident-rebind-declined", timing_started);
        }
    }
    auto prepared = finish_warm_edit_proxy(
        raw_rebinding_source_->metadata(),
        max_edge_,
        raw_rebinding_source_->bind(raw_development_plan),
        retained_optics_provider_.get(),
        retained_optics_settings_
    );
    log_warm_rebind_timing(timing_enabled, "materialized-fallback-ready", timing_started);
    return WarmEditPreviewSession(
        std::move(prepared.working_proxy),
        max_edge_,
        std::move(prepared.raw_development_receipt),
        std::move(prepared.raw_pipeline_receipt),
        std::move(prepared.optics_receipt),
        std::move(prepared.sensor_clipping_mask),
        std::move(prepared.highlight_chroma_risk_map),
        raw_rebinding_source_,
        retained_optics_provider_,
        retained_optics_settings_
    );
}

bool WarmEditPreviewSession::supports_raw_foundation_amount_rebinding() const noexcept {
    return raw_rebinding_source_ != nullptr
           && raw_rebinding_source_->supports_foundation_amount_rebinding();
}

WarmEditPreviewSession WarmEditPreviewSession::rebind_raw_foundation_amount(
    const RawDevelopmentPlan& raw_development_plan,
    const std::uint8_t amount_percent
) const {
    if (!supports_raw_foundation_amount_rebinding()) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            0,
            "this edit preview does not retain a rebindable AI foundation amount basis"
        );
    }
    auto prepared = finish_warm_edit_proxy(
        raw_rebinding_source_->metadata(),
        max_edge_,
        raw_rebinding_source_->bind_foundation_amount(raw_development_plan, amount_percent),
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
        std::move(prepared.highlight_chroma_risk_map),
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
        sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr,
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
        sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr,
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
        sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr,
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
        sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr,
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
        sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr,
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
    const PhotoLiquify* liquify,
    const std::optional<std::uint32_t> target_component_index
) const {
    auto prepared = prepare_edit_preview_layer_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        layers,
        geometry,
        liquify,
        sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr,
        false,
        cancellation,
        target_layer_index,
        detail::WarmEditGpuOutputIntent::host_rgb8,
        target_component_index
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
    const PhotoLiquify* liquify,
    const std::optional<std::uint32_t> target_component_index
) const {
    auto prepared = prepare_edit_preview_layer_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        layers,
        geometry,
        liquify,
        sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr,
        false,
        cancellation,
        target_layer_index,
        detail::WarmEditGpuOutputIntent::metal_presentation_surface,
        target_component_index
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
        sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr,
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
        sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr,
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
    const PhotoLiquify* liquify,
    const std::optional<std::uint32_t> target_component_index
) const {
    proxy_detail::validate_jpeg_quality(jpeg_quality);
    auto prepared = prepare_edit_preview_layer_pixels(
        working_proxy_,
        warm_gpu_session_,
        warm_gpu_diagnostic_,
        layers,
        geometry,
        liquify,
        sensor_clipping_mask_.has_value() ? &*sensor_clipping_mask_ : nullptr,
        highlight_chroma_risk_map_.has_value() ? &*highlight_chroma_risk_map_ : nullptr,
        true,
        cancellation,
        target_layer_index,
        detail::WarmEditGpuOutputIntent::host_rgb8,
        target_component_index
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
        std::move(prepared.highlight_chroma_risk_map),
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
        std::move(prepared.highlight_chroma_risk_map),
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
        std::move(prepared.highlight_chroma_risk_map),
        std::move(prepared.raw_rebinding_source),
        std::move(optics_provider),
        optics_settings
    );
}

WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
    const DecodeSession& metadata_session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    RawFrame staged_frame,
    std::shared_ptr<const OpticsProvider> optics_provider,
    const OpticsSettings& optics_settings
) {
    return prepare_rebindable_warm_edit_preview(
        metadata_session.metadata(),
        max_edge,
        raw_development_plan,
        std::move(staged_frame),
        std::move(optics_provider),
        optics_settings
    );
}

WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
    const AssetMetadata& metadata,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    RawFrame staged_frame,
    std::shared_ptr<const OpticsProvider> optics_provider,
    const OpticsSettings& optics_settings
) {
    validate_warm_edit_max_edge(max_edge);
    proxy_detail::validate_raw_development_plan_intent(
        raw_development_plan,
        RawDevelopmentIntent::preview,
        "staged rebindable RAW warm edit preview"
    );
    auto prepared = prepare_warm_edit_proxy_from_preview_reference(
        metadata,
        max_edge,
        raw_development_plan,
        std::move(staged_frame),
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
        std::move(prepared.highlight_chroma_risk_map),
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
        std::move(prepared.highlight_chroma_risk_map),
        std::move(prepared.raw_rebinding_source),
        std::move(optics_provider),
        optics_settings
    );
}

WarmEditPreviewSession prepare_rebindable_warm_edit_preview(
    const DecodeSession& metadata_session,
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& raw_development_plan,
    const RawFoundationCameraRgbView& foundation,
    RawFrame staged_frame,
    std::shared_ptr<const OpticsProvider> optics_provider,
    const OpticsSettings& optics_settings
) {
    validate_warm_edit_max_edge(max_edge);
    proxy_detail::validate_raw_development_plan_intent(
        raw_development_plan,
        RawDevelopmentIntent::preview,
        "staged rebindable AI RAW foundation warm edit preview"
    );
    auto prepared = prepare_warm_edit_proxy_from_foundation_reference(
        metadata_session,
        max_edge,
        raw_development_plan,
        foundation,
        std::move(staged_frame),
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
        std::move(prepared.highlight_chroma_risk_map),
        std::move(prepared.raw_rebinding_source),
        std::move(optics_provider),
        optics_settings
    );
}

} // namespace shadow::image
