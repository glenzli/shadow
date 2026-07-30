#include <shadow/image/raw_white_balance.hpp>

#include "dcp_color_matrix_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <ranges>

namespace shadow::image {

namespace {

using detail::dcp_color_matrix_math::from_dcp;
using detail::dcp_color_matrix_math::interpolate;
using detail::dcp_color_matrix_math::invert;
using detail::dcp_color_matrix_math::Matrix3;
using detail::dcp_color_matrix_math::multiply;
using detail::dcp_color_matrix_math::Vector3;
using detail::dcp_color_matrix_math::xyz_d65_to_linear_srgb;

constexpr double tint_duv_per_unit = 0.0001;
constexpr double minimum_mired = 1'000'000.0
                                 / static_cast<double>(
                                     raw_white_balance_maximum_temperature_kelvin
                                 );
constexpr double maximum_mired = 1'000'000.0
                                 / static_cast<double>(
                                     raw_white_balance_minimum_temperature_kelvin
                                 );

struct Uv1960 final {
    double u = 0.0;
    double v = 0.0;
};

[[nodiscard]] std::optional<Vector3> xy_to_xyz(const double x, const double y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || x <= 0.0 || y <= 0.0 || x + y >= 1.0) {
        return std::nullopt;
    }
    return Vector3{x / y, 1.0, (1.0 - x - y) / y};
}

[[nodiscard]] std::optional<std::array<double, 2U>> xyz_to_xy(const Vector3& xyz) noexcept {
    const double sum = xyz[0] + xyz[1] + xyz[2];
    if (!std::isfinite(sum) || sum <= 0.0) {
        return std::nullopt;
    }
    const double x = xyz[0] / sum;
    const double y = xyz[1] / sum;
    if (!xy_to_xyz(x, y).has_value()) {
        return std::nullopt;
    }
    return std::array<double, 2U>{x, y};
}

[[nodiscard]] std::optional<Uv1960> xy_to_uv(const double x, const double y) noexcept {
    const double denominator = -2.0 * x + 12.0 * y + 3.0;
    if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-12) {
        return std::nullopt;
    }
    return Uv1960{
        .u = 4.0 * x / denominator,
        .v = 6.0 * y / denominator,
    };
}

[[nodiscard]] std::optional<std::array<double, 2U>>
uv_to_xy(const Uv1960 uv) noexcept {
    const double denominator = 2.0 * uv.u - 8.0 * uv.v + 4.0;
    if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-12) {
        return std::nullopt;
    }
    const double x = 3.0 * uv.u / denominator;
    const double y = 2.0 * uv.v / denominator;
    if (!xy_to_xyz(x, y).has_value()) {
        return std::nullopt;
    }
    return std::array<double, 2U>{x, y};
}

// Smooth analytic approximation to the Planckian/daylight photographic
// temperature locus over Shadow's bounded 2000..25000 K authoring range.
[[nodiscard]] std::optional<std::array<double, 2U>>
temperature_locus_xy(const double temperature_kelvin) noexcept {
    if (!std::isfinite(temperature_kelvin)
        || temperature_kelvin
               < static_cast<double>(raw_white_balance_minimum_temperature_kelvin)
        || temperature_kelvin
               > static_cast<double>(raw_white_balance_maximum_temperature_kelvin)) {
        return std::nullopt;
    }
    const double temperature2 = temperature_kelvin * temperature_kelvin;
    const double temperature3 = temperature2 * temperature_kelvin;
    const double x =
        temperature_kelvin <= 4'000.0
            ? -0.2661239e9 / temperature3 - 0.2343580e6 / temperature2
                  + 0.8776956e3 / temperature_kelvin + 0.179910
            : -3.0258469e9 / temperature3 + 2.1070379e6 / temperature2
                  + 0.2226347e3 / temperature_kelvin + 0.240390;
    double y = 0.0;
    if (temperature_kelvin <= 2'222.0) {
        y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x
            - 0.20219683;
    } else if (temperature_kelvin <= 4'000.0) {
        y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x
            - 0.16748867;
    } else {
        y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x
            - 0.37001483;
    }
    if (!xy_to_xyz(x, y).has_value()) {
        return std::nullopt;
    }
    return std::array<double, 2U>{x, y};
}

[[nodiscard]] std::optional<Uv1960> locus_uv_for_mired(const double mired) noexcept {
    if (!std::isfinite(mired) || mired < minimum_mired || mired > maximum_mired) {
        return std::nullopt;
    }
    const auto xy = temperature_locus_xy(1'000'000.0 / mired);
    return xy.has_value() ? xy_to_uv((*xy)[0], (*xy)[1]) : std::nullopt;
}

[[nodiscard]] std::optional<Uv1960> locus_normal_for_mired(const double mired) noexcept {
    const double delta = 0.25;
    const double low = std::max(minimum_mired, mired - delta);
    const double high = std::min(maximum_mired, mired + delta);
    const auto first = locus_uv_for_mired(low);
    const auto second = locus_uv_for_mired(high);
    if (!first.has_value() || !second.has_value()) {
        return std::nullopt;
    }
    const double tangent_u = second->u - first->u;
    const double tangent_v = second->v - first->v;
    const double length = std::hypot(tangent_u, tangent_v);
    if (!std::isfinite(length) || length <= 1.0e-12) {
        return std::nullopt;
    }
    return Uv1960{
        .u = -tangent_v / length,
        .v = tangent_u / length,
    };
}

[[nodiscard]] double squared_uv_distance(const Uv1960 left, const Uv1960 right) noexcept {
    const double du = left.u - right.u;
    const double dv = left.v - right.v;
    return du * du + dv * dv;
}

[[nodiscard]] std::optional<double> nearest_locus_mired(const Uv1960 target) noexcept {
    double low = minimum_mired;
    double high = maximum_mired;
    constexpr double golden = 0.6180339887498948482;
    double first = high - (high - low) * golden;
    double second = low + (high - low) * golden;
    auto first_uv = locus_uv_for_mired(first);
    auto second_uv = locus_uv_for_mired(second);
    if (!first_uv.has_value() || !second_uv.has_value()) {
        return std::nullopt;
    }
    for (std::size_t iteration = 0U; iteration < 64U; ++iteration) {
        if (squared_uv_distance(target, *first_uv)
            < squared_uv_distance(target, *second_uv)) {
            high = second;
            second = first;
            second_uv = first_uv;
            first = high - (high - low) * golden;
            first_uv = locus_uv_for_mired(first);
            if (!first_uv.has_value()) {
                return std::nullopt;
            }
        } else {
            low = first;
            first = second;
            first_uv = second_uv;
            second = low + (high - low) * golden;
            second_uv = locus_uv_for_mired(second);
            if (!second_uv.has_value()) {
                return std::nullopt;
            }
        }
    }
    return (low + high) * 0.5;
}

[[nodiscard]] std::optional<double>
illuminant_temperature(const std::uint16_t illuminant) noexcept {
    switch (illuminant) {
    case 1U:
    case 4U:
    case 9U:
        return 5'500.0;
    case 2U:
        return 4'230.0;
    case 3U:
        return 2'850.0;
    case 10U:
        return 6'500.0;
    case 11U:
        return 7'500.0;
    case 12U:
        return 6'400.0;
    case 13U:
        return 5'000.0;
    case 14U:
        return 4'200.0;
    case 15U:
        return 3'500.0;
    case 17U:
        return 2'856.0;
    case 18U:
        return 4'874.0;
    case 19U:
        return 6'774.0;
    case 20U:
        return 5'503.0;
    case 21U:
        return 6'504.0;
    case 22U:
        return 7'504.0;
    case 23U:
        return 5'003.0;
    case 24U:
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
    return std::clamp(
        ((1.0 / temperature) - (1.0 / temperature2))
            / ((1.0 / temperature1) - (1.0 / temperature2)),
        0.0,
        1.0
    );
}

[[nodiscard]] std::optional<Matrix3>
dcp_color_matrix_for_temperature(const DcpProfile& profile, const double temperature) noexcept {
    const Matrix3 first = from_dcp(profile.calibration1.color_matrix);
    if (!profile.calibration2.has_value()) {
        return first;
    }
    const auto first_temperature = illuminant_temperature(profile.calibration1.illuminant);
    const auto second_temperature = illuminant_temperature(profile.calibration2->illuminant);
    if (!first_temperature.has_value() || !second_temperature.has_value()) {
        return std::nullopt;
    }
    const double weight =
        reciprocal_temperature_weight(temperature, *first_temperature, *second_temperature);
    return interpolate(first, from_dcp(profile.calibration2->color_matrix), weight);
}

[[nodiscard]] std::optional<Vector3>
normalized_positive_camera_neutral(Vector3 neutral) noexcept {
    if (!std::ranges::all_of(neutral, [](const double value) {
            return std::isfinite(value) && value > 0.0;
        })
        || neutral[1] <= 0.0) {
        return std::nullopt;
    }
    for (double& value : neutral) {
        value /= neutral[1];
    }
    return neutral;
}

[[nodiscard]] std::optional<Vector3> camera_neutral_from_matrix(
    const Matrix3& camera_to_output,
    const Vector3& output_white
) noexcept {
    try {
        return normalized_positive_camera_neutral(
            multiply(invert(camera_to_output), output_white)
        );
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

bool RawWhiteBalancePresentation::valid() const noexcept {
    return std::isfinite(temperature_kelvin)
           && temperature_kelvin
                  >= static_cast<double>(raw_white_balance_minimum_temperature_kelvin)
           && temperature_kelvin
                  <= static_cast<double>(raw_white_balance_maximum_temperature_kelvin)
           && std::isfinite(tint)
           && xy_to_xyz(white_x, white_y).has_value();
}

std::optional<std::array<double, 2U>>
raw_white_xy_from_temperature_tint(
    const double temperature_kelvin,
    const double tint
) noexcept {
    if (!std::isfinite(tint)
        || tint < static_cast<double>(raw_white_balance_minimum_tint)
        || tint > static_cast<double>(raw_white_balance_maximum_tint)) {
        return std::nullopt;
    }
    const auto locus_xy = temperature_locus_xy(temperature_kelvin);
    if (!locus_xy.has_value()) {
        return std::nullopt;
    }
    const auto locus_uv = xy_to_uv((*locus_xy)[0], (*locus_xy)[1]);
    const double mired = 1'000'000.0 / temperature_kelvin;
    const auto normal = locus_normal_for_mired(mired);
    if (!locus_uv.has_value() || !normal.has_value()) {
        return std::nullopt;
    }
    // Positive photographic tint is magenta, opposite the positive Duv
    // direction used by the chosen locus normal.
    const double displacement = -tint * tint_duv_per_unit;
    return uv_to_xy(Uv1960{
        .u = locus_uv->u + normal->u * displacement,
        .v = locus_uv->v + normal->v * displacement,
    });
}

std::optional<RawWhiteBalancePresentation>
raw_white_balance_presentation_from_xy(const double white_x, const double white_y) noexcept {
    const auto target = xy_to_uv(white_x, white_y);
    if (!target.has_value()) {
        return std::nullopt;
    }
    const auto mired = nearest_locus_mired(*target);
    if (!mired.has_value()) {
        return std::nullopt;
    }
    const auto locus = locus_uv_for_mired(*mired);
    const auto normal = locus_normal_for_mired(*mired);
    if (!locus.has_value() || !normal.has_value()) {
        return std::nullopt;
    }
    const double du = target->u - locus->u;
    const double dv = target->v - locus->v;
    const double tint = -(du * normal->u + dv * normal->v) / tint_duv_per_unit;
    RawWhiteBalancePresentation result{
        .temperature_kelvin = 1'000'000.0 / *mired,
        .tint = tint,
        .white_x = white_x,
        .white_y = white_y,
    };
    if (!result.valid()) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::array<double, 3U>>
raw_as_shot_camera_neutral(const RawFrameDescriptor& descriptor) noexcept {
    if (descriptor.cfa_layout != RawFrameCfaLayout::bayer_2x2) {
        return std::nullopt;
    }
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
            return std::nullopt;
        }
        const double value = descriptor.as_shot_neutral[site];
        if (!std::isfinite(value) || value <= 0.0) {
            return std::nullopt;
        }
        totals[channel] += value;
        ++counts[channel];
    }
    Vector3 neutral{};
    for (std::size_t channel = 0U; channel < neutral.size(); ++channel) {
        if (counts[channel] == 0U) {
            return std::nullopt;
        }
        neutral[channel] = totals[channel] / static_cast<double>(counts[channel]);
    }
    return normalized_positive_camera_neutral(neutral);
}

std::optional<std::array<double, 3U>> raw_dcp_camera_neutral(
    const DcpProfile& profile,
    const RawWhiteBalance& white_balance
) noexcept {
    if (!valid_raw_white_balance(white_balance)
        || white_balance.mode != RawWhiteBalanceMode::temperature_tint) {
        return std::nullopt;
    }
    const auto xy = raw_white_xy_from_temperature_tint(
        static_cast<double>(white_balance.temperature_kelvin),
        static_cast<double>(white_balance.tint)
    );
    if (!xy.has_value()) {
        return std::nullopt;
    }
    const auto white_xyz = xy_to_xyz((*xy)[0], (*xy)[1]);
    const auto color_matrix = dcp_color_matrix_for_temperature(
        profile,
        static_cast<double>(white_balance.temperature_kelvin)
    );
    if (!white_xyz.has_value() || !color_matrix.has_value()) {
        return std::nullopt;
    }
    return normalized_positive_camera_neutral(multiply(*color_matrix, *white_xyz));
}

std::optional<RawWhiteBalancePresentation> raw_dcp_white_balance_presentation(
    const DcpProfile& profile,
    const std::array<double, 3U>& camera_neutral
) noexcept {
    auto neutral = normalized_positive_camera_neutral(camera_neutral);
    if (!neutral.has_value()) {
        return std::nullopt;
    }
    double temperature = 5'000.0;
    std::optional<std::array<double, 2U>> white_xy;
    try {
        for (std::size_t iteration = 0U; iteration < 24U; ++iteration) {
            const auto color_matrix = dcp_color_matrix_for_temperature(profile, temperature);
            if (!color_matrix.has_value()) {
                return std::nullopt;
            }
            white_xy = xyz_to_xy(multiply(invert(*color_matrix), *neutral));
            if (!white_xy.has_value()) {
                return std::nullopt;
            }
            const auto presentation =
                raw_white_balance_presentation_from_xy((*white_xy)[0], (*white_xy)[1]);
            if (!presentation.has_value()) {
                return std::nullopt;
            }
            if (std::abs(presentation->temperature_kelvin - temperature) <= 1.0e-7) {
                return presentation;
            }
            temperature = presentation->temperature_kelvin;
        }
    } catch (...) {
        return std::nullopt;
    }
    return white_xy.has_value()
               ? raw_white_balance_presentation_from_xy((*white_xy)[0], (*white_xy)[1])
               : std::nullopt;
}

std::optional<std::array<double, 3U>> raw_frame_camera_neutral(
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance
) noexcept {
    if (!valid_raw_white_balance(white_balance)) {
        return std::nullopt;
    }
    if (white_balance.mode == RawWhiteBalanceMode::as_shot) {
        return raw_as_shot_camera_neutral(descriptor);
    }
    const auto xy = raw_white_xy_from_temperature_tint(
        static_cast<double>(white_balance.temperature_kelvin),
        static_cast<double>(white_balance.tint)
    );
    if (!xy.has_value()) {
        return std::nullopt;
    }
    const auto white_xyz = xy_to_xyz((*xy)[0], (*xy)[1]);
    if (!white_xyz.has_value()) {
        return std::nullopt;
    }
    if (descriptor.has_camera_to_xyz_d50) {
        Matrix3 camera_to_xyz{};
        for (std::size_t input = 0U; input < 3U; ++input) {
            for (std::size_t output = 0U; output < 3U; ++output) {
                camera_to_xyz[output * 3U + input] =
                    descriptor.camera_to_xyz_d50[input * 3U + output];
            }
        }
        return camera_neutral_from_matrix(camera_to_xyz, *white_xyz);
    }
    if (descriptor.has_camera_to_linear_srgb_d65) {
        return camera_neutral_from_matrix(
            descriptor.camera_to_linear_srgb_d65,
            multiply(xyz_d65_to_linear_srgb, *white_xyz)
        );
    }
    return std::nullopt;
}

} // namespace shadow::image
