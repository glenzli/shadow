#include <shadow/image/dcp_color_development.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>

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
    return receipt.valid() && finite_matrix(camera_to_linear_srgb_d65);
}

std::array<double, 3U> DcpColorTransform::apply(
    const std::array<double, 3U>& camera_rgb
) const noexcept {
    return multiply(camera_to_linear_srgb_d65, camera_rgb);
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
    if (!profile.tone_curve.empty() || profile.look_table.has_value()
        || profile.calibration1.hue_sat_map.has_value()
        || (
            profile.calibration2.has_value()
            && profile.calibration2->hue_sat_map.has_value()
        )) {
        fail(
            DcpColorDevelopmentErrorCode::unsupported_rendering_feature,
            "profile contains HueSatMap, LookTable, or ToneCurve stages not implemented by DCP developer v1"
        );
    }
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

    DcpColorTransform result{
        .camera_to_linear_srgb_d65 = camera_to_srgb,
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
             << ";baseline-ev=" << receipt.baseline_exposure_offset_ev;
    return identity.str();
}

} // namespace shadow::image
