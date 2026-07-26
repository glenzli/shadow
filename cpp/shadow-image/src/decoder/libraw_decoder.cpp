#include <shadow/image/decoder.hpp>

#include <libraw/libraw.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace shadow::image {

namespace {

using ProcessedImage = std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)>;
inline constexpr std::uint32_t libraw_capability_contract_version = 2U;
// This version covers the display-orientation semantics of cached embedded-preview descriptors.
// It is deliberately separate from the raw-frame and rendered-RGB contracts: the JPEG bytes do
// not change, but their catalog geometry must match the auto-oriented image that Qt presents.
inline constexpr std::uint32_t libraw_embedded_preview_geometry_contract_version = 1U;
inline constexpr int libraw_reference_output_color = 1;
inline constexpr double libraw_reference_gamma_inverse_power = 1.0;
inline constexpr double libraw_reference_gamma_linear_toe_slope = 1.0;
// LibRaw parses Nikon's MakerNote compression tag even when its public decoder cannot safely
// develop that bitstream.  Values 13 and 14 are Nikon High Efficiency and High Efficiency*,
// respectively (see LibRaw's libraw_makernotes_t documentation).  Treating either as a normal
// unpackable Bayer source is unsafe: current public LibRaw builds can advertise a decoder and
// then fail, or crash, only after the host has entered the RAW-development path.
inline constexpr std::uint16_t nikon_nef_high_efficiency_compression = 13U;
inline constexpr std::uint16_t nikon_nef_high_efficiency_star_compression = 14U;

[[nodiscard]] bool libraw_nef_compression_requires_external_provider(
    const LibRaw& decoder
) noexcept {
    const std::uint16_t compression = decoder.imgdata.makernotes.nikon.NEFCompression;
    return compression == nikon_nef_high_efficiency_compression
        || compression == nikon_nef_high_efficiency_star_compression;
}

// The public LibRaw processed-RGB developer is stable on the Z9 lossless NEF fixtures, while
// Shadow's new owned RawFrame path is not yet safe for that exact source family.  Advertising an
// owned frame would route the host into the sensor-domain developer before it has a chance to
// choose LibRaw's proven processed compatibility path. Keep reference-RGB editing available and
// make only the unverified RawFrame capability unavailable until that developer has a dedicated
// Z9 calibration/layout validation suite.
[[nodiscard]] bool libraw_raw_frame_is_temporarily_unsafe(
    const LibRaw& decoder
) noexcept {
    const auto& identity = decoder.imgdata.idata;
    return decoder.imgdata.makernotes.nikon.NEFCompression == 3U
        && std::string_view(identity.normalized_make) == "Nikon"
        && std::string_view(identity.normalized_model) == "Z 9";
}

[[nodiscard]] RawDevelopmentCapabilities libraw_raw_development_capabilities() noexcept {
    RawDevelopmentCapabilities capabilities;
    capabilities.schema_version = raw_development_capabilities_schema_version;
    capabilities.available = true;
    // The caller sets this from the session's exact unpack capability. Keeping it here false
    // avoids claiming a frame merely because this build of LibRaw understands the plan contract.
    capabilities.raw_frame = false;
    // LibRaw cannot tell Shadow which individual DNG opcode it applied. The receipt still
    // preserves declared lists and marks them `provider_default`, but this capability stays
    // false until a provider can distinguish actual apply/defer/skip outcomes.
    capabilities.dng_opcode_execution_receipt = false;
    capabilities.supported_intents = raw_development_intent_mask(RawDevelopmentIntent::preview)
        | raw_development_intent_mask(RawDevelopmentIntent::detail)
        | raw_development_intent_mask(RawDevelopmentIntent::export_image);
    // Existing preview speed comes from the separately recorded half-size render path, not a
    // hidden claim that LibRaw's quality selector maps to Shadow's Bayer implementation tiers.
    capabilities.supported_qualities = raw_development_quality_mask(
        RawDevelopmentQuality::balanced
    );
    capabilities.supported_dng_opcode_policies = dng_opcode_policy_mask(
        DngOpcodePolicy::provider_default
    );
    capabilities.supported_noise_reduction_intents = raw_noise_reduction_intent_mask(
        RawNoiseReductionIntent::provider_default
    );
    capabilities.supported_highlight_recovery_intents = raw_highlight_recovery_intent_mask(
        RawHighlightRecoveryIntent::provider_default
    );
    return capabilities;
}

[[nodiscard]] std::array<DngOpcodeExecutionStatus, 3U> dng_opcode_execution(
    const PendingCorrections& declared,
    const DngOpcodePolicy policy
) noexcept {
    std::array<DngOpcodeExecutionStatus, 3U> result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        if (declared.dng_opcode_list_bytes[index] == 0U) {
            result[index] = DngOpcodeExecutionStatus::not_declared;
            continue;
        }
        // The capability negotiation has already rejected policies that LibRaw cannot make
        // auditable. This branch is deliberately explicit rather than quietly upgrading a
        // provider-default list to `applied` based on its presence.
        result[index] = policy == DngOpcodePolicy::provider_default
            ? DngOpcodeExecutionStatus::provider_default
            : DngOpcodeExecutionStatus::unsupported;
    }
    return result;
}

// This is intentionally distinct from the verbose receipt signature below. Provider versions
// become part of catalog ContentIdentity, whose text fields are capped at 128 bytes. Preserve
// every output-affecting LibRaw setting in a compact, reversible form rather than letting an
// explanatory sentence make a valid cache identity impossible to persist.
[[nodiscard]] std::string compact_libraw_development_settings_identity(
    const LibRawDevelopmentSettings& settings
) {
    std::ostringstream identity;
    identity << "s" << settings.schema_version
             << "-w" << (settings.use_camera_white_balance ? 1 : 0)
             << "-m" << (settings.use_camera_matrix ? 1 : 0)
             << "-a" << (settings.use_auto_brightness ? 1 : 0)
             << "-e" << (settings.use_exposure_correction ? 1 : 0)
             << "-b" << std::hex << std::bit_cast<std::uint32_t>(settings.brightness)
             << "-x" << std::bit_cast<std::uint32_t>(settings.maximum_adjustment_threshold)
             << std::dec << "-p" << settings.output_bits_per_channel
             << "-q" << settings.demosaic_quality;
    return identity.str();
}

void configure_reference_render_parameters(
    LibRaw& renderer,
    const LibRawDevelopmentSettings& settings,
    const bool half_size
) {
    auto& parameters = renderer.imgdata.params;
    // These requests are deliberately installed before open/unpack. LibRaw documents that
    // camera WB/matrix and half-size can affect earlier decode stages for some formats, so doing
    // it only before dcraw_process would make the receipt claim more than the decoder guaranteed.
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
    parameters.user_qual = static_cast<int>(settings.demosaic_quality);
    parameters.half_size = half_size ? 1 : 0;
}

// LibRaw offers a real half-size demosaic but no arbitrary preview edge. Its
// half-size result can still be several thousand pixels wide on modern RAWs.
// Enforce Shadow's provider preview contract here, before the result crosses
// into the editor or catalog cache, so callers never retain an oversized RGB
// buffer merely because a camera exceeds the half-size threshold.
[[nodiscard]] PixelBuffer downsample_linear_reference_for_preview(
    PixelBuffer source,
    const std::uint32_t max_edge
) {
    const Dimensions target = proxy_dimensions(source.dimensions, max_edge);
    if (source.dimensions == target) {
        return source;
    }
    if (
        source.bits_per_channel != 16U || source.channels == 0U
        || source.row_stride_bytes
            != static_cast<std::size_t>(source.dimensions.width) * source.channels
                * sizeof(std::uint16_t)
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            LIBRAW_NOT_IMPLEMENTED,
            "LibRaw preview downsample requires contiguous 16-bit reference samples"
        );
    }

    // `source` already owns the large LibRaw render. Do not copy its vector before
    // shrinking it: catalog indexing and the editor can otherwise briefly retain two
    // full-resolution RGB buffers per request. Carry only its small descriptive
    // fields into the bounded preview allocation.
    PixelBuffer result;
    result.dimensions = target;
    result.bits_per_channel = source.bits_per_channel;
    result.channels = source.channels;
    result.primaries = source.primaries;
    result.transfer_function = source.transfer_function;
    result.reference = source.reference;
    result.raw_development_receipt = source.raw_development_receipt;
    result.row_stride_bytes = static_cast<std::size_t>(target.width) * source.channels
        * sizeof(std::uint16_t);
    result.samples.resize(static_cast<std::size_t>(target.width) * target.height * source.channels);
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
        for (std::uint32_t output_x = 0U; output_x < target.width; ++output_x) {
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
            for (std::size_t channel = 0U; channel < source.channels; ++channel) {
                const auto sample = [&source, channel](const std::size_t x, const std::size_t y) {
                    return static_cast<double>(source.samples[
                        (y * source.dimensions.width + x) * source.channels + channel
                    ]);
                };
                const double top = sample(x0, y0) * (1.0 - fx) + sample(x1, y0) * fx;
                const double bottom = sample(x0, y1) * (1.0 - fx) + sample(x1, y1) * fx;
                result.samples[
                    (static_cast<std::size_t>(output_y) * target.width + output_x)
                        * source.channels + channel
                ] = static_cast<std::uint16_t>(std::lround(
                    std::clamp(top * (1.0 - fy) + bottom * fy, 0.0, 65'535.0)
                ));
            }
        }
    }
    result.raw_development_receipt.rendered_dimensions = target;
    return result;
}

[[nodiscard]] DecodeErrorCode map_libraw_error(const int result) noexcept {
    switch (result) {
    case LIBRAW_FILE_UNSUPPORTED:
    case LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE:
        return DecodeErrorCode::unsupported;
    case LIBRAW_IO_ERROR:
    case LIBRAW_INPUT_CLOSED:
        return DecodeErrorCode::io;
    case LIBRAW_DATA_ERROR:
        return DecodeErrorCode::corrupt_data;
    case LIBRAW_NO_THUMBNAIL:
    case LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL:
        return DecodeErrorCode::no_preview;
    case LIBRAW_UNSUPPORTED_THUMBNAIL:
    case LIBRAW_NOT_IMPLEMENTED:
        return DecodeErrorCode::unsupported_layout;
    case LIBRAW_TOO_BIG:
    case LIBRAW_UNSUFFICIENT_MEMORY:
    case LIBRAW_MEMPOOL_OVERFLOW:
        return DecodeErrorCode::resource_limit;
    case LIBRAW_CANCELLED_BY_CALLBACK:
        return DecodeErrorCode::cancelled;
    case LIBRAW_OUT_OF_ORDER_CALL:
    case LIBRAW_BAD_CROP:
        return DecodeErrorCode::invalid_request;
    default:
        return DecodeErrorCode::internal;
    }
}

[[noreturn]] void throw_libraw_error(const int result, const std::string_view operation) {
    std::ostringstream message;
    message << operation << " failed: " << libraw_strerror(result) << " (" << result << ')';
    throw DecodeError(map_libraw_error(result), result, message.str());
}

void require_libraw_success(const int result, const std::string_view operation) {
    if (result != LIBRAW_SUCCESS) {
        throw_libraw_error(result, operation);
    }
}

[[nodiscard]] int open_path(LibRaw& decoder, const std::filesystem::path& path) {
#if defined(_WIN32)
    return decoder.open_file(path.c_str());
#else
    const std::string native_path = path.native();
    return decoder.open_file(native_path.c_str());
#endif
}

[[nodiscard]] std::string dng_version_string(const unsigned version) {
    if (version == 0U) {
        return {};
    }

    std::ostringstream output;
    output << ((version >> 24U) & 0xffU) << '.' << ((version >> 16U) & 0xffU) << '.'
           << ((version >> 8U) & 0xffU) << '.' << (version & 0xffU);
    return output.str();
}

[[nodiscard]] std::string cfa_pattern(LibRaw& decoder) {
    if (decoder.imgdata.idata.filters == 0U) {
        return "none/linear";
    }

    std::string result;
    result.reserve(4);
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const int color_index = decoder.COLOR(row, column);
            const bool valid_index = color_index >= 0 && color_index < 4;
            result.push_back(valid_index ? decoder.imgdata.idata.cdesc[color_index] : '?');
        }
    }
    return result;
}

[[nodiscard]] RawCfaColor raw_cfa_color(LibRaw& decoder, const int row, const int column) noexcept {
    const int color_index = decoder.COLOR(row, column);
    if (color_index < 0 || color_index >= 4) {
        return RawCfaColor::unknown;
    }
    switch (decoder.imgdata.idata.cdesc[color_index]) {
    case 'R':
    case 'r':
        return RawCfaColor::red;
    case 'G':
    case 'g':
        return RawCfaColor::green;
    case 'B':
    case 'b':
        return RawCfaColor::blue;
    default:
        return RawCfaColor::unknown;
    }
}

[[nodiscard]] std::array<RawCfaColor, 4U> raw_frame_bayer_2x2(LibRaw& decoder) noexcept {
    return {
        raw_cfa_color(decoder, 0, 0),
        raw_cfa_color(decoder, 0, 1),
        raw_cfa_color(decoder, 1, 0),
        raw_cfa_color(decoder, 1, 1),
    };
}

[[nodiscard]] RawFrameCfaLayout raw_frame_cfa_layout(
    LibRaw& decoder,
    const std::array<RawCfaColor, 4U>& bayer_2x2
) noexcept {
    if (decoder.imgdata.idata.filters == 0U) {
        return RawFrameCfaLayout::monochrome;
    }
    // LibRaw uses this marker for X-Trans. Its first 2x2 cells must not be mistaken for a
    // Bayer repeat, even if their colours happen to contain one R, two G and one B.
    if (decoder.imgdata.idata.filters == 9U) {
        return RawFrameCfaLayout::unknown;
    }
    std::size_t red = 0U;
    std::size_t green = 0U;
    std::size_t blue = 0U;
    for (const auto color : bayer_2x2) {
        red += color == RawCfaColor::red ? 1U : 0U;
        green += color == RawCfaColor::green ? 1U : 0U;
        blue += color == RawCfaColor::blue ? 1U : 0U;
    }
    return red == 1U && green == 2U && blue == 1U
        ? RawFrameCfaLayout::bayer_2x2
        : RawFrameCfaLayout::unknown;
}

[[nodiscard]] std::array<int, 4U> raw_frame_color_indices(
    LibRaw& decoder,
    const RawFrameCfaLayout layout
) noexcept {
    if (layout == RawFrameCfaLayout::monochrome) {
        return {0, 0, 0, 0};
    }
    return {
        decoder.COLOR(0, 0),
        decoder.COLOR(0, 1),
        decoder.COLOR(1, 0),
        decoder.COLOR(1, 1),
    };
}

[[nodiscard]] bool valid_color_index(const int index) noexcept {
    return index >= 0 && index < 4;
}

[[nodiscard]] constexpr std::uint32_t combined_black_level(
    const std::uint32_t common,
    const std::uint32_t correction
) noexcept {
    return correction > std::numeric_limits<std::uint32_t>::max() - common
        ? std::numeric_limits<std::uint32_t>::max()
        : common + correction;
}

static_assert(combined_black_level(255U, 1U) == 256U);
static_assert(
    combined_black_level(std::numeric_limits<std::uint32_t>::max(), 1U)
    == std::numeric_limits<std::uint32_t>::max()
);

[[nodiscard]] std::uint32_t raw_frame_black_level(
    const libraw_colordata_t& color,
    const int color_index
) noexcept {
    const auto index = static_cast<std::size_t>(color_index);
    // LibRaw defines `black` as the common sensor floor and `cblack[0..3]` as
    // per-channel corrections to that floor. They are additive, not competing
    // alternatives. Treating a small correction (for example Canon's 1 DN)
    // as the complete black level lifts that CFA channel by hundreds of DN
    // and turns clipped/high-key regions pink after white balance.
    return combined_black_level(color.black, color.cblack[index]);
}

[[nodiscard]] std::uint32_t raw_frame_white_level(
    const libraw_colordata_t& color,
    const int color_index
) noexcept {
    const auto channel_maximum = color.linear_max[static_cast<std::size_t>(color_index)];
    const auto black_level = raw_frame_black_level(color, color_index);
    return channel_maximum > black_level ? channel_maximum : color.maximum;
}

[[nodiscard]] bool positive_finite(const double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] double camera_white_balance_multiplier(
    const libraw_data_t& data,
    const int requested_color_index
) noexcept {
    const auto requested = static_cast<std::size_t>(requested_color_index);
    if (positive_finite(data.color.cam_mul[requested])) {
        return data.color.cam_mul[requested];
    }

    // Some three-colour RAWs describe their CFA as RGBG but carry only one green multiplier.
    // Resolve the second green from that equivalent colour component rather than treating the
    // absent fourth coefficient as a neutral value.
    const char requested_color = data.idata.cdesc[requested];
    for (std::size_t candidate = 0U; candidate < 4U; ++candidate) {
        if (
            data.idata.cdesc[candidate] == requested_color
            && positive_finite(data.color.cam_mul[candidate])
        ) {
            return data.color.cam_mul[candidate];
        }
    }
    return 0.0;
}

[[nodiscard]] std::array<double, 4U> raw_frame_as_shot_neutral(
    const libraw_data_t& data,
    const RawFrameCfaLayout layout,
    const std::array<RawCfaColor, 4U>& bayer_2x2,
    const std::array<int, 4U>& color_indices
) {
    if (layout == RawFrameCfaLayout::monochrome) {
        return {1.0, 1.0, 1.0, 1.0};
    }

    std::array<double, 4U> neutral{};
    bool embedded_is_valid = true;
    for (std::size_t site = 0U; site < neutral.size(); ++site) {
        const auto color_index = static_cast<std::size_t>(color_indices[site]);
        neutral[site] = data.color.dng_levels.asshotneutral[color_index];
        embedded_is_valid = embedded_is_valid && positive_finite(neutral[site]);
    }
    if (embedded_is_valid) {
        return neutral;
    }

    // DNG AsShotNeutral is the inverse of the camera-space WB multipliers up to a common scale.
    // Normalize that scale to the mean of the two green CFA sites, matching the conventional
    // neutral representation consumed by Shadow's future RAW white-balance stage.
    for (std::size_t site = 0U; site < neutral.size(); ++site) {
        const double multiplier = camera_white_balance_multiplier(data, color_indices[site]);
        if (!positive_finite(multiplier)) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_NOT_IMPLEMENTED,
                "LibRaw did not expose a usable AsShotNeutral or camera white balance"
            );
        }
        neutral[site] = 1.0 / multiplier;
    }

    double green_neutral_sum = 0.0;
    std::size_t green_site_count = 0U;
    for (std::size_t site = 0U; site < neutral.size(); ++site) {
        if (bayer_2x2[site] == RawCfaColor::green) {
            green_neutral_sum += neutral[site];
            ++green_site_count;
        }
    }
    const double normalization = green_site_count == 0U
        ? neutral.front()
        : green_neutral_sum / static_cast<double>(green_site_count);
    if (!positive_finite(normalization)) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            LIBRAW_NOT_IMPLEMENTED,
            "LibRaw camera white balance cannot be normalized"
        );
    }
    for (auto& value : neutral) {
        value /= normalization;
    }
    return neutral;
}

[[nodiscard]] std::optional<std::array<double, 9U>> camera_to_linear_srgb_d65(
    const libraw_data_t& data,
    const RawFrameCfaLayout layout,
    const std::array<int, 4U>& color_indices
) noexcept {
    if (layout != RawFrameCfaLayout::bayer_2x2) {
        return std::nullopt;
    }

    std::array<bool, 4U> used_color_indices{};
    for (const int color_index : color_indices) {
        if (!valid_color_index(color_index)) {
            return std::nullopt;
        }
        used_color_indices[static_cast<std::size_t>(color_index)] = true;
    }

    std::array<double, 9U> matrix{};
    std::array<bool, 3U> has_canonical_input{};
    for (std::size_t source = 0U; source < used_color_indices.size(); ++source) {
        if (!used_color_indices[source]) {
            continue;
        }
        std::size_t canonical_input = 0U;
        switch (data.idata.cdesc[source]) {
        case 'R':
        case 'r':
            canonical_input = 0U;
            break;
        case 'G':
        case 'g':
            canonical_input = 1U;
            break;
        case 'B':
        case 'b':
            canonical_input = 2U;
            break;
        default:
            return std::nullopt;
        }
        has_canonical_input[canonical_input] = true;
        for (std::size_t output = 0U; output < 3U; ++output) {
            const double coefficient = data.color.rgb_cam[output][source];
            if (!std::isfinite(coefficient)) {
                return std::nullopt;
            }
            // If LibRaw keeps two distinct green inputs, Shadow's demosaiced camera RGB has one
            // green channel, so both green coefficients contribute to that canonical column.
            matrix[output * 3U + canonical_input] += coefficient;
        }
    }

    bool has_non_zero_coefficient = false;
    for (const double value : matrix) {
        has_non_zero_coefficient = has_non_zero_coefficient || value != 0.0;
    }
    if (
        !std::ranges::all_of(has_canonical_input, [](const bool present) { return present; })
        || !has_non_zero_coefficient
    ) {
        return std::nullopt;
    }
    return matrix;
}

void validate_development_settings(const LibRawDevelopmentSettings& settings) {
    if (settings.schema_version != libraw_development_settings_schema_version) {
        throw std::invalid_argument("unsupported LibRaw development settings schema version");
    }
    if (!std::isfinite(settings.brightness) || settings.brightness <= 0.0F
        || settings.brightness > 8.0F) {
        throw std::invalid_argument("LibRaw brightness must be finite and in (0, 8]");
    }
    if (!std::isfinite(settings.maximum_adjustment_threshold)
        || settings.maximum_adjustment_threshold < 0.0F
        || settings.maximum_adjustment_threshold > 1.0F) {
        throw std::invalid_argument(
            "LibRaw maximum adjustment threshold must be finite and in [0, 1]"
        );
    }
    if (settings.output_bits_per_channel != 16U) {
        throw std::invalid_argument("Shadow's processed-linear LibRaw contract requires 16-bit output");
    }
    if (settings.demosaic_quality < 0 || settings.demosaic_quality > 13) {
        throw std::invalid_argument("LibRaw demosaic quality must be in [0, 13]");
    }
}

[[nodiscard]] PreviewFormat preview_format(const LibRaw_internal_thumbnail_formats format) noexcept {
    switch (format) {
    case LIBRAW_INTERNAL_THUMBNAIL_JPEG:
        return PreviewFormat::jpeg;
    case LIBRAW_INTERNAL_THUMBNAIL_JPEGXL:
        return PreviewFormat::jpeg_xl;
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_THUMB:
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_YCBCR:
    case LIBRAW_INTERNAL_THUMBNAIL_KODAK_RGB:
    case LIBRAW_INTERNAL_THUMBNAIL_PPM:
    case LIBRAW_INTERNAL_THUMBNAIL_PPM16:
    case LIBRAW_INTERNAL_THUMBNAIL_DNG_YCBCR:
        return PreviewFormat::bitmap;
    default:
        return PreviewFormat::unknown;
    }
}

[[nodiscard]] PreviewFormat preview_format(const LibRaw_image_formats format) noexcept {
    switch (format) {
    case LIBRAW_IMAGE_JPEG:
        return PreviewFormat::jpeg;
    case LIBRAW_IMAGE_BITMAP:
        return PreviewFormat::bitmap;
    case LIBRAW_IMAGE_JPEGXL:
        return PreviewFormat::jpeg_xl;
    case LIBRAW_IMAGE_H265:
        return PreviewFormat::h265;
    default:
        return PreviewFormat::unknown;
    }
}

[[nodiscard]] Margins image_margins(const libraw_image_sizes_t& sizes) noexcept {
    const auto used_width = static_cast<std::uint32_t>(sizes.left_margin) + sizes.width;
    const auto used_height = static_cast<std::uint32_t>(sizes.top_margin) + sizes.height;
    const auto right = sizes.raw_width > used_width ? sizes.raw_width - used_width : 0U;
    const auto bottom = sizes.raw_height > used_height ? sizes.raw_height - used_height : 0U;
    return Margins{sizes.left_margin, sizes.top_margin, right, bottom};
}

[[nodiscard]] PendingCorrections pending_corrections(const libraw_data_t& data) noexcept {
    const auto& opcodes = data.color.dng_levels.rawopcodes;
    return PendingCorrections{{opcodes[0].len, opcodes[1].len, opcodes[2].len}};
}

[[nodiscard]] AssetMetadata read_metadata(LibRaw& decoder) {
    const auto& identity = decoder.imgdata.idata;
    const auto& sizes = decoder.imgdata.sizes;
    const auto& color = decoder.imgdata.color;
    const auto& dng = color.dng_levels;
    const auto& capture = decoder.imgdata.other;
    const auto& lens = decoder.imgdata.lens;

    AssetMetadata metadata;
    metadata.make = identity.make;
    metadata.model = identity.model;
    metadata.normalized_make = identity.normalized_make;
    metadata.normalized_model = identity.normalized_model;
    metadata.dng_version = dng_version_string(identity.dng_version);
    metadata.raw_count = identity.raw_count;
    metadata.raw_dimensions = Dimensions{sizes.raw_width, sizes.raw_height};
    metadata.image_dimensions = Dimensions{sizes.width, sizes.height};
    metadata.margins = image_margins(sizes);
    metadata.orientation = sizes.flip;
    metadata.cfa_pattern = cfa_pattern(decoder);
    metadata.sensor_colors = static_cast<std::uint32_t>(identity.colors);
    metadata.sensor_bits = color.raw_bps;
    metadata.black_level = color.black;
    metadata.white_level = color.maximum;
    for (std::size_t index = 0; index < metadata.as_shot_neutral.size(); ++index) {
        metadata.as_shot_neutral[index] = dng.asshotneutral[index];
    }
    metadata.baseline_exposure = dng.baseline_exposure;
    metadata.iso_speed = capture.iso_speed;
    metadata.exposure_time_seconds = capture.shutter;
    metadata.aperture_f_number = capture.aperture;
    metadata.focal_length_mm = capture.focal_len;
    metadata.captured_at_unix_seconds = static_cast<std::int64_t>(capture.timestamp);
    metadata.lens_make = lens.LensMake;
    metadata.lens_model = lens.Lens;
    metadata.focal_length_35mm = lens.FocalLengthIn35mmFormat;
    return metadata;
}

// LibRaw's `sizes.flip` describes the output orientation rather than standard EXIF orientation
// values. Its current 5 and 6 cases transpose the displayed axes. Keep cached preview geometry
// in that display coordinate system because the Qt thumbnail reader applies the same orientation
// while loading the encoded preview bytes. Without this, an upright portrait preview receives a
// landscape justified tile and exposes empty side bars around the image.
[[nodiscard]] Dimensions display_oriented_preview_dimensions(
    Dimensions dimensions,
    const std::int32_t orientation
) noexcept {
    if (orientation == 5 || orientation == 6) {
        std::swap(dimensions.width, dimensions.height);
    }
    return dimensions;
}

[[nodiscard]] std::vector<PreviewDescriptor> read_previews(const libraw_data_t& data) {
    std::vector<PreviewDescriptor> previews;
    const int count = std::max(0, data.thumbs_list.thumbcount);
    previews.reserve(static_cast<std::size_t>(count));

    for (int index = 0; index < count; ++index) {
        const auto& candidate = data.thumbs_list.thumblist[index];
        const bool placeholder =
            candidate.tlength == 0U && candidate.twidth == 0U && candidate.theight == 0U;
        if (placeholder) {
            continue;
        }

        PreviewDescriptor descriptor;
        descriptor.id = static_cast<std::size_t>(index);
        descriptor.format = preview_format(candidate.tformat);
        descriptor.dimensions = display_oriented_preview_dimensions(
            Dimensions{candidate.twidth, candidate.theight},
            data.sizes.flip
        );
        descriptor.bits_per_channel = static_cast<std::uint16_t>(candidate.tmisc & 31U);
        descriptor.channels = static_cast<std::uint16_t>(candidate.tmisc >> 5U);
        descriptor.encoded_bytes = candidate.tlength;
        descriptor.decodable = descriptor.format != PreviewFormat::unknown;
        previews.push_back(std::move(descriptor));
    }
    return previews;
}

class LibRawSession final : public DecodeSession {
public:
    explicit LibRawSession(
        std::filesystem::path path,
        LibRawDevelopmentSettings settings,
        ProviderInfo provider_info
    )
        : path_(std::move(path)), settings_(settings), provider_info_(std::move(provider_info)) {
        decoder_.imgdata.rawparams.max_raw_memory_mb = 2'048U;
        require_libraw_success(open_path(decoder_, path_), "open_file");

        metadata_ = read_metadata(decoder_);
        previews_ = read_previews(decoder_.imgdata);
        libraw_decoder_info_t decoder_info{};
        const bool decoder_advertises_unpack =
            decoder_.get_decoder_info(&decoder_info) == LIBRAW_SUCCESS
            && (decoder_info.decoder_flags
                & (LIBRAW_DECODER_UNSUPPORTED_FORMAT | LIBRAW_DECODER_NOTSET)) == 0U;
        // Preserve factual metadata and any camera JPEG for browse mode, but do not let public
        // LibRaw enter its unsafe HE/HE* development path. The photo router still gives an
        // independently installed private provider the opportunity to claim this source after
        // seeing these public capabilities.
        const bool decoder_can_unpack = decoder_advertises_unpack
            && !libraw_nef_compression_requires_external_provider(decoder_);
        capabilities_.metadata = true;
        capabilities_.embedded_previews = !previews_.empty();
        capabilities_.raw_frame = decoder_can_unpack
            && !libraw_raw_frame_is_temporarily_unsafe(decoder_)
            && (decoder_.imgdata.idata.filters != 0U
                || decoder_.imgdata.idata.colors == 1);
        capabilities_.reference_rgb = decoder_can_unpack;
        capabilities_.pending_corrections = pending_corrections(decoder_.imgdata);
        capabilities_.raw_development = decoder_can_unpack
            ? libraw_raw_development_capabilities()
            : RawDevelopmentCapabilities{};
        capabilities_.raw_development.raw_frame = capabilities_.raw_frame;
    }

    [[nodiscard]] const AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const PreviewDescriptor> previews() const noexcept override {
        return previews_;
    }

    [[nodiscard]] const RawDevelopmentCapabilities& raw_development_capabilities() const noexcept
        override {
        return capabilities_.raw_development;
    }

    [[nodiscard]] RawDevelopmentPlanNegotiation negotiate_raw_development_plan(
        const RawDevelopmentPlan& plan
    ) const noexcept override {
        auto negotiation = shadow::image::negotiate_raw_development_plan(
            plan,
            capabilities_.raw_development
        );
        // Shadow's export preset asks providers for the high tier. LibRaw currently has one
        // honest full-resolution development path, represented by `balanced`; it must not make
        // exports unavailable merely because it cannot distinguish an additional quality tier.
        // Record the downgrade explicitly so cache identity and provenance retain both plans.
        if (
            !negotiation.accepted()
            && negotiation.unresolved == RawDevelopmentPlanAspect::quality
            && plan.quality == RawDevelopmentQuality::high
        ) {
            auto effective = plan;
            effective.quality = RawDevelopmentQuality::balanced;
            if (capabilities_.raw_development.supports(effective)) {
                negotiation.effective = effective;
                negotiation.status = RawDevelopmentPlanNegotiationStatus::adjusted;
                negotiation.unresolved = RawDevelopmentPlanAspect::none;
            }
        }
        return negotiation;
    }

    [[nodiscard]] PreviewPayload decode_preview(const std::size_t id) override {
        const auto descriptor = std::find_if(
            previews_.begin(),
            previews_.end(),
            [id](const PreviewDescriptor& candidate) { return candidate.id == id; }
        );
        if (descriptor == previews_.end() || !descriptor->decodable || id > INT_MAX) {
            throw DecodeError(
                DecodeErrorCode::no_preview,
                LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL,
                "requested preview is not available"
            );
        }

        require_libraw_success(decoder_.unpack_thumb_ex(static_cast<int>(id)), "unpack_thumb_ex");
        int result = LIBRAW_SUCCESS;
        ProcessedImage image(
            decoder_.dcraw_make_mem_thumb(&result),
            &LibRaw::dcraw_clear_mem
        );
        if (!image) {
            throw_libraw_error(result, "dcraw_make_mem_thumb");
        }

        PreviewPayload payload;
        payload.descriptor = *descriptor;
        payload.descriptor.format = preview_format(image->type);
        if (image->width != 0U && image->height != 0U) {
            payload.descriptor.dimensions = display_oriented_preview_dimensions(
                Dimensions{image->width, image->height},
                metadata_.orientation
            );
        }
        if (image->bits != 0U) {
            payload.descriptor.bits_per_channel = image->bits;
        }
        if (image->colors != 0U) {
            payload.descriptor.channels = image->colors;
        }
        payload.descriptor.encoded_bytes = image->data_size;
        payload.descriptor.decodable = payload.descriptor.format != PreviewFormat::unknown;
        payload.byte_order =
            payload.descriptor.format == PreviewFormat::bitmap && image->bits == 16U
                ? ByteOrder::native
                : ByteOrder::not_applicable;
        payload.bytes.assign(image->data, image->data + image->data_size);
        return payload;
    }

    [[nodiscard]] RawFrame decode_raw_frame() override {
        require_raw_frame();
        ensure_unpacked();
        const auto& sizes = decoder_.imgdata.sizes;
        const auto* raw_image = decoder_.imgdata.rawdata.raw_image;
        if (raw_image == nullptr) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_NOT_IMPLEMENTED,
                "decoder did not return a single-plane integer RAW frame"
            );
        }

        const std::size_t width = sizes.raw_width;
        const std::size_t height = sizes.raw_height;
        if (height != 0U && width > std::numeric_limits<std::size_t>::max() / height) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                LIBRAW_TOO_BIG,
                "RAW frame dimensions overflow the address space"
            );
        }

        const std::size_t source_stride =
            sizes.raw_pitch == 0U ? width : sizes.raw_pitch / sizeof(std::uint16_t);
        if (source_stride < width || sizes.raw_pitch % sizeof(std::uint16_t) != 0U) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_DATA_ERROR,
                "RAW frame row stride is invalid"
            );
        }

        RawFrame frame;
        auto& descriptor = frame.descriptor;
        descriptor.schema_version = raw_frame_schema_version;
        descriptor.provider_id = provider_info_.id;
        descriptor.provider_version = provider_info_.version;
        descriptor.storage_dimensions = metadata_.raw_dimensions;
        descriptor.active_dimensions = metadata_.image_dimensions;
        descriptor.active_margins = metadata_.margins;
        descriptor.orientation = metadata_.orientation;
        descriptor.sample_encoding = RawFrameSampleEncoding::uint16_native;
        descriptor.bayer_2x2 = raw_frame_bayer_2x2(decoder_);
        descriptor.cfa_layout = raw_frame_cfa_layout(decoder_, descriptor.bayer_2x2);
        descriptor.cfa_pattern = metadata_.cfa_pattern;
        descriptor.bits_per_sample = metadata_.sensor_bits;
        const auto color_indices = raw_frame_color_indices(decoder_, descriptor.cfa_layout);
        if (
            std::ranges::any_of(
                color_indices,
                [](const int color_index) { return !valid_color_index(color_index); }
            )
        ) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_NOT_IMPLEMENTED,
                "LibRaw returned an invalid CFA colour index"
            );
        }
        const auto& color = decoder_.imgdata.color;
        for (std::size_t site = 0U; site < descriptor.black_levels.size(); ++site) {
            descriptor.black_levels[site] = raw_frame_black_level(color, color_indices[site]);
            descriptor.white_levels[site] = raw_frame_white_level(color, color_indices[site]);
        }
        descriptor.as_shot_neutral = raw_frame_as_shot_neutral(
            decoder_.imgdata,
            descriptor.cfa_layout,
            descriptor.bayer_2x2,
            color_indices
        );
        if (const auto matrix = camera_to_linear_srgb_d65(
                decoder_.imgdata,
                descriptor.cfa_layout,
                color_indices
            )) {
            descriptor.camera_to_linear_srgb_d65 = *matrix;
            descriptor.has_camera_to_linear_srgb_d65 = true;
        }
        descriptor.declared_pending_corrections = capabilities_.pending_corrections;
        frame.samples.resize(width * height);

        for (std::size_t row = 0; row < height; ++row) {
            const auto* source = raw_image + (row * source_stride);
            auto* destination = frame.samples.data() + (row * width);
            std::copy_n(source, width, destination);
        }
        if (!frame.valid()) {
            throw DecodeError(
                DecodeErrorCode::unsupported_layout,
                LIBRAW_NOT_IMPLEMENTED,
                "decoder returned an invalid provider-neutral RAW frame"
            );
        }
        return frame;
    }

    [[nodiscard]] PixelBuffer render_reference_rgb() const override {
        return render_reference_rgb(default_raw_development_plan());
    }

    [[nodiscard]] PixelBuffer render_reference_rgb(
        const RawDevelopmentPlan& plan
    ) const override {
        const auto negotiation = require_accepted_plan(plan, false);
        return render_reference_rgb_impl(false, plan, negotiation);
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge
    ) const override {
        return render_reference_rgb_for_preview(max_edge, preview_raw_development_plan());
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge,
        const RawDevelopmentPlan& plan
    ) const override {
        if (max_edge == 0U) {
            throw DecodeError(
                DecodeErrorCode::invalid_request,
                LIBRAW_BAD_CROP,
                "preview reference edge must be non-zero"
            );
        }
        const std::uint64_t native_edge = std::max(
            static_cast<std::uint64_t>(metadata_.image_dimensions.width),
            static_cast<std::uint64_t>(metadata_.image_dimensions.height)
        );
        // LibRaw's half-size mode is a genuine reduced demosaic path, not a post-process
        // resize. It avoids spending full-resolution CPU and memory bandwidth on a source whose
        // next step is a 1200–2048 px interactive proxy. Keep full quality for smaller files and
        // for any request where a half-size raster would not materially reduce work.
        const bool use_half_size = native_edge > static_cast<std::uint64_t>(max_edge) * 2U;
        const auto negotiation = require_accepted_plan(plan, true);
        return render_reference_rgb_impl(use_half_size, plan, negotiation, max_edge);
    }

private:
    [[nodiscard]] RawDevelopmentPlanNegotiation require_accepted_plan(
        const RawDevelopmentPlan& plan,
        const bool preview_render
    ) const {
        const auto negotiation = negotiate_raw_development_plan(plan);
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
                != (negotiation.effective.intent == RawDevelopmentIntent::preview)
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

    [[nodiscard]] PixelBuffer render_reference_rgb_impl(
        const bool half_size,
        const RawDevelopmentPlan& requested_plan,
        const RawDevelopmentPlanNegotiation& negotiation,
        const std::optional<std::uint32_t> preview_max_edge = std::nullopt
    ) const {
        require_editable_raw();
        // LibRaw embeds sizeable fixed storage in the decoder object. QtConcurrent worker
        // threads use a substantially smaller stack than the process main thread on macOS,
        // so keeping a temporary LibRaw here can overflow the worker before open_file runs.
        // The renderer is independent state and belongs on the heap regardless of caller.
        auto renderer = std::make_unique<LibRaw>();
        renderer->imgdata.rawparams.max_raw_memory_mb = 2'048U;
        configure_reference_render_parameters(*renderer, settings_, half_size);
        require_libraw_success(open_path(*renderer, path_), "reference open_file");
        require_libraw_success(renderer->unpack(), "reference unpack");

        // LibRaw may update `sizes.width` and `sizes.height` while producing its output bitmap.
        // Snapshot the source declaration before `dcraw_process()` so a half-size receipt can
        // distinguish its original image geometry from the rendered raster.
        const Dimensions declared_image_dimensions{
            renderer->imgdata.sizes.width,
            renderer->imgdata.sizes.height,
        };
        const std::int32_t declared_orientation = renderer->imgdata.sizes.flip;
        const PendingCorrections declared_dng_opcode_lists = pending_corrections(renderer->imgdata);
        // Re-assert the same request after unpack as a defensive guard against a LibRaw build
        // that initializes a postprocess default while loading metadata. The pre-open call above
        // remains the important one for formats where these switches influence loading itself.
        configure_reference_render_parameters(*renderer, settings_, half_size);
        auto& parameters = renderer->imgdata.params;
        require_libraw_success(renderer->dcraw_process(), "dcraw_process");

        int result = LIBRAW_SUCCESS;
        ProcessedImage image(
            renderer->dcraw_make_mem_image(&result),
            &LibRaw::dcraw_clear_mem
        );
        if (!image) {
            throw_libraw_error(result, "dcraw_make_mem_image");
        }
        if (
            image->type != LIBRAW_IMAGE_BITMAP || image->bits != 16U
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
        const bool pixel_count_overflows =
            height != 0U && width > std::numeric_limits<std::size_t>::max() / height;
        if (pixel_count_overflows) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                LIBRAW_TOO_BIG,
                "reference image dimensions overflow the address space"
            );
        }

        const std::size_t pixel_count = width * height;
        if (
            channels != 0U
            && pixel_count > std::numeric_limits<std::size_t>::max() / channels
        ) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                LIBRAW_TOO_BIG,
                "reference image channel count overflows the address space"
            );
        }

        const std::size_t sample_count = pixel_count * channels;
        if (sample_count > std::numeric_limits<std::size_t>::max() / sizeof(std::uint16_t)) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                LIBRAW_TOO_BIG,
                "reference image byte count overflows the address space"
            );
        }
        const std::size_t byte_count = sample_count * sizeof(std::uint16_t);
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
        buffer.row_stride_bytes = width * channels * sizeof(std::uint16_t);
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
            .development_settings_signature = libraw_development_settings_signature(settings_),
            .requested_plan_identity = raw_development_plan_identity(requested_plan),
            .effective_plan_identity = raw_development_plan_identity(negotiation.effective),
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
            .maximum_adjustment_threshold = parameters.adjust_maximum_thr,
            .output_bits_per_channel = static_cast<std::uint16_t>(parameters.output_bps),
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
            ? downsample_linear_reference_for_preview(std::move(buffer), *preview_max_edge)
            : buffer;
    }

    void ensure_unpacked() {
        require_raw_frame();
        if (unpacked_) {
            return;
        }
        require_libraw_success(decoder_.unpack(), "unpack");
        unpacked_ = true;
    }

    void require_editable_raw() const {
        if (capabilities_.reference_rgb) {
            return;
        }
        const std::uint16_t compression = decoder_.imgdata.makernotes.nikon.NEFCompression;
        if (compression == nikon_nef_high_efficiency_compression) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                LIBRAW_NOT_IMPLEMENTED,
                "Nikon NEF High Efficiency compression requires an external decoder provider"
            );
        }
        if (compression == nikon_nef_high_efficiency_star_compression) {
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

    void require_raw_frame() const {
        if (capabilities_.raw_frame) {
            return;
        }
        if (capabilities_.reference_rgb) {
            throw DecodeError(
                DecodeErrorCode::unsupported,
                LIBRAW_NOT_IMPLEMENTED,
                "this LibRaw source uses the processed-RGB compatibility path instead of Shadow RawFrame"
            );
        }
        require_editable_raw();
    }

    std::filesystem::path path_;
    LibRawDevelopmentSettings settings_;
    ProviderInfo provider_info_;
    LibRaw decoder_;
    AssetMetadata metadata_;
    DecodeCapabilities capabilities_;
    std::vector<PreviewDescriptor> previews_;
    bool unpacked_ = false;
};

class LibRawProvider final : public DecoderProvider {
public:
    explicit LibRawProvider(LibRawDevelopmentSettings settings) : settings_(settings) {
        validate_development_settings(settings_);
        const unsigned capabilities = LibRaw::capabilities();
        info_.id = "libraw";
        // Provider version participates in generated-proxy/cache identity. Include Shadow's
        // reference/output contracts so a transfer or gamut-mapping change cannot reuse bytes
        // generated under the same linked LibRaw release.
        info_.version = "libraw=" + std::string(LibRaw::version())
            + ";cap=" + std::to_string(libraw_capability_contract_version)
            + ";linear=" + std::to_string(processed_linear_reference_rgb_contract_version)
            + ";receipt=" + std::to_string(raw_development_receipt_schema_version)
            + ";plan=" + std::to_string(raw_development_plan_schema_version)
            + ";frame=" + std::to_string(raw_frame_schema_version)
            + ";preview=" + std::to_string(libraw_embedded_preview_geometry_contract_version)
            + ";display=" + std::to_string(display_srgb8_output_transform_version)
            + ";settings=" + compact_libraw_development_settings_identity(settings_);
        if (info_.version.size() > 128U) {
            throw std::invalid_argument("LibRaw provider cache identity exceeds 128 bytes");
        }
        info_.dng_sdk = (capabilities & LIBRAW_CAPS_DNGSDK) != 0U;
        info_.rawspeed =
            (capabilities & (LIBRAW_CAPS_RAWSPEED | LIBRAW_CAPS_RAWSPEED3)) != 0U;
        info_.jpeg = (capabilities & LIBRAW_CAPS_JPEG) != 0U;
    }

    [[nodiscard]] const ProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] std::unique_ptr<DecodeSession> open(
        const std::filesystem::path& path
    ) const override {
        return std::make_unique<LibRawSession>(path, settings_, info_);
    }

private:
    LibRawDevelopmentSettings settings_;
    ProviderInfo info_;
};

} // namespace

std::uint64_t Dimensions::pixel_count() const noexcept {
    return static_cast<std::uint64_t>(width) * height;
}

bool PendingCorrections::has_pending() const noexcept {
    return std::ranges::any_of(dng_opcode_list_bytes, [](const std::uint32_t size) {
        return size != 0U;
    });
}

DecodeError::DecodeError(
    const DecodeErrorCode code,
    const int provider_code,
    std::string message
)
    : std::runtime_error(std::move(message)), code_(code), provider_code_(provider_code) {}

DecodeErrorCode DecodeError::code() const noexcept {
    return code_;
}

int DecodeError::provider_code() const noexcept {
    return provider_code_;
}

LibRawDevelopmentSettings default_libraw_development_settings() noexcept {
    return LibRawDevelopmentSettings{
        .schema_version = libraw_development_settings_schema_version,
        .use_camera_white_balance = true,
        .use_camera_matrix = true,
        .use_auto_brightness = false,
        .use_exposure_correction = false,
        .brightness = 1.0F,
        .maximum_adjustment_threshold = processed_linear_reference_maximum_adjustment_threshold,
        .output_bits_per_channel = 16U,
        .demosaic_quality = 3,
    };
}

std::string libraw_development_settings_signature(const LibRawDevelopmentSettings& settings) {
    std::ostringstream signature;
    signature << "shadow-libraw-develop-v" << settings.schema_version
              << ";wb=" << (settings.use_camera_white_balance ? "camera" : "none")
              << ";matrix=" << (settings.use_camera_matrix ? "camera" : "none")
              << ";auto-bright=" << (settings.use_auto_brightness ? "on" : "off")
              << ";exposure=" << (settings.use_exposure_correction ? "on" : "off")
              << ";bright=" << settings.brightness
              << ";max-adjust=" << settings.maximum_adjustment_threshold
              << ";bps=" << settings.output_bits_per_channel
              << ";qual=" << settings.demosaic_quality;
    return signature.str();
}

std::unique_ptr<DecoderProvider> make_libraw_decoder_provider(
    const LibRawDevelopmentSettings settings
) {
    return std::make_unique<LibRawProvider>(settings);
}

std::optional<std::size_t> select_best_preview(
    const std::span<const PreviewDescriptor> previews
) noexcept {
    const PreviewDescriptor* best = nullptr;
    for (const auto& candidate : previews) {
        if (!candidate.decodable) {
            continue;
        }
        if (
            best == nullptr || candidate.dimensions.pixel_count() > best->dimensions.pixel_count()
            || (
                candidate.dimensions.pixel_count() == best->dimensions.pixel_count()
                && candidate.encoded_bytes > best->encoded_bytes
            )
        ) {
            best = &candidate;
        }
    }
    return best == nullptr ? std::nullopt : std::optional<std::size_t>{best->id};
}

std::string_view to_string(const PreviewFormat format) noexcept {
    switch (format) {
    case PreviewFormat::jpeg:
        return "jpeg";
    case PreviewFormat::bitmap:
        return "bitmap";
    case PreviewFormat::jpeg_xl:
        return "jpeg-xl";
    case PreviewFormat::h265:
        return "h265";
    case PreviewFormat::unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace shadow::image
