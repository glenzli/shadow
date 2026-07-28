#include "libraw_reference_development.hpp"

#include "libraw_runtime.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_development_receipt.hpp>

#include <libraw/libraw.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace shadow::image {

namespace {

using ProcessedImage = std::unique_ptr<
    libraw_processed_image_t,
    void (*)(libraw_processed_image_t*)
>;

inline constexpr int libraw_reference_output_color = 1;
inline constexpr double libraw_reference_gamma_inverse_power = 1.0;
inline constexpr double libraw_reference_gamma_linear_toe_slope = 1.0;
inline constexpr std::uint16_t nikon_nef_high_efficiency_compression = 13U;
inline constexpr std::uint16_t nikon_nef_high_efficiency_star_compression = 14U;

[[nodiscard]] RawDevelopmentCapabilities reference_capabilities(
    const bool available,
    const bool raw_frame_available
) noexcept {
    if (!available)
        return {};

    RawDevelopmentCapabilities capabilities;
    capabilities.schema_version = raw_development_capabilities_schema_version;
    capabilities.available = true;
    capabilities.raw_frame = raw_frame_available;
    capabilities.dng_opcode_execution_receipt = false;
    capabilities.supported_intents =
        raw_development_intent_mask(RawDevelopmentIntent::preview)
        | raw_development_intent_mask(RawDevelopmentIntent::detail)
        | raw_development_intent_mask(RawDevelopmentIntent::export_image);
    capabilities.supported_qualities = raw_development_quality_mask(
        RawDevelopmentQuality::balanced
    );
    capabilities.supported_dng_opcode_policies = dng_opcode_policy_mask(
        DngOpcodePolicy::provider_default
    );
    capabilities.supported_noise_reduction_intents =
        raw_noise_reduction_intent_mask(
            RawNoiseReductionIntent::provider_default
        );
    capabilities.supported_highlight_recovery_intents =
        raw_highlight_recovery_intent_mask(
            RawHighlightRecoveryIntent::provider_default
        );
    return capabilities;
}

[[nodiscard]] std::array<DngOpcodeExecutionStatus, 3U>
dng_opcode_execution(
    const PendingCorrections& declared,
    const DngOpcodePolicy policy
) noexcept {
    std::array<DngOpcodeExecutionStatus, 3U> result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        if (declared.dng_opcode_list_bytes[index] == 0U) {
            result[index] = DngOpcodeExecutionStatus::not_declared;
            continue;
        }
        result[index] = policy == DngOpcodePolicy::provider_default
            ? DngOpcodeExecutionStatus::provider_default
            : DngOpcodeExecutionStatus::unsupported;
    }
    return result;
}

void configure_reference_render_parameters(
    LibRaw& renderer,
    const LibRawDevelopmentSettings& settings,
    const bool half_size
) {
    auto& parameters = renderer.imgdata.params;
    parameters.gamm[0] = libraw_reference_gamma_inverse_power;
    parameters.gamm[1] = libraw_reference_gamma_linear_toe_slope;
    parameters.output_bps = static_cast<int>(settings.output_bits_per_channel);
    parameters.use_camera_wb = settings.use_camera_white_balance ? 1 : 0;
    parameters.use_camera_matrix = settings.use_camera_matrix ? 1 : 0;
    parameters.bright = settings.brightness;
    parameters.exp_correc = settings.use_exposure_correction ? 1 : 0;
    parameters.no_auto_bright = settings.use_auto_brightness ? 0 : 1;
    parameters.adjust_maximum_thr = settings.maximum_adjustment_threshold;
    parameters.output_color = libraw_reference_output_color;
    parameters.user_qual = settings.demosaic_quality;
    parameters.half_size = half_size ? 1 : 0;
}

[[nodiscard]] PixelBuffer downsample_linear_reference_for_preview(
    PixelBuffer source,
    const std::uint32_t max_edge
) {
    const Dimensions target = proxy_dimensions(source.dimensions, max_edge);
    if (source.dimensions == target)
        return source;
    if (
        source.bits_per_channel != 16U || source.channels == 0U
        || source.row_stride_bytes
            != static_cast<std::size_t>(source.dimensions.width)
                * source.channels * sizeof(std::uint16_t)
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            LIBRAW_NOT_IMPLEMENTED,
            "LibRaw preview downsample requires contiguous 16-bit reference samples"
        );
    }

    PixelBuffer result;
    result.dimensions = target;
    result.bits_per_channel = source.bits_per_channel;
    result.channels = source.channels;
    result.primaries = source.primaries;
    result.transfer_function = source.transfer_function;
    result.reference = source.reference;
    result.raw_development_receipt = source.raw_development_receipt;
    result.row_stride_bytes = static_cast<std::size_t>(target.width)
        * source.channels * sizeof(std::uint16_t);
    result.samples.resize(
        static_cast<std::size_t>(target.width)
            * target.height * source.channels
    );
    const double scale_x = static_cast<double>(source.dimensions.width)
        / static_cast<double>(target.width);
    const double scale_y = static_cast<double>(source.dimensions.height)
        / static_cast<double>(target.height);
    for (std::uint32_t output_y = 0U; output_y < target.height; ++output_y) {
        const double source_y = std::max(
            0.0,
            (static_cast<double>(output_y) + 0.5) * scale_y - 0.5
        );
        const auto y0 = static_cast<std::size_t>(source_y);
        const auto y1 = std::min(
            y0 + 1U,
            static_cast<std::size_t>(source.dimensions.height - 1U)
        );
        const double fy = source_y - static_cast<double>(y0);
        for (
            std::uint32_t output_x = 0U;
            output_x < target.width;
            ++output_x
        ) {
            const double source_x = std::max(
                0.0,
                (static_cast<double>(output_x) + 0.5) * scale_x - 0.5
            );
            const auto x0 = static_cast<std::size_t>(source_x);
            const auto x1 = std::min(
                x0 + 1U,
                static_cast<std::size_t>(source.dimensions.width - 1U)
            );
            const double fx = source_x - static_cast<double>(x0);
            for (
                std::size_t channel = 0U;
                channel < source.channels;
                ++channel
            ) {
                const auto sample = [&source, channel](
                                        const std::size_t x,
                                        const std::size_t y
                                    ) {
                    return static_cast<double>(source.samples[
                        (y * source.dimensions.width + x)
                            * source.channels + channel
                    ]);
                };
                const double top = sample(x0, y0) * (1.0 - fx)
                    + sample(x1, y0) * fx;
                const double bottom = sample(x0, y1) * (1.0 - fx)
                    + sample(x1, y1) * fx;
                result.samples[
                    (static_cast<std::size_t>(output_y) * target.width
                        + output_x) * source.channels + channel
                ] = static_cast<std::uint16_t>(std::lround(std::clamp(
                    top * (1.0 - fy) + bottom * fy,
                    0.0,
                    65'535.0
                )));
            }
        }
    }
    result.raw_development_receipt.rendered_dimensions = target;
    return result;
}

} // namespace

void validate_libraw_development_settings(
    const LibRawDevelopmentSettings& settings
) {
    if (settings.schema_version != libraw_development_settings_schema_version) {
        throw std::invalid_argument(
            "unsupported LibRaw development settings schema version"
        );
    }
    if (
        !std::isfinite(settings.brightness)
        || settings.brightness <= 0.0F
        || settings.brightness > 8.0F
    ) {
        throw std::invalid_argument(
            "LibRaw brightness must be finite and in (0, 8]"
        );
    }
    if (
        !std::isfinite(settings.maximum_adjustment_threshold)
        || settings.maximum_adjustment_threshold < 0.0F
        || settings.maximum_adjustment_threshold > 1.0F
    ) {
        throw std::invalid_argument(
            "LibRaw maximum adjustment threshold must be finite and in [0, 1]"
        );
    }
    if (settings.output_bits_per_channel != 16U) {
        throw std::invalid_argument(
            "Shadow's processed-linear LibRaw contract requires 16-bit output"
        );
    }
    if (settings.demosaic_quality < 0 || settings.demosaic_quality > 13) {
        throw std::invalid_argument(
            "LibRaw demosaic quality must be in [0, 13]"
        );
    }
}

std::string compact_libraw_development_settings_identity(
    const LibRawDevelopmentSettings& settings
) {
    std::ostringstream identity;
    identity << "s" << settings.schema_version
             << "-w" << (settings.use_camera_white_balance ? 1 : 0)
             << "-m" << (settings.use_camera_matrix ? 1 : 0)
             << "-a" << (settings.use_auto_brightness ? 1 : 0)
             << "-e" << (settings.use_exposure_correction ? 1 : 0)
             << "-b" << std::hex
             << std::bit_cast<std::uint32_t>(settings.brightness)
             << "-x"
             << std::bit_cast<std::uint32_t>(
                    settings.maximum_adjustment_threshold
                )
             << std::dec
             << "-p" << settings.output_bits_per_channel
             << "-q" << settings.demosaic_quality;
    return identity.str();
}

LibRawReferenceDeveloper::LibRawReferenceDeveloper(
    std::filesystem::path path,
    const LibRawDevelopmentSettings settings,
    ProviderInfo provider_info,
    const Dimensions source_dimensions,
    const bool reference_rgb_available,
    const bool raw_frame_available,
    const std::uint16_t nikon_nef_compression
)
    : path_(std::move(path)),
      settings_(settings),
      provider_info_(std::move(provider_info)),
      source_dimensions_(source_dimensions),
      capabilities_(reference_capabilities(
          reference_rgb_available,
          raw_frame_available
      )),
      nikon_nef_compression_(nikon_nef_compression) {
    validate_libraw_development_settings(settings_);
}

const RawDevelopmentCapabilities&
LibRawReferenceDeveloper::capabilities() const noexcept {
    return capabilities_;
}

RawDevelopmentPlanNegotiation LibRawReferenceDeveloper::negotiate(
    const RawDevelopmentPlan& plan
) const noexcept {
    auto negotiation = shadow::image::negotiate_raw_development_plan(
        plan,
        capabilities_
    );
    if (
        !negotiation.accepted()
        && negotiation.unresolved == RawDevelopmentPlanAspect::quality
        && plan.quality == RawDevelopmentQuality::high
    ) {
        auto effective = plan;
        effective.quality = RawDevelopmentQuality::balanced;
        if (capabilities_.supports(effective)) {
            negotiation.effective = effective;
            negotiation.status =
                RawDevelopmentPlanNegotiationStatus::adjusted;
            negotiation.unresolved = RawDevelopmentPlanAspect::none;
        }
    }
    return negotiation;
}

PixelBuffer LibRawReferenceDeveloper::render(
    const RawDevelopmentPlan& plan
) const {
    const auto negotiation = require_accepted_plan(plan, false);
    return render_impl(false, plan, negotiation);
}

PixelBuffer LibRawReferenceDeveloper::render_preview(
    const std::uint32_t max_edge,
    const RawDevelopmentPlan& plan
) const {
    if (max_edge == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            LIBRAW_BAD_CROP,
            "preview reference edge must be non-zero"
        );
    }
    const std::uint64_t native_edge = std::max(
        static_cast<std::uint64_t>(source_dimensions_.width),
        static_cast<std::uint64_t>(source_dimensions_.height)
    );
    const bool use_half_size =
        native_edge > static_cast<std::uint64_t>(max_edge) * 2U;
    const auto negotiation = require_accepted_plan(plan, true);
    return render_impl(
        use_half_size,
        plan,
        negotiation,
        max_edge
    );
}

RawDevelopmentPlanNegotiation
LibRawReferenceDeveloper::require_accepted_plan(
    const RawDevelopmentPlan& plan,
    const bool preview_render
) const {
    const auto negotiation = negotiate(plan);
    if (!negotiation.accepted()) {
        const auto error_code = raw_development_plan_aspect_contains(
            negotiation.unresolved,
            RawDevelopmentPlanAspect::schema
        )
            ? DecodeErrorCode::invalid_request
            : DecodeErrorCode::unsupported;
        throw DecodeError(
            error_code,
            LIBRAW_NOT_IMPLEMENTED,
            "LibRaw cannot satisfy the requested RAW development plan"
        );
    }
    if (
        preview_render
            != (negotiation.effective.intent
                == RawDevelopmentIntent::preview)
    ) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            LIBRAW_BAD_CROP,
            preview_render
                ? "preview rendering requires a preview RAW development plan"
                : "full RAW rendering requires a detail or export development plan"
        );
    }
    return negotiation;
}

PixelBuffer LibRawReferenceDeveloper::render_impl(
    const bool half_size,
    const RawDevelopmentPlan& requested_plan,
    const RawDevelopmentPlanNegotiation& negotiation,
    const std::optional<std::uint32_t> preview_max_edge
) const {
    require_available();
    auto renderer = std::make_unique<LibRaw>();
    renderer->imgdata.rawparams.max_raw_memory_mb = 2'048U;
    configure_reference_render_parameters(*renderer, settings_, half_size);
    require_libraw_success(
        libraw_open_path(*renderer, path_),
        "reference open_file"
    );
    require_libraw_success(renderer->unpack(), "reference unpack");

    const Dimensions declared_image_dimensions{
        renderer->imgdata.sizes.width,
        renderer->imgdata.sizes.height,
    };
    const std::int32_t declared_orientation = renderer->imgdata.sizes.flip;
    const PendingCorrections declared_dng_opcode_lists =
        libraw_pending_corrections(renderer->imgdata);
    configure_reference_render_parameters(*renderer, settings_, half_size);
    auto& parameters = renderer->imgdata.params;
    require_libraw_success(renderer->dcraw_process(), "dcraw_process");

    int result = LIBRAW_SUCCESS;
    ProcessedImage image(
        renderer->dcraw_make_mem_image(&result),
        &LibRaw::dcraw_clear_mem
    );
    if (!image)
        throw_libraw_error(result, "dcraw_make_mem_image");
    if (
        image->type != LIBRAW_IMAGE_BITMAP
        || image->bits != 16U
        || (image->colors != 1U && image->colors != 3U)
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            LIBRAW_NOT_IMPLEMENTED,
            "reference renderer returned an unsupported pixel layout"
        );
    }

    const std::size_t width = image->width;
    const std::size_t height = image->height;
    const std::size_t channels = image->colors;
    if (
        height != 0U
        && width > std::numeric_limits<std::size_t>::max() / height
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            LIBRAW_TOO_BIG,
            "reference image dimensions overflow the address space"
        );
    }
    const std::size_t pixel_count = width * height;
    if (
        channels != 0U
        && pixel_count
            > std::numeric_limits<std::size_t>::max() / channels
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            LIBRAW_TOO_BIG,
            "reference image channel count overflows the address space"
        );
    }
    const std::size_t sample_count = pixel_count * channels;
    if (
        sample_count
            > std::numeric_limits<std::size_t>::max()
                / sizeof(std::uint16_t)
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            LIBRAW_TOO_BIG,
            "reference image byte count overflows the address space"
        );
    }
    const std::size_t byte_count =
        sample_count * sizeof(std::uint16_t);
    if (image->data_size < byte_count) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            LIBRAW_DATA_ERROR,
            "reference image buffer is shorter than its descriptor"
        );
    }

    PixelBuffer buffer;
    buffer.dimensions = Dimensions{image->width, image->height};
    buffer.bits_per_channel = image->bits;
    buffer.channels = image->colors;
    buffer.row_stride_bytes =
        width * channels * sizeof(std::uint16_t);
    buffer.primaries = RgbPrimaries::srgb_rec709_d65;
    buffer.transfer_function = RgbTransferFunction::linear;
    buffer.reference = RgbBufferReference::processed_raw;
    buffer.samples.resize(sample_count);
    std::memcpy(buffer.samples.data(), image->data, byte_count);
    buffer.raw_development_receipt = RawDevelopmentReceipt{
        .schema_version = raw_development_receipt_schema_version,
        .provider_id = provider_info_.id,
        .provider_version = provider_info_.version,
        .library_version = std::string(LibRaw::version()),
        .development_settings_signature =
            libraw_development_settings_signature(settings_),
        .requested_plan_identity =
            raw_development_plan_identity(requested_plan),
        .effective_plan_identity =
            raw_development_plan_identity(negotiation.effective),
        .requested_plan = requested_plan,
        .effective_plan = negotiation.effective,
        .plan_negotiation_status = negotiation.status,
        .processed_linear_reference_contract_version =
            processed_linear_reference_rgb_contract_version,
        .declared_image_dimensions = declared_image_dimensions,
        .rendered_dimensions = buffer.dimensions,
        .orientation = declared_orientation,
        .half_size = half_size,
        .use_camera_white_balance = parameters.use_camera_wb != 0,
        .use_camera_matrix = parameters.use_camera_matrix != 0,
        .use_auto_brightness = parameters.no_auto_bright == 0,
        .use_exposure_correction = parameters.exp_correc != 0,
        .brightness = parameters.bright,
        .maximum_adjustment_threshold =
            parameters.adjust_maximum_thr,
        .output_bits_per_channel =
            static_cast<std::uint16_t>(parameters.output_bps),
        .demosaic_quality = parameters.user_qual,
        .output_color = parameters.output_color,
        .gamma_inverse_power = parameters.gamm[0],
        .gamma_linear_toe_slope = parameters.gamm[1],
        .declared_dng_opcode_lists = declared_dng_opcode_lists,
        .dng_opcode_execution = dng_opcode_execution(
            declared_dng_opcode_lists,
            negotiation.effective.dng_opcode_policy
        ),
        .process_warnings = renderer->imgdata.process_warnings,
    };
    return preview_max_edge.has_value()
        ? downsample_linear_reference_for_preview(
            std::move(buffer),
            *preview_max_edge
        )
        : buffer;
}

void LibRawReferenceDeveloper::require_available() const {
    if (capabilities_.available)
        return;
    if (
        nikon_nef_compression_
            == nikon_nef_high_efficiency_compression
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            LIBRAW_NOT_IMPLEMENTED,
            "Nikon NEF High Efficiency compression requires an external decoder provider"
        );
    }
    if (
        nikon_nef_compression_
            == nikon_nef_high_efficiency_star_compression
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported,
            LIBRAW_NOT_IMPLEMENTED,
            "Nikon NEF High Efficiency* compression requires an external decoder provider"
        );
    }
    throw DecodeError(
        DecodeErrorCode::unsupported,
        LIBRAW_NOT_IMPLEMENTED,
        "LibRaw cannot develop this RAW source"
    );
}

LibRawDevelopmentSettings default_libraw_development_settings() noexcept {
    return LibRawDevelopmentSettings{
        .schema_version = libraw_development_settings_schema_version,
        .use_camera_white_balance = true,
        .use_camera_matrix = true,
        .use_auto_brightness = false,
        .use_exposure_correction = false,
        .brightness = 1.0F,
        .maximum_adjustment_threshold =
            processed_linear_reference_maximum_adjustment_threshold,
        .output_bits_per_channel = 16U,
        .demosaic_quality = 3,
    };
}

std::string libraw_development_settings_signature(
    const LibRawDevelopmentSettings& settings
) {
    std::ostringstream signature;
    signature
        << "shadow-libraw-develop-v" << settings.schema_version
        << ";wb="
        << (settings.use_camera_white_balance ? "camera" : "none")
        << ";matrix="
        << (settings.use_camera_matrix ? "camera" : "none")
        << ";auto-bright="
        << (settings.use_auto_brightness ? "on" : "off")
        << ";exposure="
        << (settings.use_exposure_correction ? "on" : "off")
        << ";bright=" << settings.brightness
        << ";max-adjust=" << settings.maximum_adjustment_threshold
        << ";bps=" << settings.output_bits_per_channel
        << ";qual=" << settings.demosaic_quality;
    return signature.str();
}

} // namespace shadow::image
