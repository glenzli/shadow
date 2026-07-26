#include <shadow/image/dcp_color_development.hpp>

#include "metal_raw_development.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <thread>

namespace shadow::image {

namespace {

using Matrix3 = std::array<double, 9U>;
using Vector3 = std::array<double, 3U>;

inline constexpr Vector3 d50_xyz{0.964295676, 1.0, 0.825104603};
inline constexpr Vector3 d65_xyz{0.95047, 1.0, 1.08883};
inline constexpr Matrix3 bradford{
    0.8951, 0.2664, -0.1614,
    -0.7502, 1.7135, 0.0367,
    0.0389, -0.0685, 1.0296,
};
inline constexpr Matrix3 bradford_inverse{
    0.986992905, -0.147054256, 0.159962652,
    0.432305269, 0.518360272, 0.049291228,
    -0.008528665, 0.040042821, 0.968486696,
};
inline constexpr Matrix3 xyz_d65_to_linear_srgb{
    3.2404542, -1.5371385, -0.4985314,
    -0.9692660, 1.8760108, 0.0415560,
    0.0556434, -0.2040259, 1.0572252,
};
inline constexpr Matrix3 linear_srgb_to_xyz_d65{
    0.4124564, 0.3575761, 0.1804375,
    0.2126729, 0.7151522, 0.0721750,
    0.0193339, 0.1191920, 0.9503041,
};
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

[[nodiscard]] bool finite_matrix(const Matrix3& matrix) noexcept {
    return std::ranges::all_of(matrix, [](const double value) {
        return std::isfinite(value);
    });
}

[[nodiscard]] Matrix3 from_dcp(const DcpMatrix3x3& matrix) noexcept {
    return matrix.row_major;
}

[[nodiscard]] Vector3 multiply(
    const Matrix3& matrix,
    const Vector3& vector
) noexcept {
    Vector3 result{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            result[row] += matrix[row * 3U + column] * vector[column];
        }
    }
    return result;
}

[[nodiscard]] Matrix3 multiply(
    const Matrix3& left,
    const Matrix3& right
) noexcept {
    Matrix3 result{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            for (std::size_t inner = 0U; inner < 3U; ++inner) {
                result[row * 3U + column] +=
                    left[row * 3U + inner] * right[inner * 3U + column];
            }
        }
    }
    return result;
}

[[nodiscard]] Matrix3 scale_matrix(const Matrix3& matrix, const double scale) noexcept {
    Matrix3 result = matrix;
    for (double& value : result) {
        value *= scale;
    }
    return result;
}

[[nodiscard]] Matrix3 interpolate(
    const Matrix3& first,
    const Matrix3& second,
    const double first_weight
) noexcept {
    Matrix3 result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] =
            first[index] * first_weight + second[index] * (1.0 - first_weight);
    }
    return result;
}

[[nodiscard]] Matrix3 invert(const Matrix3& matrix) {
    const double determinant =
        matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7])
        - matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6])
        + matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    double largest = 0.0;
    for (const double value : matrix) {
        largest = std::max(largest, std::abs(value));
    }
    const double scale = std::max(1.0, largest * largest * largest);
    if (!std::isfinite(determinant)
        || std::abs(determinant) <= std::numeric_limits<double>::epsilon() * scale * 64.0) {
        fail(DcpColorDevelopmentErrorCode::singular_matrix, "DCP color matrix is singular");
    }
    const double reciprocal = 1.0 / determinant;
    return {
        (matrix[4] * matrix[8] - matrix[5] * matrix[7]) * reciprocal,
        (matrix[2] * matrix[7] - matrix[1] * matrix[8]) * reciprocal,
        (matrix[1] * matrix[5] - matrix[2] * matrix[4]) * reciprocal,
        (matrix[5] * matrix[6] - matrix[3] * matrix[8]) * reciprocal,
        (matrix[0] * matrix[8] - matrix[2] * matrix[6]) * reciprocal,
        (matrix[2] * matrix[3] - matrix[0] * matrix[5]) * reciprocal,
        (matrix[3] * matrix[7] - matrix[4] * matrix[6]) * reciprocal,
        (matrix[1] * matrix[6] - matrix[0] * matrix[7]) * reciprocal,
        (matrix[0] * matrix[4] - matrix[1] * matrix[3]) * reciprocal,
    };
}

[[nodiscard]] Vector3 canonical_camera_neutral(
    const RawFrameDescriptor& descriptor
) {
    std::array<double, 3U> totals{};
    std::array<std::uint32_t, 3U> counts{};
    for (std::size_t site = 0U; site < descriptor.bayer_2x2.size(); ++site) {
        std::size_t channel = 0U;
        switch (descriptor.bayer_2x2[site]) {
        case RawCfaColor::red:
            channel = 0U;
            break;
        case RawCfaColor::green:
            channel = 1U;
            break;
        case RawCfaColor::blue:
            channel = 2U;
            break;
        case RawCfaColor::unknown:
            fail(
                DcpColorDevelopmentErrorCode::invalid_input,
                "DCP development requires an RGB Bayer camera neutral"
            );
        }
        const double neutral = descriptor.as_shot_neutral[site];
        if (!std::isfinite(neutral) || neutral <= 0.0) {
            fail(
                DcpColorDevelopmentErrorCode::invalid_input,
                "DCP development requires a positive finite camera neutral"
            );
        }
        totals[channel] += neutral;
        ++counts[channel];
    }
    if (counts[0] == 0U || counts[1] == 0U || counts[2] == 0U) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP development camera neutral does not cover RGB"
        );
    }
    Vector3 neutral{
        totals[0] / static_cast<double>(counts[0]),
        totals[1] / static_cast<double>(counts[1]),
        totals[2] / static_cast<double>(counts[2]),
    };
    // AsShotNeutral is defined only up to a common scale. Shadow's generic path preserves the
    // green camera channel, so use the same convention before white-point normalization.
    for (double& value : neutral) {
        value /= neutral[1];
    }
    return neutral;
}

[[nodiscard]] Vector3 xy_to_xyz(const double x, const double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || x <= 0.0 || y <= 0.0
        || x + y >= 1.0) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_white_point,
            "DCP white chromaticity is outside the finite visible triangle"
        );
    }
    return {x / y, 1.0, (1.0 - x - y) / y};
}

[[nodiscard]] std::array<double, 2U> xyz_to_xy(const Vector3& xyz) {
    const double sum = xyz[0] + xyz[1] + xyz[2];
    if (!std::isfinite(sum) || sum <= 0.0) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_white_point,
            "DCP matrix and camera neutral do not produce a positive white point"
        );
    }
    const double x = xyz[0] / sum;
    const double y = xyz[1] / sum;
    static_cast<void>(xy_to_xyz(x, y));
    return {x, y};
}

[[nodiscard]] double correlated_color_temperature(
    const double x,
    const double y
) {
    // McCamy's compact approximation is deterministic and sufficiently accurate for choosing
    // the reciprocal-temperature interpolation weight between standard DCP illuminants.
    const double denominator = 0.1858 - y;
    if (std::abs(denominator) <= 1.0e-12) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_white_point,
            "DCP white point cannot be mapped to a finite color temperature"
        );
    }
    const double n = (x - 0.3320) / denominator;
    const double temperature =
        -449.0 * n * n * n + 3525.0 * n * n - 6823.3 * n + 5520.33;
    if (!std::isfinite(temperature) || temperature < 1'500.0 || temperature > 25'000.0) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_white_point,
            "DCP estimated white temperature is outside the supported photographic range"
        );
    }
    return temperature;
}

[[nodiscard]] std::optional<double> illuminant_temperature(
    const std::uint16_t illuminant
) noexcept {
    switch (illuminant) {
    case 1U: // Daylight
    case 4U: // Flash
    case 9U: // Fine weather
        return 5'500.0;
    case 2U: // Fluorescent
        return 4'230.0;
    case 3U: // Tungsten
        return 2'850.0;
    case 10U: // Cloudy weather
        return 6'500.0;
    case 11U: // Shade
        return 7'500.0;
    case 12U: // Daylight fluorescent
        return 6'400.0;
    case 13U: // Day white fluorescent
        return 5'000.0;
    case 14U: // Cool white fluorescent
        return 4'200.0;
    case 15U: // White fluorescent
        return 3'500.0;
    case 17U: // Standard light A
        return 2'856.0;
    case 18U: // Standard light B
        return 4'874.0;
    case 19U: // Standard light C
        return 6'774.0;
    case 20U: // D55
        return 5'503.0;
    case 21U: // D65
        return 6'504.0;
    case 22U: // D75
        return 7'504.0;
    case 23U: // D50
        return 5'003.0;
    case 24U: // ISO studio tungsten
        return 3'200.0;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] double reciprocal_temperature_weight(
    const double temperature,
    const double temperature1,
    const double temperature2
) noexcept {
    if (temperature1 == temperature2) {
        return 1.0;
    }
    const double weight =
        ((1.0 / temperature) - (1.0 / temperature2))
        / ((1.0 / temperature1) - (1.0 / temperature2));
    return std::clamp(weight, 0.0, 1.0);
}

[[nodiscard]] Matrix3 chromatic_adaptation(
    const Vector3& source_white,
    const Vector3& destination_white
) {
    const Vector3 source_cone = multiply(bradford, source_white);
    const Vector3 destination_cone = multiply(bradford, destination_white);
    Matrix3 scale{};
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        if (!std::isfinite(source_cone[channel]) || source_cone[channel] <= 0.0
            || !std::isfinite(destination_cone[channel])
            || destination_cone[channel] <= 0.0) {
            fail(
                DcpColorDevelopmentErrorCode::invalid_white_point,
                "DCP Bradford adaptation received a non-positive cone response"
            );
        }
        scale[channel * 3U + channel] = destination_cone[channel] / source_cone[channel];
    }
    return multiply(bradford_inverse, multiply(scale, bradford));
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

struct ResolvedCalibration final {
    Matrix3 color_matrix;
    std::optional<Matrix3> forward_matrix;
    Vector3 white_xyz;
    double temperature = 0.0;
    double calibration1_weight = 1.0;
};

[[nodiscard]] ResolvedCalibration resolve_calibration(
    const DcpProfile& profile,
    const Vector3& camera_neutral
) {
    const Matrix3 color1 = from_dcp(profile.calibration1.color_matrix);
    if (!profile.calibration2.has_value()) {
        const Vector3 source_xyz = multiply(invert(color1), camera_neutral);
        const auto xy = xyz_to_xy(source_xyz);
        const double temperature = correlated_color_temperature(xy[0], xy[1]);
        return ResolvedCalibration{
            .color_matrix = color1,
            .forward_matrix = profile.calibration1.forward_matrix.has_value()
                ? std::optional<Matrix3>(from_dcp(*profile.calibration1.forward_matrix))
                : std::nullopt,
            .white_xyz = xy_to_xyz(xy[0], xy[1]),
            .temperature = temperature,
            .calibration1_weight = 1.0,
        };
    }

    const auto temperature1 = illuminant_temperature(profile.calibration1.illuminant);
    const auto temperature2 = illuminant_temperature(profile.calibration2->illuminant);
    if (!temperature1.has_value() || !temperature2.has_value()) {
        fail(
            DcpColorDevelopmentErrorCode::unsupported_illuminant,
            "dual-illuminant DCP uses a light source outside Shadow's v1 standard set"
        );
    }
    const Matrix3 color2 = from_dcp(profile.calibration2->color_matrix);
    double weight = 0.5;
    double temperature = 5'000.0;
    Vector3 white_xyz = d50_xyz;
    for (std::size_t iteration = 0U; iteration < 24U; ++iteration) {
        const Matrix3 color = interpolate(color1, color2, weight);
        const Vector3 source_xyz = multiply(invert(color), camera_neutral);
        const auto xy = xyz_to_xy(source_xyz);
        white_xyz = xy_to_xyz(xy[0], xy[1]);
        temperature = correlated_color_temperature(xy[0], xy[1]);
        const double next_weight =
            reciprocal_temperature_weight(temperature, *temperature1, *temperature2);
        if (std::abs(next_weight - weight) <= 1.0e-10) {
            weight = next_weight;
            break;
        }
        weight = next_weight;
    }

    std::optional<Matrix3> forward;
    const auto& forward1 = profile.calibration1.forward_matrix;
    const auto& forward2 = profile.calibration2->forward_matrix;
    if (forward1.has_value() && forward2.has_value()) {
        forward = interpolate(from_dcp(*forward1), from_dcp(*forward2), weight);
    } else if (forward1.has_value()) {
        forward = from_dcp(*forward1);
    } else if (forward2.has_value()) {
        forward = from_dcp(*forward2);
    }
    return ResolvedCalibration{
        .color_matrix = interpolate(color1, color2, weight),
        .forward_matrix = forward,
        .white_xyz = white_xyz,
        .temperature = temperature,
        .calibration1_weight = weight,
    };
}

[[nodiscard]] Matrix3 normalized_camera_to_xyz_d50(
    const ResolvedCalibration& calibration,
    const Vector3& camera_neutral,
    DcpMatrixRoute& route
) {
    Matrix3 camera_to_d50{};
    if (calibration.forward_matrix.has_value()) {
        route = DcpMatrixRoute::forward_matrix;
        Vector3 camera_white = multiply(calibration.color_matrix, calibration.white_xyz);
        if (!std::isfinite(camera_white[1]) || camera_white[1] <= 0.0) {
            fail(
                DcpColorDevelopmentErrorCode::invalid_white_point,
                "DCP ForwardMatrix calibration produced an invalid camera white"
            );
        }
        for (double& value : camera_white) {
            value /= camera_white[1];
        }
        Matrix3 white_balance{};
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            if (!std::isfinite(camera_white[channel]) || camera_white[channel] <= 0.0) {
                fail(
                    DcpColorDevelopmentErrorCode::invalid_white_point,
                    "DCP ForwardMatrix camera white has a non-positive channel"
                );
            }
            white_balance[channel * 3U + channel] = 1.0 / camera_white[channel];
        }
        camera_to_d50 = multiply(*calibration.forward_matrix, white_balance);
    } else {
        route = DcpMatrixRoute::inverse_color_matrix;
        camera_to_d50 = multiply(
            chromatic_adaptation(calibration.white_xyz, d50_xyz),
            invert(calibration.color_matrix)
        );
    }

    // ColorMatrix/ForwardMatrix are chromatic calibrations; their common scalar is not a
    // photographic exposure. Normalize so the as-shot neutral maps to D50 with Y=1, retaining
    // Shadow's green-referenced sensor exposure convention.
    const Vector3 mapped_white = multiply(camera_to_d50, camera_neutral);
    if (!std::isfinite(mapped_white[1]) || mapped_white[1] <= 0.0) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_white_point,
            "DCP transform maps the camera neutral to a non-positive luminance"
        );
    }
    return scale_matrix(camera_to_d50, 1.0 / mapped_white[1]);
}

[[nodiscard]] const char* matrix_route_name(const DcpMatrixRoute route) noexcept {
    switch (route) {
    case DcpMatrixRoute::forward_matrix:
        return "forward-matrix";
    case DcpMatrixRoute::inverse_color_matrix:
        return "inverse-color-matrix";
    }
    return "unknown";
}

} // namespace

DcpColorDevelopmentError::DcpColorDevelopmentError(
    const DcpColorDevelopmentErrorCode code,
    std::string message
)
    : std::invalid_argument(std::move(message)),
      code_(code) {
}

DcpColorDevelopmentErrorCode DcpColorDevelopmentError::code() const noexcept {
    return code_;
}

std::string_view dcp_color_execution_backend_identity(
    const DcpColorExecutionBackend backend
) noexcept {
    switch (backend) {
    case DcpColorExecutionBackend::cpu:
        return "dcp-executor=cpu-v1;math=f64-reference";
    case DcpColorExecutionBackend::metal:
        return "dcp-executor=metal-v1;math=f32";
    }
    return "dcp-executor=unknown";
}

bool DcpColorDevelopmentReceipt::valid() const noexcept {
    return schema_version == dcp_color_receipt_schema_version
        && developer_version == dcp_color_developer_version
        && !profile_content_identity.empty() && !normalized_camera_model.empty()
        && std::isfinite(calibration1_weight) && calibration1_weight >= 0.0
        && calibration1_weight <= 1.0 && std::isfinite(estimated_white_x)
        && std::isfinite(estimated_white_y) && estimated_white_x > 0.0
        && estimated_white_y > 0.0 && estimated_white_x + estimated_white_y < 1.0
        && std::isfinite(estimated_correlated_color_temperature)
        && estimated_correlated_color_temperature > 0.0
        && std::isfinite(baseline_exposure_offset_ev);
}

bool DcpColorTransform::valid() const noexcept {
    return receipt.valid() && finite_matrix(camera_to_linear_srgb_d65)
        && receipt.hue_sat_map_applied == hue_sat_map.has_value()
        && receipt.look_table_applied == look_table.has_value()
        && receipt.tone_curve_applied == !tone_curve.empty()
        && (tone_curve.empty() || tone_curve_second_derivatives.size() == tone_curve.size());
}

std::array<double, 3U> DcpColorTransform::apply(
    const std::array<double, 3U>& camera_rgb
) const noexcept {
    return multiply(camera_to_linear_srgb_d65, camera_rgb);
}

bool DcpColorTransform::has_post_matrix_stages() const noexcept {
    return hue_sat_map.has_value() || look_table.has_value() || !tone_curve.empty();
}

DcpColorTransform compile_dcp_color_transform(
    const CameraProfileDefinition& definition,
    const RawFrameDescriptor& descriptor
) {
    if (definition.content_identity.empty() || definition.normalized_camera_model.empty()
        || descriptor.cfa_layout != RawFrameCfaLayout::bayer_2x2) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP development requires an identified profile and Bayer RawFrame"
        );
    }
    const DcpProfile& profile = definition.profile;
    const Vector3 neutral = canonical_camera_neutral(descriptor);
    const ResolvedCalibration calibration = resolve_calibration(profile, neutral);
    DcpMatrixRoute route = DcpMatrixRoute::inverse_color_matrix;
    Matrix3 camera_to_d50 = normalized_camera_to_xyz_d50(calibration, neutral, route);
    const Matrix3 d50_to_d65 = chromatic_adaptation(d50_xyz, d65_xyz);
    Matrix3 camera_to_srgb = multiply(
        xyz_d65_to_linear_srgb,
        multiply(d50_to_d65, camera_to_d50)
    );
    const double exposure_offset = profile.baseline_exposure_offset_ev.value_or(0.0);
    if (!std::isfinite(exposure_offset) || std::abs(exposure_offset) > 16.0) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP BaselineExposureOffset is outside the supported finite range"
        );
    }
    camera_to_srgb = scale_matrix(camera_to_srgb, std::exp2(exposure_offset));
    const auto white_xy = xyz_to_xy(calibration.white_xyz);
    const std::optional<DcpHsvTable> hue_sat_map =
        resolve_hue_sat_map(profile, calibration.calibration1_weight);
    if (profile.look_table.has_value()) {
        validate_hsv_table(*profile.look_table, "LookTable");
    }
    validate_tone_curve(profile.tone_curve);
    const std::vector<double> tone_curve_second_derivatives =
        natural_spline_second_derivatives(profile.tone_curve);

    DcpColorTransform result{
        .camera_to_linear_srgb_d65 = camera_to_srgb,
        .hue_sat_map = hue_sat_map,
        .look_table = profile.look_table,
        .tone_curve = profile.tone_curve,
        .tone_curve_second_derivatives = tone_curve_second_derivatives,
        .receipt = DcpColorDevelopmentReceipt{
            .schema_version = dcp_color_receipt_schema_version,
            .developer_version = dcp_color_developer_version,
            .profile_content_identity = definition.content_identity,
            .normalized_camera_model = definition.normalized_camera_model,
            .matrix_route = route,
            .calibration_illuminant1 = profile.calibration1.illuminant,
            .calibration_illuminant2 = profile.calibration2.has_value()
                ? profile.calibration2->illuminant : static_cast<std::uint16_t>(0U),
            .calibration1_weight = calibration.calibration1_weight,
            .estimated_white_x = white_xy[0],
            .estimated_white_y = white_xy[1],
            .estimated_correlated_color_temperature = calibration.temperature,
            .baseline_exposure_offset_ev = exposure_offset,
            .hue_sat_map_applied = hue_sat_map.has_value(),
            .look_table_applied = profile.look_table.has_value(),
            .tone_curve_applied = !profile.tone_curve.empty(),
        },
    };
    if (!result.valid()) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP color developer produced an invalid transform"
        );
    }
    return result;
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
    constexpr double u16_scale = 1.0 / static_cast<double>(std::numeric_limits<std::uint16_t>::max());
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

std::string dcp_color_receipt_identity(const DcpColorDevelopmentReceipt& receipt) {
    if (!receipt.valid()) {
        throw std::invalid_argument("DCP color development receipt is invalid");
    }
    std::ostringstream identity;
    identity << "shadow-dcp-color-v" << receipt.developer_version
             << ";profile=" << receipt.profile_content_identity
             << ";camera=" << receipt.normalized_camera_model
             << ";route=" << matrix_route_name(receipt.matrix_route)
             << ";illuminant1=" << receipt.calibration_illuminant1
             << ";illuminant2=" << receipt.calibration_illuminant2
             << ";weight1=" << std::setprecision(17) << receipt.calibration1_weight
             << ";white-x=" << receipt.estimated_white_x
             << ";white-y=" << receipt.estimated_white_y
             << ";cct=" << receipt.estimated_correlated_color_temperature
             << ";baseline-ev=" << receipt.baseline_exposure_offset_ev
             << ";huesat=" << (receipt.hue_sat_map_applied ? "applied" : "none")
             << ";look=" << (receipt.look_table_applied ? "applied" : "none")
             << ";tone=" << (receipt.tone_curve_applied ? "applied" : "none");
    return identity.str();
}

} // namespace shadow::image
