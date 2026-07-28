#include "dcp_color_rendering.hpp"

#include "dcp_color_matrix_math.hpp"
#include "metal_raw_development.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

using detail::dcp_color_matrix_math::Matrix3;
using detail::dcp_color_matrix_math::Vector3;
using detail::dcp_color_matrix_math::chromatic_adaptation;
using detail::dcp_color_matrix_math::d50_xyz;
using detail::dcp_color_matrix_math::d65_xyz;
using detail::dcp_color_matrix_math::linear_srgb_to_xyz_d65;
using detail::dcp_color_matrix_math::multiply;
using detail::dcp_color_matrix_math::xyz_d65_to_linear_srgb;

// DCP's HueSatMap, LookTable, and ProfileToneCurve are specified in the
// linear ProPhoto/ROMM RGB working space after the camera transform. These
// matrices form a contained boundary around Shadow's linear-sRGB RAW source;
// no creative Recipe operation needs to know about the profile space.
inline constexpr Matrix3 xyz_d50_to_linear_prophoto{
    1.3459433, -0.2556075, -0.0511118,
    -0.5445989, 1.5081673, 0.0205351,
    0.0, 0.0, 1.2118128,
};
inline constexpr Matrix3 linear_prophoto_to_xyz_d50{
    0.7977604897, 0.1351858372, 0.0313493496,
    0.2880711282, 0.7118432178, 0.00008565396,
    0.0, 0.0, 0.8251046025,
};
inline constexpr double pi = 3.141592653589793238462643383279502884;

[[noreturn]] void fail(
    const DcpColorDevelopmentErrorCode code,
    const std::string_view message
) {
    throw DcpColorDevelopmentError(code, std::string(message));
}

[[nodiscard]] double clamp_unit(const double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] std::size_t hsv_table_entry_count(const DcpHsvTable& table) {
    const std::uint64_t count = static_cast<std::uint64_t>(table.hue_divisions)
        * static_cast<std::uint64_t>(table.saturation_divisions)
        * static_cast<std::uint64_t>(table.value_divisions);
    if (count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP HSV table exceeds the local address space"
        );
    }
    return static_cast<std::size_t>(count);
}

void validate_hsv_table(const DcpHsvTable& table, const std::string_view name) {
    if (table.hue_divisions == 0U || table.saturation_divisions < 2U
        || table.value_divisions == 0U
        || hsv_table_entry_count(table) != table.entries.size()
        || (table.value_divisions == 1U && table.encoding != DcpTableEncoding::linear)) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            std::string("DCP ") + std::string(name) + " has an invalid table shape"
        );
    }
    for (std::size_t index = 0U; index < table.entries.size(); ++index) {
        const DcpHsvDelta& delta = table.entries[index];
        if (!std::isfinite(delta.hue_shift_degrees)
            || !std::isfinite(delta.saturation_scale)
            || !std::isfinite(delta.value_scale)
            || delta.saturation_scale < 0.0F || delta.value_scale < 0.0F) {
            fail(
                DcpColorDevelopmentErrorCode::invalid_input,
                std::string("DCP ") + std::string(name) + " has an invalid HSV delta"
            );
        }
        const std::size_t saturation_index =
            index % static_cast<std::size_t>(table.saturation_divisions);
        if (saturation_index == 0U && delta.value_scale != 1.0F) {
            fail(
                DcpColorDevelopmentErrorCode::invalid_input,
                std::string("DCP ") + std::string(name)
                    + " changes value at zero saturation"
            );
        }
    }
}

[[nodiscard]] std::optional<DcpHsvTable> resolve_hue_sat_map(
    const DcpProfile& profile,
    const double calibration1_weight
) {
    const auto& first = profile.calibration1.hue_sat_map;
    const auto& second = profile.calibration2.has_value()
        ? profile.calibration2->hue_sat_map : std::optional<DcpHsvTable>{};
    if (!first.has_value() && !second.has_value()) {
        return std::nullopt;
    }
    if (!first.has_value()) {
        validate_hsv_table(*second, "HueSatMap2");
        return *second;
    }
    validate_hsv_table(*first, "HueSatMap1");
    if (!second.has_value()) {
        return *first;
    }
    validate_hsv_table(*second, "HueSatMap2");
    if (first->hue_divisions != second->hue_divisions
        || first->saturation_divisions != second->saturation_divisions
        || first->value_divisions != second->value_divisions
        || first->encoding != second->encoding) {
        fail(
            DcpColorDevelopmentErrorCode::unsupported_rendering_feature,
            "dual-illuminant DCP HueSatMap tables do not share one interpolation grid"
        );
    }
    DcpHsvTable interpolated = *first;
    for (std::size_t index = 0U; index < interpolated.entries.size(); ++index) {
        const DcpHsvDelta& a = first->entries[index];
        const DcpHsvDelta& b = second->entries[index];
        interpolated.entries[index] = DcpHsvDelta{
            .hue_shift_degrees = static_cast<float>(
                a.hue_shift_degrees * calibration1_weight
                + b.hue_shift_degrees * (1.0 - calibration1_weight)
            ),
            .saturation_scale = static_cast<float>(
                a.saturation_scale * calibration1_weight
                + b.saturation_scale * (1.0 - calibration1_weight)
            ),
            .value_scale = static_cast<float>(
                a.value_scale * calibration1_weight
                + b.value_scale * (1.0 - calibration1_weight)
            ),
        };
    }
    return interpolated;
}

void validate_tone_curve(const std::vector<DcpToneCurvePoint>& curve) {
    if (curve.empty()) {
        return;
    }
    if (curve.size() < 2U || curve.front().input != 0.0F || curve.front().output != 0.0F
        || curve.back().input != 1.0F || curve.back().output != 1.0F) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP tone curve must retain normalized black and white endpoints"
        );
    }
    float previous = -1.0F;
    for (const DcpToneCurvePoint& point : curve) {
        if (!std::isfinite(point.input) || !std::isfinite(point.output)
            || point.input < 0.0F || point.input > 1.0F
            || point.output < 0.0F || point.output > 1.0F
            || point.input <= previous) {
            fail(
                DcpColorDevelopmentErrorCode::invalid_input,
                "DCP tone curve must be finite and strictly ordered"
            );
        }
        previous = point.input;
    }
}

[[nodiscard]] std::vector<double> natural_spline_second_derivatives(
    const std::vector<DcpToneCurvePoint>& curve
) {
    std::vector<double> second(curve.size(), 0.0);
    if (curve.size() <= 2U) {
        return second;
    }
    std::vector<double> work(curve.size() - 1U, 0.0);
    for (std::size_t index = 1U; index + 1U < curve.size(); ++index) {
        const double left = curve[index].input - curve[index - 1U].input;
        const double right = curve[index + 1U].input - curve[index].input;
        const double span = curve[index + 1U].input - curve[index - 1U].input;
        const double sigma = left / span;
        const double pivot = sigma * second[index - 1U] + 2.0;
        second[index] = (sigma - 1.0) / pivot;
        work[index] = (
            6.0 * ((curve[index + 1U].output - curve[index].output) / right
                - (curve[index].output - curve[index - 1U].output) / left) / span
            - sigma * work[index - 1U]
        ) / pivot;
    }
    for (std::size_t index = curve.size() - 1U; index-- > 0U;) {
        second[index] = second[index] * second[index + 1U] + work[index];
    }
    return second;
}

[[nodiscard]] double sample_tone_curve(
    const std::vector<DcpToneCurvePoint>& curve,
    const std::vector<double>& second_derivatives,
    const double input
) noexcept {
    if (curve.empty()) {
        return clamp_unit(input);
    }
    const double x = clamp_unit(input);
    const auto upper = std::upper_bound(
        curve.begin(),
        curve.end(),
        x,
        [](const double value, const DcpToneCurvePoint& point) {
            return value < point.input;
        }
    );
    const std::size_t right = upper == curve.end()
        ? curve.size() - 1U
        : static_cast<std::size_t>(upper - curve.begin());
    if (right == 0U) {
        return curve.front().output;
    }
    const std::size_t left = right - 1U;
    const double width = curve[right].input - curve[left].input;
    const double a = (curve[right].input - x) / width;
    const double b = (x - curve[left].input) / width;
    const double output = a * curve[left].output + b * curve[right].output
        + ((a * a * a - a) * second_derivatives[left]
            + (b * b * b - b) * second_derivatives[right])
            * width * width / 6.0;
    return clamp_unit(output);
}

[[nodiscard]] double srgb_encode(const double linear) noexcept {
    const double value = clamp_unit(linear);
    return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

[[nodiscard]] double srgb_decode(const double encoded) noexcept {
    const double value = clamp_unit(encoded);
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

struct Hsv final {
    double hue = 0.0;
    double saturation = 0.0;
    double value = 0.0;
};

[[nodiscard]] Hsv rgb_to_hsv(const Vector3& rgb) noexcept {
    const double red = clamp_unit(rgb[0]);
    const double green = clamp_unit(rgb[1]);
    const double blue = clamp_unit(rgb[2]);
    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    const double chroma = maximum - minimum;
    Hsv hsv{.value = maximum};
    if (maximum <= 1.0e-12 || chroma <= 1.0e-12) {
        return hsv;
    }
    hsv.saturation = chroma / maximum;
    if (maximum == red) {
        hsv.hue = (green - blue) / chroma;
    } else if (maximum == green) {
        hsv.hue = 2.0 + (blue - red) / chroma;
    } else {
        hsv.hue = 4.0 + (red - green) / chroma;
    }
    hsv.hue = std::fmod(hsv.hue / 6.0 + 1.0, 1.0);
    return hsv;
}

[[nodiscard]] Vector3 hsv_to_rgb(const Hsv& hsv) noexcept {
    const double hue = std::fmod(hsv.hue + 1.0, 1.0) * 6.0;
    const double saturation = clamp_unit(hsv.saturation);
    const double value = clamp_unit(hsv.value);
    const double chroma = value * saturation;
    const double intermediate = chroma * (1.0 - std::abs(std::fmod(hue, 2.0) - 1.0));
    const double match = value - chroma;
    if (hue < 1.0) {
        return {chroma + match, intermediate + match, match};
    }
    if (hue < 2.0) {
        return {intermediate + match, chroma + match, match};
    }
    if (hue < 3.0) {
        return {match, chroma + match, intermediate + match};
    }
    if (hue < 4.0) {
        return {match, intermediate + match, chroma + match};
    }
    if (hue < 5.0) {
        return {intermediate + match, match, chroma + match};
    }
    return {chroma + match, match, intermediate + match};
}

struct HsvDeltaSample final {
    double hue_shift_degrees = 0.0;
    double saturation_scale = 1.0;
    double value_scale = 1.0;
};

[[nodiscard]] HsvDeltaSample sample_hsv_table(
    const DcpHsvTable& table,
    const Hsv& hsv
) noexcept {
    const double hue_coordinate = hsv.hue * static_cast<double>(table.hue_divisions);
    const std::size_t hue0 = static_cast<std::size_t>(std::floor(hue_coordinate))
        % static_cast<std::size_t>(table.hue_divisions);
    const std::size_t hue1 = (hue0 + 1U) % static_cast<std::size_t>(table.hue_divisions);
    const double hue_fraction = hue_coordinate - std::floor(hue_coordinate);

    const double saturation_coordinate = clamp_unit(hsv.saturation)
        * static_cast<double>(table.saturation_divisions - 1U);
    const std::size_t saturation0 = static_cast<std::size_t>(std::floor(saturation_coordinate));
    const std::size_t saturation1 = std::min(
        saturation0 + 1U,
        static_cast<std::size_t>(table.saturation_divisions - 1U)
    );
    const double saturation_fraction = saturation_coordinate - std::floor(saturation_coordinate);

    const double value_coordinate = clamp_unit(hsv.value)
        * static_cast<double>(table.value_divisions - 1U);
    const std::size_t value0 = static_cast<std::size_t>(std::floor(value_coordinate));
    const std::size_t value1 = std::min(
        value0 + 1U,
        static_cast<std::size_t>(table.value_divisions - 1U)
    );
    const double value_fraction = value_coordinate - std::floor(value_coordinate);

    const auto entry = [&table](
        const std::size_t value,
        const std::size_t hue,
        const std::size_t saturation
    ) -> const DcpHsvDelta& {
        const std::size_t index = ((value * static_cast<std::size_t>(table.hue_divisions)) + hue)
            * static_cast<std::size_t>(table.saturation_divisions) + saturation;
        return table.entries[index];
    };

    double hue_sine = 0.0;
    double hue_cosine = 0.0;
    double saturation_scale = 0.0;
    double value_scale = 0.0;
    for (const auto [value, value_weight] : std::array{
             std::pair{value0, 1.0 - value_fraction},
             std::pair{value1, value_fraction},
         }) {
        for (const auto [hue, hue_weight] : std::array{
                 std::pair{hue0, 1.0 - hue_fraction},
                 std::pair{hue1, hue_fraction},
             }) {
            for (const auto [saturation, saturation_weight] : std::array{
                     std::pair{saturation0, 1.0 - saturation_fraction},
                     std::pair{saturation1, saturation_fraction},
                 }) {
                const double weight = value_weight * hue_weight * saturation_weight;
                const DcpHsvDelta& delta = entry(value, hue, saturation);
                const double radians = static_cast<double>(delta.hue_shift_degrees) * pi / 180.0;
                hue_sine += std::sin(radians) * weight;
                hue_cosine += std::cos(radians) * weight;
                saturation_scale += static_cast<double>(delta.saturation_scale) * weight;
                value_scale += static_cast<double>(delta.value_scale) * weight;
            }
        }
    }
    return HsvDeltaSample{
        .hue_shift_degrees = std::atan2(hue_sine, hue_cosine) * 180.0 / pi,
        .saturation_scale = saturation_scale,
        .value_scale = value_scale,
    };
}

[[nodiscard]] Vector3 apply_hsv_table(
    const Vector3& linear_prophoto,
    const DcpHsvTable& table
) noexcept {
    Hsv hsv = rgb_to_hsv(linear_prophoto);
    const bool use_srgb_value = table.encoding == DcpTableEncoding::srgb;
    if (use_srgb_value) {
        hsv.value = srgb_encode(hsv.value);
    }
    const HsvDeltaSample delta = sample_hsv_table(table, hsv);
    hsv.hue = std::fmod(hsv.hue + delta.hue_shift_degrees / 360.0 + 1.0, 1.0);
    hsv.saturation = clamp_unit(hsv.saturation * delta.saturation_scale);
    hsv.value = clamp_unit(hsv.value * delta.value_scale);
    if (use_srgb_value) {
        hsv.value = srgb_decode(hsv.value);
    }
    return hsv_to_rgb(hsv);
}

[[nodiscard]] Matrix3 srgb_to_dcp_working_space() noexcept {
    return multiply(
        xyz_d50_to_linear_prophoto,
        multiply(
            chromatic_adaptation(d65_xyz, d50_xyz),
            linear_srgb_to_xyz_d65
        )
    );
}

[[nodiscard]] Matrix3 dcp_working_space_to_srgb() noexcept {
    return multiply(
        xyz_d65_to_linear_srgb,
        multiply(
            chromatic_adaptation(d50_xyz, d65_xyz),
            linear_prophoto_to_xyz_d50
        )
    );
}

[[nodiscard]] bool finite_vector(const Vector3& value) noexcept {
    return std::ranges::all_of(value, [](const double channel) {
        return std::isfinite(channel);
    });
}

// DCP input rendering is a camera-owned, per-pixel stage after the fused RAW
// developer.  HueSatMap and LookTable evaluation is expensive enough to make
// a substantial preview feel serial on desktop CPUs, while each pixel remains
// completely independent.  Keep the threshold high enough that tiny proxies
// avoid scheduling overhead, cap the worker count for concurrent catalog work,
// and retain the exact per-pixel arithmetic of the serial reference path.
inline constexpr std::size_t dcp_parallel_minimum_pixels = 32U * 1'024U;
inline constexpr std::size_t dcp_parallel_maximum_workers = 8U;

template <typename PixelOperation>
void apply_dcp_to_pixels(
    const std::size_t pixel_count,
    PixelOperation&& operation
) {
    if (pixel_count < dcp_parallel_minimum_pixels) {
        for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
            operation(pixel * 3U);
        }
        return;
    }

    const std::size_t hardware_workers = std::max<std::size_t>(
        1U,
        static_cast<std::size_t>(std::thread::hardware_concurrency())
    );
    const std::size_t useful_workers =
        (pixel_count + dcp_parallel_minimum_pixels - 1U) / dcp_parallel_minimum_pixels;
    const std::size_t worker_count = std::min({
        hardware_workers,
        useful_workers,
        dcp_parallel_maximum_workers,
    });
    if (worker_count <= 1U) {
        for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
            operation(pixel * 3U);
        }
        return;
    }

    const std::size_t pixels_per_worker = (pixel_count + worker_count - 1U) / worker_count;
    std::atomic_bool cancelled{false};
    std::exception_ptr failure;
    std::mutex failure_mutex;
    {
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0U; worker < worker_count; ++worker) {
            const std::size_t first_pixel = worker * pixels_per_worker;
            const std::size_t final_pixel = std::min(first_pixel + pixels_per_worker, pixel_count);
            if (first_pixel >= final_pixel) {
                continue;
            }
            workers.emplace_back([&, first_pixel, final_pixel] {
                try {
                    for (std::size_t pixel = first_pixel; pixel < final_pixel; ++pixel) {
                        if (cancelled.load(std::memory_order_relaxed)) {
                            return;
                        }
                        operation(pixel * 3U);
                    }
                } catch (...) {
                    {
                        std::lock_guard failure_lock(failure_mutex);
                        if (failure == nullptr) {
                            failure = std::current_exception();
                        }
                    }
                    cancelled.store(true, std::memory_order_relaxed);
                }
            });
        }
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

// DCP's HSV tables and tone curve have a defined [0, 1] domain.  For an HDR
// scene-linear pixel, apply them to its chromatic ratio and restore the peak
// afterwards.  This is continuous at display white, keeps the profile's hue
// and saturation intent, and—unlike the former packed compatibility route—
// does not throw away measured highlight headroom.  Negative gamut-excursion
// components bypass these bounded profile stages rather than being silently
// clamped to black.
[[nodiscard]] Vector3 apply_dcp_post_matrix_stages(
    const Vector3& linear_srgb,
    const DcpColorTransform& transform
) {
    if (!finite_vector(linear_srgb)) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP input rendering received non-finite scene-linear samples"
        );
    }
    Vector3 linear_prophoto = multiply(srgb_to_dcp_working_space(), linear_srgb);
    if (!finite_vector(linear_prophoto)) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP input rendering produced non-finite working-space samples"
        );
    }
    const bool bounded_input = std::ranges::all_of(linear_prophoto, [](const double channel) {
        return channel >= 0.0 && channel <= 1.0;
    });
    if (!bounded_input) {
        const double peak = std::max({
            linear_prophoto[0], linear_prophoto[1], linear_prophoto[2],
        });
        if (peak <= 0.0) {
            return linear_srgb;
        }
        for (double& channel : linear_prophoto) {
            if (channel < 0.0) {
                return linear_srgb;
            }
            channel /= peak;
        }
        if (transform.hue_sat_map.has_value()) {
            linear_prophoto = apply_hsv_table(linear_prophoto, *transform.hue_sat_map);
        }
        if (transform.look_table.has_value()) {
            linear_prophoto = apply_hsv_table(linear_prophoto, *transform.look_table);
        }
        if (!transform.tone_curve.empty()) {
            for (double& channel : linear_prophoto) {
                channel = sample_tone_curve(
                    transform.tone_curve,
                    transform.tone_curve_second_derivatives,
                    channel
                );
            }
        }
        for (double& channel : linear_prophoto) {
            channel *= peak;
        }
    } else {
        if (transform.hue_sat_map.has_value()) {
            linear_prophoto = apply_hsv_table(linear_prophoto, *transform.hue_sat_map);
        }
        if (transform.look_table.has_value()) {
            linear_prophoto = apply_hsv_table(linear_prophoto, *transform.look_table);
        }
        if (!transform.tone_curve.empty()) {
            for (double& channel : linear_prophoto) {
                channel = sample_tone_curve(
                    transform.tone_curve,
                    transform.tone_curve_second_derivatives,
                    channel
                );
            }
        }
    }
    const Vector3 result = multiply(dcp_working_space_to_srgb(), linear_prophoto);
    if (!finite_vector(result)) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP input rendering produced non-finite linear-sRGB samples"
        );
    }
    return result;
}

} // namespace

detail::PreparedDcpRenderingStages detail::prepare_dcp_rendering_stages(
    const DcpProfile& profile,
    const double calibration1_weight
) {
    const std::optional<DcpHsvTable> hue_sat_map =
        resolve_hue_sat_map(profile, calibration1_weight);
    if (profile.look_table.has_value()) {
        validate_hsv_table(*profile.look_table, "LookTable");
    }
    validate_tone_curve(profile.tone_curve);
    return PreparedDcpRenderingStages{
        .hue_sat_map = hue_sat_map,
        .look_table = profile.look_table,
        .tone_curve = profile.tone_curve,
        .tone_curve_second_derivatives =
            natural_spline_second_derivatives(profile.tone_curve),
    };
}

bool detail::dcp_rendering_stages_valid(
    const DcpColorTransform& transform
) noexcept {
    return transform.receipt.hue_sat_map_applied == transform.hue_sat_map.has_value()
        && transform.receipt.look_table_applied == transform.look_table.has_value()
        && transform.receipt.tone_curve_applied == !transform.tone_curve.empty()
        && (transform.tone_curve.empty()
            || transform.tone_curve_second_derivatives.size()
                == transform.tone_curve.size());
}

DcpColorExecutionBackend apply_dcp_color_rendering_stages(
    SceneLinearRgbFrame& pixels,
    const DcpColorTransform& transform
) {
    if (!transform.valid()) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP input rendering received an invalid compiled transform"
        );
    }
    if (!transform.has_post_matrix_stages()) {
        return DcpColorExecutionBackend::cpu;
    }
    const std::size_t expected_samples = static_cast<std::size_t>(pixels.dimensions.width)
        * static_cast<std::size_t>(pixels.dimensions.height) * 3U;
    if (!pixels.valid()
        || pixels.row_stride_bytes
            != static_cast<std::size_t>(pixels.dimensions.width) * 3U * sizeof(float)
        || pixels.samples.size() != expected_samples) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP input rendering requires Shadow's canonical scene-linear fp32 RAW frame"
        );
    }
    const RawDevelopmentBackendMode requested_backend =
        raw_development_backend_mode_from_environment();
    if (requested_backend != RawDevelopmentBackendMode::cpu) {
        const auto attempt = detail::try_apply_dcp_color_rendering_stages_metal(
            pixels,
            transform
        );
        if (attempt.applied) {
            return DcpColorExecutionBackend::metal;
        }
        if (requested_backend == RawDevelopmentBackendMode::metal) {
            throw DcpColorDevelopmentError(
                DcpColorDevelopmentErrorCode::unsupported_rendering_feature,
                "DCP Metal executor was explicitly requested but unavailable: "
                    + attempt.diagnostic
            );
        }
    }
    apply_dcp_to_pixels(pixels.samples.size() / 3U, [&](const std::size_t index) {
        const Vector3 linear_srgb = apply_dcp_post_matrix_stages(
            Vector3{
                static_cast<double>(pixels.samples[index]),
                static_cast<double>(pixels.samples[index + 1U]),
                static_cast<double>(pixels.samples[index + 2U]),
            },
            transform
        );
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            if (linear_srgb[channel] > static_cast<double>(std::numeric_limits<float>::max())
                || linear_srgb[channel] < -static_cast<double>(std::numeric_limits<float>::max())) {
                fail(
                    DcpColorDevelopmentErrorCode::invalid_input,
                    "DCP input rendering exceeded the fp32 scene-linear range"
                );
            }
            pixels.samples[index + channel] = static_cast<float>(linear_srgb[channel]);
        }
    });
    return DcpColorExecutionBackend::cpu;
}

DcpColorExecutionBackend apply_dcp_color_rendering_stages(
    PixelBuffer& pixels,
    const DcpColorTransform& transform
) {
    if (!transform.valid()) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP input rendering received an invalid compiled transform"
        );
    }
    if (!transform.has_post_matrix_stages()) {
        return DcpColorExecutionBackend::cpu;
    }
    const std::size_t expected_samples = static_cast<std::size_t>(pixels.dimensions.width)
        * static_cast<std::size_t>(pixels.dimensions.height) * 3U;
    if (pixels.bits_per_channel != 16U || pixels.channels != 3U
        || pixels.row_stride_bytes
            != static_cast<std::size_t>(pixels.dimensions.width) * 3U * sizeof(std::uint16_t)
        || pixels.primaries != RgbPrimaries::srgb_rec709_d65
        || pixels.transfer_function != RgbTransferFunction::linear
        || pixels.reference != RgbBufferReference::processed_raw
        || pixels.samples.size() != expected_samples) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP input rendering requires Shadow's canonical linear-sRGB RAW buffer"
        );
    }
    constexpr double u16_scale =
        1.0 / static_cast<double>(std::numeric_limits<std::uint16_t>::max());
    apply_dcp_to_pixels(pixels.samples.size() / 3U, [&](const std::size_t index) {
        const Vector3 linear_srgb = apply_dcp_post_matrix_stages(
            Vector3{
                static_cast<double>(pixels.samples[index]) * u16_scale,
                static_cast<double>(pixels.samples[index + 1U]) * u16_scale,
                static_cast<double>(pixels.samples[index + 2U]) * u16_scale,
            },
            transform
        );
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            const double encoded = std::round(clamp_unit(linear_srgb[channel]) / u16_scale);
            pixels.samples[index + channel] = static_cast<std::uint16_t>(encoded);
        }
    });
    return DcpColorExecutionBackend::cpu;
}

} // namespace shadow::image
