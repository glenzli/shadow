#include <shadow/image/source_rendering.hpp>

#include <shadow/image/edit.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace shadow::image {

namespace {

inline constexpr std::string_view shadow_standard_profile_id = "shadow-standard";
inline constexpr std::string_view shadow_standard_profile_identity = "shadow-standard-v1";
inline constexpr std::string_view dcp_shadow_standard_profile_id = "shadow-standard-dcp";
inline constexpr std::string_view embedded_rendering_profile_id = "embedded-rendering";
inline constexpr std::string_view embedded_rendering_profile_identity = "embedded-rendering-v1";
inline constexpr double maximum_reasonable_dng_baseline_exposure_stops = 8.0;
inline constexpr std::size_t maximum_luminance_samples = 65'536U;
inline constexpr double normalization_percentile = 0.990;
inline constexpr double normalization_target_luminance = 0.70;
inline constexpr double normalization_floor_luminance = 0.015;
inline constexpr double maximum_standard_lift_stops = 1.0;
inline constexpr double curve_epsilon = 1.0e-12;

[[nodiscard]] bool is_standardized_linear_rgb(const PixelBuffer& source) noexcept {
    return source.bits_per_channel == 16U && (source.channels == 1U || source.channels == 3U)
        && source.dimensions.width != 0U && source.dimensions.height != 0U
        && source.primaries == RgbPrimaries::srgb_rec709_d65
        && source.transfer_function == RgbTransferFunction::linear;
}

[[nodiscard]] bool has_valid_dng_baseline_exposure(const AssetMetadata& metadata) noexcept {
    return !metadata.dng_version.empty() && std::isfinite(metadata.baseline_exposure)
        && std::abs(metadata.baseline_exposure) <= maximum_reasonable_dng_baseline_exposure_stops;
}

[[nodiscard]] double raw_luminance_percentile(const PixelBuffer& source) {
    if (!is_standardized_linear_rgb(source)) {
        throw std::invalid_argument(
            "source rendering requires standardized 16-bit linear sRGB RAW pixels"
        );
    }
    if (source.row_stride_bytes % sizeof(std::uint16_t) != 0U) {
        throw std::invalid_argument("source rendering received a non-u16 source row stride");
    }
    const std::size_t row_stride = source.row_stride_bytes / sizeof(std::uint16_t);
    const std::size_t minimum_stride =
        static_cast<std::size_t>(source.dimensions.width) * source.channels;
    if (row_stride < minimum_stride) {
        throw std::invalid_argument("source rendering received a truncated source row stride");
    }
    const std::uint64_t pixels = source.dimensions.pixel_count();
    if (pixels == 0U || pixels > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("source rendering source pixel count is invalid");
    }
    const std::size_t required = row_stride * static_cast<std::size_t>(source.dimensions.height);
    if (source.samples.size() < required) {
        throw std::invalid_argument("source rendering source samples are truncated");
    }

    const std::size_t pixel_count = static_cast<std::size_t>(pixels);
    const std::size_t sample_count = std::min(pixel_count, maximum_luminance_samples);
    const std::size_t step = std::max<std::size_t>(1U, pixel_count / sample_count);
    std::vector<double> luminances;
    luminances.reserve(sample_count + 1U);
    for (std::size_t linear_index = 0U; linear_index < pixel_count; linear_index += step) {
        const std::size_t row = linear_index / source.dimensions.width;
        const std::size_t column = linear_index % source.dimensions.width;
        const std::size_t index = row * row_stride + column * source.channels;
        const double red = static_cast<double>(source.samples[index]) / 65'535.0;
        const double green = source.channels == 1U
            ? red : static_cast<double>(source.samples[index + 1U]) / 65'535.0;
        const double blue = source.channels == 1U
            ? red : static_cast<double>(source.samples[index + 2U]) / 65'535.0;
        const double luminance = red * 0.2126 + green * 0.7152 + blue * 0.0722;
        if (std::isfinite(luminance) && luminance >= 0.0) {
            luminances.push_back(luminance);
        }
    }
    if (luminances.empty()) {
        return 0.0;
    }
    const std::size_t percentile_index = std::min(
        luminances.size() - 1U,
        static_cast<std::size_t>(
            std::floor(static_cast<double>(luminances.size() - 1U) * normalization_percentile)
        )
    );
    std::nth_element(
        luminances.begin(),
        luminances.begin() + static_cast<std::ptrdiff_t>(percentile_index),
        luminances.end()
    );
    return luminances[percentile_index];
}

[[nodiscard]] double standard_exposure_normalization_stops(const PixelBuffer& source) {
    const double high_luminance = raw_luminance_percentile(source);
    if (!(high_luminance >= normalization_floor_luminance)
        || high_luminance >= normalization_target_luminance) {
        return 0.0;
    }
    return std::clamp(
        std::log2(normalization_target_luminance / high_luminance),
        0.0,
        maximum_standard_lift_stops
    );
}

[[nodiscard]] std::size_t validated_float_row_stride(const FloatRgbImage& image) {
    if (
        image.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || image.transfer_function != TransferFunction::linear
        || image.dimensions.width == 0U || image.dimensions.height == 0U
        || image.row_stride_bytes % sizeof(float) != 0U
    ) {
        throw std::invalid_argument("source rendering requires a non-empty linear float RGB image");
    }
    const std::size_t row_stride = image.row_stride_bytes / sizeof(float);
    const std::size_t minimum_stride = static_cast<std::size_t>(image.dimensions.width) * 3U;
    if (row_stride < minimum_stride) {
        throw std::invalid_argument("source rendering float image row stride is invalid");
    }
    const std::size_t required = row_stride * static_cast<std::size_t>(image.dimensions.height);
    if (image.samples.size() < required) {
        throw std::invalid_argument("source rendering float image samples are truncated");
    }
    return row_stride;
}

[[nodiscard]] bool has_valid_luminance_tone_curve(
    const std::vector<SourceToneCurvePoint>& curve
) noexcept {
    if (curve.empty()) {
        return true;
    }
    if (curve.size() < 2U || curve.front().input != 0.0 || curve.back().input != 1.0) {
        return false;
    }
    for (std::size_t index = 0U; index < curve.size(); ++index) {
        const auto& point = curve[index];
        if (!std::isfinite(point.input) || !std::isfinite(point.output) || point.input < 0.0
            || point.input > 1.0 || point.output < 0.0 || point.output > 1.0) {
            return false;
        }
        if (index != 0U && (point.input <= curve[index - 1U].input
            || point.output < curve[index - 1U].output)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] double monotone_curve_value(
    const std::vector<SourceToneCurvePoint>& curve,
    const double input
) {
    if (curve.empty()) {
        return input;
    }
    if (input <= curve.front().input) {
        return curve.front().output;
    }
    if (input >= curve.back().input) {
        return curve.back().output;
    }

    const auto right = std::upper_bound(
        curve.begin(),
        curve.end(),
        input,
        [](const double value, const SourceToneCurvePoint& point) {
            return value < point.input;
        }
    );
    const std::size_t upper = static_cast<std::size_t>(right - curve.begin());
    const std::size_t lower = upper - 1U;
    const double x0 = curve[lower].input;
    const double x1 = curve[upper].input;
    const double y0 = curve[lower].output;
    const double y1 = curve[upper].output;
    const double interval = x1 - x0;
    const auto tangent_at = [&curve](const std::size_t index) {
        if (index == 0U) {
            return (curve[1U].output - curve[0U].output) / (curve[1U].input - curve[0U].input);
        }
        if (index + 1U == curve.size()) {
            return (curve[index].output - curve[index - 1U].output)
                / (curve[index].input - curve[index - 1U].input);
        }
        const double left_interval = curve[index].input - curve[index - 1U].input;
        const double right_interval = curve[index + 1U].input - curve[index].input;
        const double left_slope = (curve[index].output - curve[index - 1U].output) / left_interval;
        const double right_slope = (curve[index + 1U].output - curve[index].output) / right_interval;
        if (left_slope <= 0.0 || right_slope <= 0.0) {
            return 0.0;
        }
        // Fritsch-Carlson's weighted harmonic mean avoids overshoot between
        // monotone profile samples, unlike a generic cubic spline.
        const double weight_left = 2.0 * right_interval + left_interval;
        const double weight_right = right_interval + 2.0 * left_interval;
        return (weight_left + weight_right)
            / (weight_left / left_slope + weight_right / right_slope);
    };

    const double t = (input - x0) / interval;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;
    const double result = h00 * y0 + h10 * interval * tangent_at(lower)
        + h01 * y1 + h11 * interval * tangent_at(upper);
    return std::clamp(result, y0, y1);
}

void apply_luminance_tone_curve(
    FloatRgbImage& image,
    const std::vector<SourceToneCurvePoint>& curve,
    const std::size_t row_stride
) {
    if (curve.empty()) {
        return;
    }
    for (std::uint32_t row = 0U; row < image.dimensions.height; ++row) {
        for (std::uint32_t column = 0U; column < image.dimensions.width; ++column) {
            const std::size_t index = static_cast<std::size_t>(row) * row_stride
                + static_cast<std::size_t>(column) * 3U;
            const double red = std::max(0.0, static_cast<double>(image.samples[index]));
            const double green = std::max(0.0, static_cast<double>(image.samples[index + 1U]));
            const double blue = std::max(0.0, static_cast<double>(image.samples[index + 2U]));
            const double luminance = red * 0.2126 + green * 0.7152 + blue * 0.0722;
            // A profile curve is defined on the normalized RAW proxy domain.
            // Preserve super-white values rather than turning an exposure
            // lift into an unexpected highlight clip.
            if (luminance <= curve_epsilon || luminance >= 1.0) {
                continue;
            }
            const double mapped_luminance = monotone_curve_value(curve, luminance);
            const double gain = mapped_luminance / luminance;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                image.samples[index + channel] = static_cast<float>(
                    static_cast<double>(image.samples[index + channel]) * gain
                );
            }
        }
    }
}

[[nodiscard]] std::string fixed_identity_value(const double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6) << value;
    return stream.str();
}

} // namespace

SourceRenderingReceipt resolve_source_rendering(
    const PixelBuffer& source,
    const AssetMetadata& metadata
) {
    return resolve_source_rendering(source, metadata, load_local_source_profile_catalog());
}

SourceRenderingReceipt resolve_source_rendering(
    const PixelBuffer& source,
    const AssetMetadata& metadata,
    const SourceProfileCatalog& catalog
) {
    SourceRenderingReceipt receipt;
    if (source.reference != RgbBufferReference::processed_raw) {
        receipt.profile_id = std::string(embedded_rendering_profile_id);
        receipt.profile_identity = std::string(embedded_rendering_profile_identity);
        receipt.kind = SourceRenderingKind::embedded_rendering;
        return receipt;
    }

    receipt.profile_id = std::string(shadow_standard_profile_id);
    receipt.profile_identity = std::string(shadow_standard_profile_identity);
    receipt.kind = SourceRenderingKind::shadow_standard;
    const auto matched_profile = match_source_profile(catalog, metadata);
    if (matched_profile.has_value()) {
        receipt.profile_id = matched_profile->id;
        receipt.profile_identity = matched_profile->content_identity;
        receipt.kind = SourceRenderingKind::public_profile;
        receipt.profile_exposure_stops = matched_profile->display_exposure_stops;
        receipt.luminance_tone_curve = matched_profile->luminance_tone_curve;
    }
    if (has_valid_dng_baseline_exposure(metadata)) {
        receipt.camera_baseline_exposure_stops = metadata.baseline_exposure;
    } else if (!matched_profile.has_value() || !matched_profile->has_exposure_calibration) {
        receipt.standard_exposure_normalization_stops = standard_exposure_normalization_stops(source);
    }
    return receipt;
}

SourceRenderingReceipt resolve_source_rendering(
    const PixelBuffer& source,
    const AssetMetadata& metadata,
    const RawPipelineReceipt& pipeline
) {
    return resolve_source_rendering(
        source,
        metadata,
        pipeline,
        load_local_source_profile_catalog()
    );
}

SourceRenderingReceipt resolve_source_rendering(
    const PixelBuffer& source,
    const AssetMetadata& metadata,
    const RawPipelineReceipt& pipeline,
    const SourceProfileCatalog& catalog
) {
    if (!pipeline.valid()) {
        throw std::invalid_argument(
            "source rendering received an invalid RAW pipeline receipt"
        );
    }
    if (
        source.reference != RgbBufferReference::processed_raw
        || pipeline.camera_profile_status != RawCameraProfileStatus::applied
    ) {
        return resolve_source_rendering(source, metadata, catalog);
    }

    SourceRenderingReceipt receipt;
    receipt.profile_id = std::string(dcp_shadow_standard_profile_id);
    receipt.profile_identity = std::string(shadow_standard_profile_identity)
        + ";dcp=" + pipeline.camera_profile_identity;
    receipt.kind = SourceRenderingKind::shadow_standard;
    // DCP BaselineExposureOffset has already been folded into camera->working color. The source
    // DNG BaselineExposure is a separate per-image calibration and remains part of neutral source
    // rendering; only the maker/public camera look is suppressed.
    if (has_valid_dng_baseline_exposure(metadata)) {
        receipt.camera_baseline_exposure_stops = metadata.baseline_exposure;
    } else {
        receipt.standard_exposure_normalization_stops =
            standard_exposure_normalization_stops(source);
    }
    return receipt;
}

void apply_source_rendering(
    FloatRgbImage& image,
    const SourceRenderingReceipt& receipt
) {
    if (receipt.schema_version != source_rendering_schema_version) {
        throw std::invalid_argument("source rendering receipt schema is unsupported");
    }
    if (receipt.kind == SourceRenderingKind::embedded_rendering) {
        return;
    }
    if (
        receipt.kind != SourceRenderingKind::shadow_standard
        && receipt.kind != SourceRenderingKind::public_profile
    ) {
        throw std::invalid_argument("source rendering receipt kind is unsupported");
    }
    const double stops = receipt.total_exposure_stops();
    if (!std::isfinite(stops)) {
        throw std::invalid_argument("source rendering exposure is non-finite");
    }
    if (!has_valid_luminance_tone_curve(receipt.luminance_tone_curve)) {
        throw std::invalid_argument("source rendering tone curve is invalid");
    }
    const std::size_t row_stride = validated_float_row_stride(image);
    if (stops != 0.0) {
        const double gain = std::exp2(stops);
        for (std::uint32_t row = 0U; row < image.dimensions.height; ++row) {
            for (std::uint32_t column = 0U; column < image.dimensions.width; ++column) {
                const std::size_t index = static_cast<std::size_t>(row) * row_stride
                    + static_cast<std::size_t>(column) * 3U;
                for (std::size_t channel = 0U; channel < 3U; ++channel) {
                    image.samples[index + channel] = static_cast<float>(
                        static_cast<double>(image.samples[index + channel]) * gain
                    );
                }
            }
        }
    }
    apply_luminance_tone_curve(image, receipt.luminance_tone_curve, row_stride);
}

std::string source_rendering_identity(const SourceRenderingReceipt& receipt) {
    if (receipt.schema_version != source_rendering_schema_version) {
        throw std::invalid_argument("source rendering receipt schema is unsupported");
    }
    const std::string_view kind = receipt.kind == SourceRenderingKind::embedded_rendering
        ? "embedded"
        : receipt.kind == SourceRenderingKind::shadow_standard
        ? "shadow-standard"
        : receipt.kind == SourceRenderingKind::public_profile ? "public-profile" : "unknown";
    if (kind == "unknown" || receipt.profile_id.empty()) {
        throw std::invalid_argument("source rendering receipt is invalid");
    }
    std::string tone_curve_identity = "none";
    if (!receipt.luminance_tone_curve.empty()) {
        if (!has_valid_luminance_tone_curve(receipt.luminance_tone_curve)) {
            throw std::invalid_argument("source rendering tone curve is invalid");
        }
        tone_curve_identity.clear();
        for (const auto& point : receipt.luminance_tone_curve) {
            if (!tone_curve_identity.empty()) {
                tone_curve_identity.push_back(',');
            }
            tone_curve_identity += fixed_identity_value(point.input);
            tone_curve_identity.push_back(':');
            tone_curve_identity += fixed_identity_value(point.output);
        }
    }
    return "shadow-source-render-v" + std::to_string(receipt.schema_version)
        + ";implementation=" + std::to_string(source_rendering_implementation_version)
        + ";kind=" + std::string(kind)
        + ";profile=" + receipt.profile_id
        + ";profile-content=" + receipt.profile_identity
        + ";dng-baseline=" + fixed_identity_value(receipt.camera_baseline_exposure_stops)
        + ";standard-normalization="
        + fixed_identity_value(receipt.standard_exposure_normalization_stops)
        + ";profile-exposure=" + fixed_identity_value(receipt.profile_exposure_stops)
        + ";luminance-tone-curve=" + tone_curve_identity;
}

} // namespace shadow::image
