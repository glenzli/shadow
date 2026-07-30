#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/raw_white_balance.hpp>

#include "dcp_color_matrix_math.hpp"
#include "dcp_color_rendering.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::image {

namespace {

using detail::dcp_color_matrix_math::chromatic_adaptation;
using detail::dcp_color_matrix_math::d50_xyz;
using detail::dcp_color_matrix_math::d65_xyz;
using detail::dcp_color_matrix_math::finite_matrix;
using detail::dcp_color_matrix_math::from_dcp;
using detail::dcp_color_matrix_math::interpolate;
using detail::dcp_color_matrix_math::invert;
using detail::dcp_color_matrix_math::Matrix3;
using detail::dcp_color_matrix_math::multiply;
using detail::dcp_color_matrix_math::scale_matrix;
using detail::dcp_color_matrix_math::Vector3;
using detail::dcp_color_matrix_math::xyz_d65_to_linear_srgb;

[[noreturn]] void fail(const DcpColorDevelopmentErrorCode code, const std::string_view message) {
    throw DcpColorDevelopmentError(code, std::string(message));
}

[[nodiscard]] Vector3 canonical_camera_neutral(
    const DcpProfile& profile,
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance
) {
    if (!valid_raw_white_balance(white_balance)) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP development requires a canonical RAW white balance"
        );
    }
    const auto neutral =
        white_balance.mode == RawWhiteBalanceMode::as_shot
            ? raw_as_shot_camera_neutral(descriptor)
            : raw_dcp_camera_neutral(profile, white_balance);
    if (!neutral.has_value()) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP development cannot resolve the requested photographic white balance"
        );
    }
    return *neutral;
}

[[nodiscard]] Vector3 xy_to_xyz(const double x, const double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || x <= 0.0 || y <= 0.0 || x + y >= 1.0) {
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

[[nodiscard]] double correlated_color_temperature(const double x, const double y) {
    const auto presentation = raw_white_balance_presentation_from_xy(x, y);
    if (!presentation.has_value()) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_white_point,
            "DCP white point cannot be mapped to the supported photographic temperature locus"
        );
    }
    return presentation->temperature_kelvin;
}

[[nodiscard]] std::optional<double>
illuminant_temperature(const std::uint16_t illuminant) noexcept {
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
    const double weight = ((1.0 / temperature) - (1.0 / temperature2))
                          / ((1.0 / temperature1) - (1.0 / temperature2));
    return std::clamp(weight, 0.0, 1.0);
}

struct ResolvedCalibration final {
    Matrix3 color_matrix;
    std::optional<Matrix3> forward_matrix;
    Vector3 white_xyz;
    double temperature = 0.0;
    double calibration1_weight = 1.0;
};

[[nodiscard]] ResolvedCalibration
resolve_calibration(const DcpProfile& profile, const Vector3& camera_neutral) {
    const Matrix3 color1 = from_dcp(profile.calibration1.color_matrix);
    if (!profile.calibration2.has_value()) {
        const Vector3 source_xyz = multiply(invert(color1), camera_neutral);
        const auto xy = xyz_to_xy(source_xyz);
        const double temperature = correlated_color_temperature(xy[0], xy[1]);
        return ResolvedCalibration{
            .color_matrix = color1,
            .forward_matrix =
                profile.calibration1.forward_matrix.has_value()
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
) : std::invalid_argument(std::move(message)), code_(code) {}

DcpColorDevelopmentErrorCode DcpColorDevelopmentError::code() const noexcept {
    return code_;
}

std::string_view
dcp_color_execution_backend_identity(const DcpColorExecutionBackend backend) noexcept {
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
           && developer_version == dcp_color_developer_version && !profile_content_identity.empty()
           && !normalized_camera_model.empty() && std::isfinite(calibration1_weight)
           && calibration1_weight >= 0.0 && calibration1_weight <= 1.0
           && std::isfinite(estimated_white_x) && std::isfinite(estimated_white_y)
           && estimated_white_x > 0.0 && estimated_white_y > 0.0
           && estimated_white_x + estimated_white_y < 1.0
           && std::isfinite(estimated_correlated_color_temperature)
           && estimated_correlated_color_temperature > 0.0
           && std::isfinite(baseline_exposure_offset_ev);
}

bool DcpColorTransform::valid() const noexcept {
    return receipt.valid() && finite_matrix(camera_to_linear_srgb_d65)
           && detail::dcp_rendering_stages_valid(*this);
}

std::array<double, 3U>
DcpColorTransform::apply(const std::array<double, 3U>& camera_rgb) const noexcept {
    return multiply(camera_to_linear_srgb_d65, camera_rgb);
}

bool DcpColorTransform::has_post_matrix_stages() const noexcept {
    return hue_sat_map.has_value() || look_table.has_value() || !tone_curve.empty();
}

DcpColorTransform compile_dcp_color_transform(
    const CameraProfileDefinition& definition,
    const RawFrameDescriptor& descriptor
) {
    return compile_dcp_color_transform(definition, descriptor, RawWhiteBalance{});
}

DcpColorTransform compile_dcp_color_transform(
    const CameraProfileDefinition& definition,
    const RawFrameDescriptor& descriptor,
    const RawWhiteBalance& white_balance
) {
    if (definition.content_identity.empty() || definition.normalized_camera_model.empty()
        || descriptor.cfa_layout != RawFrameCfaLayout::bayer_2x2) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP development requires an identified profile and Bayer RawFrame"
        );
    }
    const DcpProfile& profile = definition.profile;
    const Vector3 neutral = canonical_camera_neutral(profile, descriptor, white_balance);
    const ResolvedCalibration calibration = resolve_calibration(profile, neutral);
    DcpMatrixRoute route = DcpMatrixRoute::inverse_color_matrix;
    Matrix3 camera_to_d50 = normalized_camera_to_xyz_d50(calibration, neutral, route);
    const Matrix3 d50_to_d65 = chromatic_adaptation(d50_xyz, d65_xyz);
    Matrix3 camera_to_srgb = multiply(xyz_d65_to_linear_srgb, multiply(d50_to_d65, camera_to_d50));
    const double exposure_offset = profile.baseline_exposure_offset_ev.value_or(0.0);
    if (!std::isfinite(exposure_offset) || std::abs(exposure_offset) > 16.0) {
        fail(
            DcpColorDevelopmentErrorCode::invalid_input,
            "DCP BaselineExposureOffset is outside the supported finite range"
        );
    }
    camera_to_srgb = scale_matrix(camera_to_srgb, std::exp2(exposure_offset));
    const auto white_xy = xyz_to_xy(calibration.white_xyz);
    const detail::PreparedDcpRenderingStages rendering_stages =
        detail::prepare_dcp_rendering_stages(profile, calibration.calibration1_weight);

    DcpColorTransform result{
        .camera_to_linear_srgb_d65 = camera_to_srgb,
        .hue_sat_map = rendering_stages.hue_sat_map,
        .look_table = rendering_stages.look_table,
        .tone_curve = rendering_stages.tone_curve,
        .tone_curve_second_derivatives = rendering_stages.tone_curve_second_derivatives,
        .receipt = DcpColorDevelopmentReceipt{
            .schema_version = dcp_color_receipt_schema_version,
            .developer_version = dcp_color_developer_version,
            .profile_content_identity = definition.content_identity,
            .normalized_camera_model = definition.normalized_camera_model,
            .matrix_route = route,
            .calibration_illuminant1 = profile.calibration1.illuminant,
            .calibration_illuminant2 = profile.calibration2.has_value()
                                           ? profile.calibration2->illuminant
                                           : static_cast<std::uint16_t>(0U),
            .calibration1_weight = calibration.calibration1_weight,
            .estimated_white_x = white_xy[0],
            .estimated_white_y = white_xy[1],
            .estimated_correlated_color_temperature = calibration.temperature,
            .baseline_exposure_offset_ev = exposure_offset,
            .hue_sat_map_applied = rendering_stages.hue_sat_map.has_value(),
            .look_table_applied = rendering_stages.look_table.has_value(),
            .tone_curve_applied = !rendering_stages.tone_curve.empty(),
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
             << ";white-x=" << receipt.estimated_white_x << ";white-y=" << receipt.estimated_white_y
             << ";cct=" << receipt.estimated_correlated_color_temperature
             << ";baseline-ev=" << receipt.baseline_exposure_offset_ev
             << ";huesat=" << (receipt.hue_sat_map_applied ? "applied" : "none")
             << ";look=" << (receipt.look_table_applied ? "applied" : "none")
             << ";tone=" << (receipt.tone_curve_applied ? "applied" : "none");
    return identity.str();
}

} // namespace shadow::image
