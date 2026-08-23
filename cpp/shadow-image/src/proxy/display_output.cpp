#include <shadow/image/display_output.hpp>

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/reference_pixels.hpp>
#include <shadow/image/working_rgb.hpp>

#include "../acceleration/image_acceleration_policy.hpp"
#include "../concurrency/row_scheduler.hpp"
#include "metal_display_output.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <ranges>
#include <string>
#include <utility>

namespace shadow::image {

namespace {

using LinearRgb = std::array<double, 3U>;

struct OklabColor final {
    double lightness = 0.0;
    double a = 0.0;
    double b = 0.0;
};

inline constexpr std::uint64_t maximum_display_rgb8_bytes = 512ULL * 1'024ULL * 1'024ULL;

[[nodiscard]] std::size_t checked_output_size(const Dimensions dimensions) {
    const std::uint64_t pixels = dimensions.pixel_count();
    if (pixels == 0U || pixels > maximum_display_rgb8_bytes / 3U) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "display RGB8 output exceeds Shadow's 512 MiB retained-buffer limit"
        );
    }
    return static_cast<std::size_t>(pixels * 3U);
}

void validate_source_and_request(const FloatRgbImage& source, const DisplayOutputRequest request) {
    constexpr double coordinate_tolerance = 1.0e-9;
    const auto close = [](const double actual, const double expected) {
        return std::abs(actual - expected) <= coordinate_tolerance;
    };
    const bool is_standardized_linear_srgb =
        source.pixel_format == FloatPixelFormat::rgb_f32_native_interleaved
        && source.transfer_function == TransferFunction::linear
        && (source.reference == ImageReference::scene_referred
            || source.reference == ImageReference::display_referred)
        && close(source.working_space.primaries[0].x, 0.6400)
        && close(source.working_space.primaries[0].y, 0.3300)
        && close(source.working_space.primaries[1].x, 0.3000)
        && close(source.working_space.primaries[1].y, 0.6000)
        && close(source.working_space.primaries[2].x, 0.1500)
        && close(source.working_space.primaries[2].y, 0.0600)
        && close(source.working_space.white_point.x, 0.3127)
        && close(source.working_space.white_point.y, 0.3290);
    if (!is_standardized_linear_srgb) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "display output requires standardized linear sRGB/Rec.709-D65 working RGB"
        );
    }
    if (source.dimensions.width == 0U || source.dimensions.height == 0U) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "display output requires non-zero source dimensions"
        );
    }
    if (request.target_dimensions != source.dimensions) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "display output v1 supports only source-sized rendering"
        );
    }
    const std::uint64_t packed_row_bytes =
        static_cast<std::uint64_t>(source.dimensions.width) * 3U * sizeof(float);
    if (packed_row_bytes > std::numeric_limits<std::size_t>::max()
        || source.row_stride_bytes < static_cast<std::size_t>(packed_row_bytes)
        || source.row_stride_bytes % sizeof(float) != 0U) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "display output source has an invalid float RGB row stride"
        );
    }
    const std::uint64_t stride_samples = source.row_stride_bytes / sizeof(float);
    const std::uint64_t required_samples =
        static_cast<std::uint64_t>(source.dimensions.height - 1U) * stride_samples
        + static_cast<std::uint64_t>(source.dimensions.width) * 3U;
    if (required_samples > source.samples.size()) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "display output source storage is shorter than its declared raster"
        );
    }
    // A GPU kernel cannot report which sample was invalid. Perform the same finite-value
    // contract check before backend selection so CPU, Metal and automatic fallback fail alike.
    for (std::uint32_t y = 0U; y < source.dimensions.height; ++y) {
        detail::throw_if_row_cancelled();
        const std::size_t row = static_cast<std::size_t>(y) * stride_samples;
        const std::size_t active_samples = static_cast<std::size_t>(source.dimensions.width) * 3U;
        for (std::size_t index = 0U; index < active_samples; ++index) {
            if (!std::isfinite(source.samples[row + index])) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "edited proxy contains a non-finite linear sample"
                );
            }
        }
    }
    static_cast<void>(checked_output_size(request.target_dimensions));
}

[[nodiscard]] OklabColor linear_srgb_to_oklab(const LinearRgb& rgb) noexcept {
    const double l =
        std::cbrt(0.4122214708 * rgb[0] + 0.5363325363 * rgb[1] + 0.0514459929 * rgb[2]);
    const double m =
        std::cbrt(0.2119034982 * rgb[0] + 0.6806995451 * rgb[1] + 0.1073969566 * rgb[2]);
    const double s =
        std::cbrt(0.0883024619 * rgb[0] + 0.2817188376 * rgb[1] + 0.6299787005 * rgb[2]);
    return OklabColor{
        .lightness = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        .a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        .b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

[[nodiscard]] LinearRgb oklab_to_linear_srgb(const OklabColor& lab) noexcept {
    const double l_root = lab.lightness + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
    const double m_root = lab.lightness - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
    const double s_root = lab.lightness - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
    const double l = l_root * l_root * l_root;
    const double m = m_root * m_root * m_root;
    const double s = s_root * s_root * s_root;
    return {
        4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    };
}

[[nodiscard]] bool is_inside_display_srgb(const LinearRgb& rgb) noexcept {
    return std::ranges::all_of(rgb, [](const double value) {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    });
}

[[nodiscard]] double scene_luminance_to_display_luminance(const double luminance) noexcept {
    if (!(luminance > 0.0)) {
        return 0.0;
    }
    // LibRaw's user-facing `-g 2.222 4.5` request stores 1 / 2.222 and 4.5
    // internally. Its resulting BT.709-style encoded value is not sRGB's
    // encoded value, while Shadow's final RGB8 boundary remains sRGB. Convert
    // the LibRaw code value back to linear sRGB here, then let the shared
    // sRGB OETF below emit the matching display code. LibRaw H=0 is the RAW
    // development policy; it is not a reason to throw away scene-linear
    // headroom at Shadow's SDR presentation boundary. Keep middle gray and
    // the normal working range unchanged, then use a C1 neutral shoulder so
    // highlight detail approaches display white without an abrupt dead-white
    // plateau. The scalar gain preserves the input's chromaticity; gamut
    // mapping remains the separate Oklab stage below.
    constexpr double shoulder_start = 0.75;
    constexpr double shoulder_headroom = 1.0 - shoulder_start;
    constexpr double rec709_linear_threshold = 0.018;
    constexpr double rec709_slope = 4.5;
    constexpr double rec709_power = 0.45;
    constexpr double rec709_gain = 1.099;
    constexpr double rec709_offset = 0.099;
    constexpr double srgb_encoded_threshold = 0.04045;
    constexpr double srgb_linear_slope = 12.92;
    constexpr double srgb_gain = 1.055;
    constexpr double srgb_offset = 0.055;
    const double scene = luminance <= shoulder_start
        ? luminance
        : shoulder_start
            + (luminance - shoulder_start) * shoulder_headroom
                / (luminance - shoulder_start + shoulder_headroom);
    const double rec709_encoded = scene < rec709_linear_threshold
        ? rec709_slope * scene
        : rec709_gain * std::pow(scene, rec709_power) - rec709_offset;
    return rec709_encoded <= srgb_encoded_threshold
        ? rec709_encoded / srgb_linear_slope
        : std::pow((rec709_encoded + srgb_offset) / srgb_gain, 2.4);
}

[[nodiscard]] LinearRgb apply_neutral_scene_display_curve(const LinearRgb& input) noexcept {
    const double luminance = input[0] * 0.2126 + input[1] * 0.7152 + input[2] * 0.0722;
    if (!(luminance > 0.0)) {
        return input;
    }
    const double gain = scene_luminance_to_display_luminance(luminance) / luminance;
    return {input[0] * gain, input[1] * gain, input[2] * gain};
}

[[nodiscard]] LinearRgb
map_linear_srgb_to_display_gamut(const LinearRgb& input, const bool apply_scene_curve) noexcept {
    const LinearRgb display_linear =
        apply_scene_curve ? apply_neutral_scene_display_curve(input) : input;
    if (is_inside_display_srgb(display_linear)) {
        return display_linear;
    }

    OklabColor lab = linear_srgb_to_oklab(display_linear);
    lab.lightness = std::clamp(lab.lightness, 0.0, 1.0);
    const double chroma = std::hypot(lab.a, lab.b);
    LinearRgb best = oklab_to_linear_srgb(OklabColor{.lightness = lab.lightness});
    if (!std::isfinite(chroma) || chroma <= 1.0e-15) {
        for (double& channel : best) {
            channel = std::clamp(channel, 0.0, 1.0);
        }
        return best;
    }

    const double a_direction = lab.a / chroma;
    const double b_direction = lab.b / chroma;
    double lower = 0.0;
    double upper = std::min(chroma, display_srgb8_maximum_oklab_chroma);
    for (std::uint32_t iteration = 0U; iteration < display_srgb8_gamut_search_iterations;
         ++iteration) {
        const double candidate_chroma = std::midpoint(lower, upper);
        const LinearRgb candidate = oklab_to_linear_srgb(
            OklabColor{
                .lightness = lab.lightness,
                .a = a_direction * candidate_chroma,
                .b = b_direction * candidate_chroma,
            }
        );
        if (is_inside_display_srgb(candidate)) {
            lower = candidate_chroma;
            best = candidate;
        } else {
            upper = candidate_chroma;
        }
    }
    for (double& channel : best) {
        channel = std::clamp(channel, 0.0, 1.0);
    }
    return best;
}

[[nodiscard]] double
display_quantization_dither(const std::uint32_t x, const std::uint32_t y) noexcept {
    std::uint32_t state = x * 0x9e3779b9U ^ y * 0x85ebca6bU;
    state ^= state >> 16U;
    state *= 0x7feb352dU;
    state ^= state >> 15U;
    state *= 0x846ca68bU;
    state ^= state >> 16U;
    const double unit =
        static_cast<double>(state) / static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    return (unit - 0.5) * 0.90;
}

[[nodiscard]] std::uint8_t
linear_display_sample_to_srgb8(const double linear_sample, const double dither) noexcept {
    const double linear = std::clamp(linear_sample, 0.0, 1.0);
    constexpr double srgb_linear_threshold = 0.0031308;
    const double encoded = linear <= srgb_linear_threshold
                               ? 12.92 * linear
                               : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(
        std::clamp(std::floor(encoded * 255.0 + dither + 0.5), 0.0, 255.0)
    );
}

[[nodiscard]] DisplayRgb8Image
render_on_cpu(const FloatRgbImage& source, const DisplayOutputRequest request) {
    DisplayRgb8Image output{
        .dimensions = request.target_dimensions,
        .row_stride_bytes = static_cast<std::size_t>(request.target_dimensions.width) * 3U,
        .bytes = std::vector<std::uint8_t>(checked_output_size(request.target_dimensions)),
        .backend = DisplayOutputBackend::cpu,
        .fell_back = false,
        .diagnostic = {},
    };
    const std::size_t source_stride = source.row_stride_bytes / sizeof(float);
    detail::parallel_for_rows(
        source.dimensions.height,
        16U,
        [&source,
         request,
         source_stride,
         &output](const std::uint32_t first_row, const std::uint32_t last_row) {
            for (std::uint32_t y = first_row; y < last_row; ++y) {
                const std::size_t source_row = static_cast<std::size_t>(y) * source_stride;
                const std::size_t output_row =
                    static_cast<std::size_t>(y) * output.row_stride_bytes;
                for (std::uint32_t x = 0U; x < source.dimensions.width; ++x) {
                    const std::size_t source_index = source_row + static_cast<std::size_t>(x) * 3U;
                    const std::size_t output_index = output_row + static_cast<std::size_t>(x) * 3U;
                    const LinearRgb mapped = map_linear_srgb_to_display_gamut(
                        {
                            source.samples[source_index],
                            source.samples[source_index + 1U],
                            source.samples[source_index + 2U],
                        },
                        source.reference == ImageReference::scene_referred
                    );
                    const double dither = display_quantization_dither(
                        request.output_origin_x + x,
                        request.output_origin_y + y
                    );
                    for (std::size_t channel = 0U; channel < 3U; ++channel) {
                        output.bytes[output_index + channel] =
                            linear_display_sample_to_srgb8(mapped[channel], dither);
                    }
                }
            }
        }
    );
    return output;
}

} // namespace

std::string_view display_output_backend_identity(const DisplayOutputBackend backend) noexcept {
    switch (backend) {
    case DisplayOutputBackend::cpu:
        return "shadow-display-output-cpu-v1;math=f64";
    case DisplayOutputBackend::metal:
        return "shadow-display-output-metal-v1;math=f32-safe";
    }
    return "shadow-display-output-unknown";
}

bool display_output_backend_available(const DisplayOutputBackend backend) noexcept {
    switch (backend) {
    case DisplayOutputBackend::cpu:
        return true;
    case DisplayOutputBackend::metal:
        return detail::metal_display_output_available();
    }
    return false;
}

DisplayOutputBackendMode display_output_backend_mode_from_environment() {
    const auto preference = detail::image_acceleration_preference_from_environment();
    if (!preference.has_value()) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "SHADOW_IMAGE_ACCELERATION must be auto, cpu, or metal"
        );
    }
    if (*preference == detail::ImageAccelerationPreference::automatic) {
        return DisplayOutputBackendMode::automatic;
    }
    if (*preference == detail::ImageAccelerationPreference::cpu) {
        return DisplayOutputBackendMode::cpu;
    }
    return DisplayOutputBackendMode::metal;
}

bool DisplayRgb8Image::valid() const noexcept {
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(dimensions.width) * 3U;
    const std::uint64_t pixels = dimensions.pixel_count();
    if (dimensions.width == 0U || dimensions.height == 0U
        || row_bytes > std::numeric_limits<std::size_t>::max()
        || row_stride_bytes != static_cast<std::size_t>(row_bytes)
        || pixels > std::numeric_limits<std::uint64_t>::max() / 3U
        || (backend != DisplayOutputBackend::cpu && backend != DisplayOutputBackend::metal)
        || (fell_back && (backend != DisplayOutputBackend::cpu || diagnostic.empty()))
        || (!fell_back && !diagnostic.empty())) {
        return false;
    }
    const std::uint64_t expected = pixels * 3U;
    return expected <= std::numeric_limits<std::size_t>::max()
           && bytes.size() == static_cast<std::size_t>(expected);
}

DisplayRgb8Image render_linear_srgb_to_display_srgb8_cpu_reference(
    const FloatRgbImage& source,
    const DisplayOutputRequest request
) {
    validate_source_and_request(source, request);
    auto result = render_on_cpu(source, request);
    if (!result.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "CPU display output produced an invalid RGB8 raster"
        );
    }
    return result;
}

DisplayRgb8Image render_linear_srgb_to_display_srgb8_with_backend(
    const FloatRgbImage& source,
    const DisplayOutputRequest request,
    const DisplayOutputBackendMode backend_mode
) {
    validate_source_and_request(source, request);
    if (backend_mode != DisplayOutputBackendMode::automatic
        && backend_mode != DisplayOutputBackendMode::cpu
        && backend_mode != DisplayOutputBackendMode::metal) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "display output received an unknown backend mode"
        );
    }
    if (backend_mode != DisplayOutputBackendMode::cpu) {
        auto attempt = detail::try_render_linear_srgb_to_display_srgb8_metal(source, request);
        if (attempt.output.has_value()) {
            return std::move(*attempt.output);
        }
        if (backend_mode == DisplayOutputBackendMode::metal) {
            const std::string diagnostic = attempt.diagnostic.empty()
                                               ? "Metal display output is unavailable"
                                               : std::move(attempt.diagnostic);
            throw DecodeError(DecodeErrorCode::internal, 0, diagnostic);
        }
        auto result = render_on_cpu(source, request);
        if (!result.valid()) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "display output fallback produced an invalid CPU RGB8 raster"
            );
        }
        result.fell_back = true;
        result.diagnostic = attempt.diagnostic.empty() ? "Metal display output declined the request"
                                                       : std::move(attempt.diagnostic);
        return result;
    }
    auto result = render_on_cpu(source, request);
    if (!result.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "display output backend selection produced an invalid RGB8 raster"
        );
    }
    return result;
}

DisplayRgb8Image render_linear_srgb_to_display_srgb8(
    const FloatRgbImage& source,
    const DisplayOutputRequest request
) {
    return render_linear_srgb_to_display_srgb8_with_backend(
        source,
        request,
        display_output_backend_mode_from_environment()
    );
}

} // namespace shadow::image
