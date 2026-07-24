#include <shadow/image/edit.hpp>

#include "adjustment_execution_internal.hpp"
#include "../concurrency/row_scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

static_assert(sizeof(float) == 4U);
static_assert(std::numeric_limits<float>::is_iec559);

constexpr std::size_t rgb_channels = 3U;
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double d65_x = 0.3127;
constexpr double d65_y = 0.3290;
constexpr double perceptual_low_chroma_ratio_epsilon = 1.0e-7;

// Public color-mixer order: red, orange, yellow, green, cyan, blue, purple,
// magenta. These are Oklch hue angles obtained by converting the indicated
// display colors to linear sRGB and then through the D65 Oklab transform used
// below. #ff8000 and #8000ff provide the two intermediate visual anchors.
// Keeping the anchors explicit prevents an HSV-style 45-degree wheel from
// silently being interpreted as Oklch.
constexpr std::array<double, perceptual_hue_band_count> perceptual_hue_anchors{
    29.23388536933038,  // #ff0000
    52.98468002449970,  // #ff8000
    109.76923279602303, // #ffff00
    142.49533925535556, // #00ff00
    194.76894786887132, // #00ffff
    264.05202307198110, // #0000ff
    293.93764240298924, // #8000ff
    328.36341829329797, // #ff00ff
};

// Photoshop Selective Color has six chromatic target families. Orange and
// purple intentionally blend between the adjacent primary target families,
// exactly as skin and twilight colors do in Photoshop's Red/Yellow and
// Blue/Magenta selections.
constexpr std::array<double, 6U> selective_color_hue_anchors{
    29.23388536933038,  // red
    109.76923279602303, // yellow
    142.49533925535556, // green
    194.76894786887132, // cyan
    264.05202307198110, // blue
    328.36341829329797, // magenta
};

using Vector3 = std::array<double, 3>;
using Matrix3 = std::array<Vector3, 3>;

struct WorkingSpaceTransform final {
    Matrix3 rgb_to_xyz{};
    Matrix3 xyz_to_rgb{};
};

struct PreparedSmoothToneCurve final {
    const ToneCurveSet* curve = nullptr;
    std::vector<double> knot_derivatives;
    bool identity = false;
};

using PreparedCurveAdjustment = std::variant<std::monostate, PreparedSmoothToneCurve>;

[[nodiscard]] std::string node_prefix(
    const std::size_t index,
    const AdjustmentNode& node
) {
    std::ostringstream message;
    message << "edit node " << index;
    if (!node.node_id.empty()) {
        message << " (" << node.node_id << ')';
    }
    message << ": ";
    return message.str();
}

[[noreturn]] void throw_node_error(
    const EditErrorCode code,
    const std::size_t index,
    const AdjustmentNode& node,
    const std::string_view detail
) {
    throw EditError(code, index, node_prefix(index, node) + std::string(detail));
}

[[nodiscard]] bool finite_chromaticity(const Chromaticity value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] Vector3 multiply(const Matrix3& matrix, const Vector3& vector) noexcept {
    return {
        matrix[0][0] * vector[0] + matrix[0][1] * vector[1]
            + matrix[0][2] * vector[2],
        matrix[1][0] * vector[0] + matrix[1][1] * vector[1]
            + matrix[1][2] * vector[2],
        matrix[2][0] * vector[0] + matrix[2][1] * vector[1]
            + matrix[2][2] * vector[2],
    };
}

[[nodiscard]] Matrix3 multiply(const Matrix3& left, const Matrix3& right) noexcept {
    Matrix3 result{};
    for (std::size_t row = 0U; row < rgb_channels; ++row) {
        for (std::size_t column = 0U; column < rgb_channels; ++column) {
            for (std::size_t inner = 0U; inner < rgb_channels; ++inner) {
                result[row][column] += left[row][inner] * right[inner][column];
            }
        }
    }
    return result;
}

[[nodiscard]] std::optional<Matrix3> inverse(const Matrix3& matrix) noexcept {
    const double determinant =
        matrix[0][0]
            * (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1])
        - matrix[0][1]
            * (matrix[1][0] * matrix[2][2] - matrix[1][2] * matrix[2][0])
        + matrix[0][2]
            * (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12) {
        return std::nullopt;
    }

    const double reciprocal = 1.0 / determinant;
    Matrix3 result{{
        {{
            (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) * reciprocal,
            (matrix[0][2] * matrix[2][1] - matrix[0][1] * matrix[2][2]) * reciprocal,
            (matrix[0][1] * matrix[1][2] - matrix[0][2] * matrix[1][1]) * reciprocal,
        }},
        {{
            (matrix[1][2] * matrix[2][0] - matrix[1][0] * matrix[2][2]) * reciprocal,
            (matrix[0][0] * matrix[2][2] - matrix[0][2] * matrix[2][0]) * reciprocal,
            (matrix[0][2] * matrix[1][0] - matrix[0][0] * matrix[1][2]) * reciprocal,
        }},
        {{
            (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]) * reciprocal,
            (matrix[0][1] * matrix[2][0] - matrix[0][0] * matrix[2][1]) * reciprocal,
            (matrix[0][0] * matrix[1][1] - matrix[0][1] * matrix[1][0]) * reciprocal,
        }},
    }};
    if (!std::ranges::all_of(result, [](const Vector3& row) {
            return std::ranges::all_of(row, [](const double value) {
                return std::isfinite(value);
            });
        })) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] WorkingSpaceTransform prepare_working_space_transform(
    const WorkingRgbSpace& space,
    const AdjustmentNode& node,
    const std::size_t index
) {
    constexpr double white_tolerance = 5.0e-4;
    if (
        std::abs(space.white_point.x - d65_x) > white_tolerance
        || std::abs(space.white_point.y - d65_y) > white_tolerance
    ) {
        throw_node_error(
            EditErrorCode::invalid_working_space,
            index,
            node,
            "D65 color adjustment requires a D65 RGB working space"
        );
    }

    Matrix3 primary_matrix{};
    for (std::size_t primary = 0U; primary < rgb_channels; ++primary) {
        const Chromaticity xy = space.primaries[primary];
        if (xy.x < 0.0 || xy.y <= 0.0 || xy.x + xy.y > 1.0 + 1.0e-9) {
            throw_node_error(
                EditErrorCode::invalid_working_space,
                index,
                node,
                "D65 color adjustment requires valid RGB primary chromaticities"
            );
        }
        primary_matrix[0][primary] = xy.x / xy.y;
        primary_matrix[1][primary] = 1.0;
        primary_matrix[2][primary] = (1.0 - xy.x - xy.y) / xy.y;
    }

    const auto primary_inverse = inverse(primary_matrix);
    if (!primary_inverse.has_value()) {
        throw_node_error(
            EditErrorCode::invalid_working_space,
            index,
            node,
            "D65 color adjustment requires independent RGB primaries"
        );
    }
    const Vector3 white_xyz{
        space.white_point.x / space.white_point.y,
        1.0,
        (1.0 - space.white_point.x - space.white_point.y) / space.white_point.y,
    };
    const Vector3 primary_scales = multiply(*primary_inverse, white_xyz);

    Matrix3 rgb_to_xyz{};
    for (std::size_t row = 0U; row < rgb_channels; ++row) {
        for (std::size_t column = 0U; column < rgb_channels; ++column) {
            rgb_to_xyz[row][column] = primary_matrix[row][column] * primary_scales[column];
        }
    }
    const auto xyz_to_rgb = inverse(rgb_to_xyz);
    if (!xyz_to_rgb.has_value()) {
        throw_node_error(
            EditErrorCode::invalid_working_space,
            index,
            node,
            "D65 color adjustment could not derive an invertible RGB matrix"
        );
    }

    // The declared coefficients are also used by selective tone and saturation. Reject a
    // contradictory space instead of silently using two different luminance definitions.
    for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
        if (std::abs(rgb_to_xyz[1][channel] - space.luminance_coefficients[channel]) > 5.0e-4) {
            throw_node_error(
                EditErrorCode::invalid_working_space,
                index,
                node,
                "working-space luminance coefficients disagree with its RGB primaries"
            );
        }
    }
    return WorkingSpaceTransform{.rgb_to_xyz = rgb_to_xyz, .xyz_to_rgb = *xyz_to_rgb};
}

[[nodiscard]] Chromaticity black_body_xy(const double kelvin) noexcept {
    const double temperature = std::clamp(kelvin, 1'667.0, 25'000.0);
    const double inverse = 1.0 / temperature;
    const double inverse2 = inverse * inverse;
    const double inverse3 = inverse2 * inverse;
    const double x = temperature <= 4'000.0
        ? -0.2661239e9 * inverse3 - 0.2343580e6 * inverse2
            + 0.8776956e3 * inverse + 0.179910
        : -3.0258469e9 * inverse3 + 2.1070379e6 * inverse2
            + 0.2226347e3 * inverse + 0.240390;
    double y = 0.0;
    if (temperature <= 2'222.0) {
        y = -1.1063814 * x * x * x - 1.34811020 * x * x
            + 2.18555832 * x - 0.20219683;
    } else if (temperature <= 4'000.0) {
        y = -0.9549476 * x * x * x - 1.37418593 * x * x
            + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x * x * x - 5.87338670 * x * x
            + 3.75112997 * x - 0.37001483;
    }
    return Chromaticity{.x = x, .y = y};
}

[[nodiscard]] Chromaticity tinted_white_xy(
    const double temperature,
    const double tint
) noexcept {
    constexpr double d65_kelvin = 6'504.0;
    constexpr double d65_mired = 1'000'000.0 / d65_kelvin;
    constexpr double maximum_mired_shift = 80.0;
    const double target_mired = d65_mired + maximum_mired_shift * temperature;
    const Chromaticity locus = black_body_xy(1'000'000.0 / target_mired);

    // CIE 1960 UCS is used only to express a bounded green↔magenta offset.
    // Positive UI tint means magenta, hence the negative v displacement.
    const double denominator = -2.0 * locus.x + 12.0 * locus.y + 3.0;
    const double u = 4.0 * locus.x / denominator;
    const double v = 6.0 * locus.y / denominator - 0.025 * tint;
    const double inverse_denominator = 6.0 * u - 24.0 * v + 12.0;
    return Chromaticity{
        .x = 9.0 * u / inverse_denominator,
        .y = 6.0 * v / inverse_denominator,
    };
}

[[nodiscard]] Matrix3 prepare_rgb_white_balance_matrix(
    const WorkingRgbSpace& space,
    const RgbWhiteBalanceAdjustment& parameters,
    const AdjustmentNode& node,
    const std::size_t index
) {
    const WorkingSpaceTransform working = prepare_working_space_transform(space, node, index);
    const Chromaticity target_xy = tinted_white_xy(parameters.temperature, parameters.tint);
    if (!finite_chromaticity(target_xy) || target_xy.x <= 0.0 || target_xy.y <= 0.0
        || target_xy.x + target_xy.y >= 1.0) {
        throw_node_error(
            EditErrorCode::invalid_parameter,
            index,
            node,
            "RGB white balance produced an invalid target white"
        );
    }

    constexpr Matrix3 cat16{{
        {{0.401288, 0.650173, -0.051461}},
        {{-0.250268, 1.204414, 0.045854}},
        {{-0.002079, 0.048952, 0.953127}},
    }};
    const auto cat16_inverse = inverse(cat16);
    if (!cat16_inverse.has_value()) {
        throw_node_error(
            EditErrorCode::numeric_overflow,
            index,
            node,
            "CAT16 matrix is not invertible"
        );
    }
    const Vector3 source_white_xyz{
        space.white_point.x / space.white_point.y,
        1.0,
        (1.0 - space.white_point.x - space.white_point.y) / space.white_point.y,
    };
    const Vector3 target_white_xyz{
        target_xy.x / target_xy.y,
        1.0,
        (1.0 - target_xy.x - target_xy.y) / target_xy.y,
    };
    const Vector3 source_response = multiply(cat16, source_white_xyz);
    const Vector3 target_response = multiply(cat16, target_white_xyz);
    Matrix3 response_scale{};
    for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
        if (!std::isfinite(source_response[channel])
            || std::abs(source_response[channel]) <= 1.0e-12
            || !std::isfinite(target_response[channel])) {
            throw_node_error(
                EditErrorCode::numeric_overflow,
                index,
                node,
                "CAT16 white response is not finite"
            );
        }
        response_scale[channel][channel] = target_response[channel]
            / source_response[channel];
    }
    const Matrix3 xyz_adaptation = multiply(
        *cat16_inverse,
        multiply(response_scale, cat16)
    );
    return multiply(
        working.xyz_to_rgb,
        multiply(xyz_adaptation, working.rgb_to_xyz)
    );
}

[[nodiscard]] Vector3 xyz_to_oklab(const Vector3& xyz) noexcept {
    const double l = std::cbrt(
        0.8190224379967030 * xyz[0] + 0.3619062600528904 * xyz[1]
        - 0.1288737815209879 * xyz[2]
    );
    const double m = std::cbrt(
        0.0329836539323885 * xyz[0] + 0.9292868615863434 * xyz[1]
        + 0.0361446663506424 * xyz[2]
    );
    const double s = std::cbrt(
        0.0481771893596242 * xyz[0] + 0.2642395317527308 * xyz[1]
        + 0.6335478284694309 * xyz[2]
    );
    return {
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

[[nodiscard]] Vector3 oklab_to_xyz(const Vector3& lab) noexcept {
    const double l_root = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
    const double m_root = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
    const double s_root = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
    const double l = l_root * l_root * l_root;
    const double m = m_root * m_root * m_root;
    const double s = s_root * s_root * s_root;
    return {
        1.2268798758459240 * l - 0.5578149944602170 * m + 0.2813910456659646 * s,
        -0.0405757452148009 * l + 1.1122868032803173 * m - 0.0717110580655164 * s,
        -0.0763729366746600 * l - 0.4214933324022431 * m + 1.5869240198367816 * s,
    };
}

#include "cpu_reference_tone.ipp"

#include "cpu_reference_color.ipp"

void validate_image(const FloatRgbImage& image) {
    if (image.dimensions.width == 0U || image.dimensions.height == 0U) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input dimensions must be non-zero"
        );
    }
    if (image.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input must use native interleaved RGB float32 samples"
        );
    }
    if (
        image.transfer_function != TransferFunction::linear
        || (image.reference != ImageReference::scene_referred
            && image.reference != ImageReference::display_referred)
    ) {
        throw EditError(
            EditErrorCode::incompatible_color_encoding,
            std::nullopt,
            "edit input must be standardized linear RGB"
        );
    }
    if (
        !std::isfinite(image.level_zero_to_raster_scale_x)
        || image.level_zero_to_raster_scale_x <= 0.0
        || !std::isfinite(image.level_zero_to_raster_scale_y)
        || image.level_zero_to_raster_scale_y <= 0.0
    ) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input level-0-to-raster scales must be finite and positive"
        );
    }

    const auto& space = image.working_space;
    if (
        space.id.empty() || !finite_chromaticity(space.white_point)
        || !std::ranges::all_of(space.primaries, finite_chromaticity)
        || !std::ranges::all_of(space.luminance_coefficients, [](const double value) {
               return std::isfinite(value);
           })
    ) {
        throw EditError(
            EditErrorCode::invalid_working_space,
            std::nullopt,
            "edit input requires a finite, explicitly identified RGB working space"
        );
    }
    const double luminance_sum =
        space.luminance_coefficients[0] + space.luminance_coefficients[1]
        + space.luminance_coefficients[2];
    if (std::abs(luminance_sum - 1.0) > 1.0e-6) {
        throw EditError(
            EditErrorCode::invalid_working_space,
            std::nullopt,
            "working-space luminance coefficients must sum to one"
        );
    }

    const std::uint64_t minimum_row_samples =
        static_cast<std::uint64_t>(image.dimensions.width) * rgb_channels;
    if (
        minimum_row_samples > std::numeric_limits<std::size_t>::max() / sizeof(float)
    ) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input row size overflows the address space"
        );
    }
    const std::size_t minimum_stride =
        static_cast<std::size_t>(minimum_row_samples) * sizeof(float);
    if (
        image.row_stride_bytes < minimum_stride
        || image.row_stride_bytes % sizeof(float) != 0U
    ) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input row stride is invalid"
        );
    }
    const std::size_t row_stride = image.row_stride_bytes / sizeof(float);
    const std::size_t height = static_cast<std::size_t>(image.dimensions.height);
    if (row_stride > std::numeric_limits<std::size_t>::max() / height) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input sample count overflows the address space"
        );
    }
    const std::size_t required_samples = row_stride * height;
    if (image.samples.size() != required_samples) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "edit input sample storage must exactly match dimensions and stride"
        );
    }
    if (!std::ranges::all_of(image.samples, [](const float value) {
            return std::isfinite(value);
        })) {
        throw EditError(
            EditErrorCode::non_finite_value,
            std::nullopt,
            "edit input contains NaN or infinity"
        );
    }
}

[[nodiscard]] AdjustmentExecutionContext validate_execution_context(
    const FloatRgbImage& input,
    AdjustmentExecutionContext context
) {
    if (context.full_dimensions.width == 0U || context.full_dimensions.height == 0U) {
        context.full_dimensions = input.dimensions;
    }
    if (context.origin_x > context.full_dimensions.width
        || context.origin_y > context.full_dimensions.height
        || input.dimensions.width > context.full_dimensions.width - context.origin_x
        || input.dimensions.height > context.full_dimensions.height - context.origin_y) {
        throw EditError(
            EditErrorCode::invalid_image_layout,
            std::nullopt,
            "adjustment execution context lies outside its full raster"
        );
    }
    return context;
}

#include "cpu_reference_curve.ipp"

[[nodiscard]] PreparedCurveAdjustment validate_node(
    const AdjustmentNode& node,
    const std::size_t index
) {
    const bool oklab_lightness_tone_curve = std::holds_alternative<OklabLightnessToneCurve>(
        node.parameters
    );
    const bool selective_tone = std::holds_alternative<SelectiveToneAdjustment>(
        node.parameters
    );
    const bool perceptual_color = std::holds_alternative<PerceptualColorAdjustment>(
        node.parameters
    );
    const bool detail_effects = std::holds_alternative<SharpenAdjustment>(node.parameters);
    const std::uint32_t expected_parameter_schema = oklab_lightness_tone_curve
        ? oklab_lightness_tone_curve_parameter_schema_version
        : selective_tone ? selective_tone_v3_parameter_schema_version
        : perceptual_color ? perceptual_color_v3_parameter_schema_version
        : detail_effects ? detail_effects_v3_parameter_schema_version
                         : adjustment_parameter_schema_version;
    const std::uint32_t expected_implementation = oklab_lightness_tone_curve
        ? oklab_lightness_tone_curve_implementation_version
        : selective_tone ? selective_tone_v3_implementation_version
        : perceptual_color ? perceptual_color_v3_implementation_version
                         : adjustment_implementation_version;
    const bool supported_detail_pass = detail_effects
        && ((std::get<SharpenAdjustment>(node.parameters).execution_pass
                == DetailEffectsExecutionPass::technical_detail
                && node.implementation_version == technical_detail_v3_implementation_version)
            || (std::get<SharpenAdjustment>(node.parameters).execution_pass
                    == DetailEffectsExecutionPass::color_grading
                    && node.implementation_version == color_grading_v3_implementation_version)
            || (std::get<SharpenAdjustment>(node.parameters).execution_pass
                    == DetailEffectsExecutionPass::finishing_effects
                    && node.implementation_version
                        == finishing_effects_v3_implementation_version));
    if (
        node.parameter_schema_version != expected_parameter_schema
        || (!detail_effects && node.implementation_version != expected_implementation)
        || (detail_effects && !supported_detail_pass)
    ) {
        throw_node_error(
            EditErrorCode::unsupported_version,
            index,
            node,
            oklab_lightness_tone_curve
                ? "Oklab lightness curve requires parameter schema 1 and implementation 1"
                : selective_tone
                    ? "selective tone requires the current guided-mask contract"
                : perceptual_color
                    ? "perceptual color requires the current complete contract"
                : detail_effects
                    ? "Detail & Effects requires the current split-pass contract"
                    : "only parameter schema 1 and implementation 1 are supported"
        );
    }

    PreparedCurveAdjustment prepared_curve;
    std::visit(
        [&node, index, &prepared_curve](const auto& parameters) {
            using Parameters = std::decay_t<decltype(parameters)>;
            if constexpr (std::is_same_v<Parameters, ExposureAdjustment>) {
                if (parameters.stops == 0.0) {
                    return;
                }
                const double gain = std::exp2(parameters.stops);
                if (!std::isfinite(parameters.stops) || !std::isfinite(gain) || gain <= 0.0) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "exposure stops must produce a finite, positive gain"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, ContrastAdjustment>) {
                if (
                    !std::isfinite(parameters.factor) || parameters.factor < 0.0
                    || !std::isfinite(parameters.pivot) || parameters.pivot < 0.0
                ) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "contrast factor and pivot must be finite and non-negative"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, OklabLightnessToneCurve>) {
                prepared_curve = prepare_oklab_lightness_tone_curve_node(parameters, node, index);
            } else if constexpr (std::is_same_v<Parameters, RgbWhiteBalanceAdjustment>) {
                if (!normalized_amount(parameters.temperature)
                    || !normalized_amount(parameters.tint)) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "RGB white balance temperature and tint must be within [-1, 1]"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, SaturationAdjustment>) {
                if (!std::isfinite(parameters.factor) || parameters.factor < 0.0) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "saturation factor must be finite and non-negative"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                if (
                    !normalized_amount(parameters.highlights)
                    || !normalized_amount(parameters.shadows)
                    || !normalized_amount(parameters.whites)
                    || !normalized_amount(parameters.blacks)
                ) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "selective tone amounts must be finite and within [-1, 1]"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, PerceptualColorAdjustment>) {
                const bool valid_bands =
                    std::ranges::all_of(parameters.hue, normalized_amount)
                    && std::ranges::all_of(parameters.saturation, normalized_amount)
                    && std::ranges::all_of(parameters.lightness, normalized_amount);
                const bool valid_selective_color = std::ranges::all_of(
                    parameters.selective_color_cmyk,
                    [](const auto& target) {
                        return std::ranges::all_of(target, normalized_amount);
                    }
                );
                const auto valid_range = [](const PerceptualColorRange& range) {
                    return
                    std::isfinite(range.center_degrees) && range.center_degrees >= 0.0
                    && range.center_degrees <= 360.0
                    && std::isfinite(range.width_degrees) && range.width_degrees >= 1.0
                    && range.width_degrees <= 180.0
                    && std::isfinite(range.softness) && range.softness >= 0.0
                    && range.softness <= 1.0
                    && std::isfinite(range.hue_shift_degrees)
                    && range.hue_shift_degrees >= -180.0
                    && range.hue_shift_degrees <= 180.0
                    && normalized_amount(range.saturation)
                    && normalized_amount(range.lightness);
                };
                const bool valid_ranges = valid_range(parameters.color_range)
                    && parameters.additional_color_ranges.size() + 1U
                        <= maximum_point_color_ranges
                    && std::ranges::all_of(parameters.additional_color_ranges, valid_range);
                if (!normalized_amount(parameters.vibrance) || !valid_bands || !valid_ranges
                    || !valid_selective_color
                    || !std::isfinite(parameters.selective_color_lightness_protection)
                    || parameters.selective_color_lightness_protection < 0.0
                    || parameters.selective_color_lightness_protection > 1.0) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "perceptual color parameters are outside their finite declared bounds"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, CubeLutAdjustment>) {
                const std::size_t size = parameters.lut.size;
                const bool empty_neutral = parameters.intensity == 0.0
                    && size == 0U && parameters.lut.entries.empty();
                const bool valid_shape = size >= 2U && size <= 65U
                    && parameters.lut.entries.size() == size * size * size;
                const bool valid_domain = std::ranges::all_of(
                    parameters.lut.domain_min,
                    [](const float value) { return std::isfinite(value); }
                ) && std::ranges::all_of(
                    parameters.lut.domain_max,
                    [](const float value) { return std::isfinite(value); }
                ) && parameters.lut.domain_min[0] < parameters.lut.domain_max[0]
                    && parameters.lut.domain_min[1] < parameters.lut.domain_max[1]
                    && parameters.lut.domain_min[2] < parameters.lut.domain_max[2];
                const bool finite_entries = std::ranges::all_of(
                    parameters.lut.entries,
                    [](const auto& entry) {
                        return std::ranges::all_of(
                            entry,
                            [](const float value) { return std::isfinite(value); }
                        );
                    }
                );
                if (!std::isfinite(parameters.intensity)
                    || parameters.intensity < 0.0 || parameters.intensity > 1.0
                    || (!empty_neutral && (!valid_shape || !valid_domain || !finite_entries))) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "3D LUT requires a valid cube and intensity within [0, 1]"
                    );
                }
            } else if constexpr (std::is_same_v<Parameters, SharpenAdjustment>) {
                const auto unit = [](const double value) {
                    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
                };
                const auto signed_unit = [](const double value) {
                    return std::isfinite(value) && value >= -1.0 && value <= 1.0;
                };
                if (!std::isfinite(parameters.amount) || parameters.amount < 0.0
                    || parameters.amount > 2.0
                    || !std::isfinite(parameters.radius) || parameters.radius < 0.1
                    || parameters.radius > 5.0
                    || !std::isfinite(parameters.threshold) || parameters.threshold < 0.0
                    || parameters.threshold > 1.0
                    || !std::isfinite(parameters.masking) || parameters.masking < 0.0
                    || parameters.masking > 1.0
                    || !signed_unit(parameters.clarity)
                    || !signed_unit(parameters.texture)
                    || !unit(parameters.denoise_luminance)
                    || !unit(parameters.denoise_detail)
                    || !unit(parameters.denoise_color)
                    || !signed_unit(parameters.dehaze)
                    || !unit(parameters.defringe_purple_amount)
                    || !unit(parameters.defringe_green_amount)
                    || !std::isfinite(parameters.defringe_purple_hue_low)
                    || parameters.defringe_purple_hue_low < 0.0
                    || parameters.defringe_purple_hue_low > 360.0
                    || !std::isfinite(parameters.defringe_purple_hue_high)
                    || parameters.defringe_purple_hue_high < 0.0
                    || parameters.defringe_purple_hue_high > 360.0
                    || parameters.defringe_purple_hue_low + 10.0
                        > parameters.defringe_purple_hue_high
                    || !std::isfinite(parameters.defringe_green_hue_low)
                    || parameters.defringe_green_hue_low < 0.0
                    || parameters.defringe_green_hue_low > 360.0
                    || !std::isfinite(parameters.defringe_green_hue_high)
                    || parameters.defringe_green_hue_high < 0.0
                    || parameters.defringe_green_hue_high > 360.0
                    || parameters.defringe_green_hue_low + 10.0
                        > parameters.defringe_green_hue_high
                    || !unit(parameters.shadows_saturation)
                    || !signed_unit(parameters.shadows_luminance)
                    || !unit(parameters.midtones_saturation)
                    || !signed_unit(parameters.midtones_luminance)
                    || !unit(parameters.highlights_saturation)
                    || !signed_unit(parameters.highlights_luminance)
                    || !unit(parameters.grading_blending)
                    || !signed_unit(parameters.grading_balance)
                    || !unit(parameters.grain_amount)
                    || !unit(parameters.grain_size)
                    || !unit(parameters.grain_roughness)
                    || !signed_unit(parameters.vignette_amount)
                    || !unit(parameters.vignette_midpoint)
                    || !signed_unit(parameters.vignette_roundness)
                    || !unit(parameters.vignette_feather)
                    || !unit(parameters.vignette_highlights)
                    || !std::isfinite(parameters.shadows_hue)
                    || parameters.shadows_hue < 0.0 || parameters.shadows_hue > 360.0
                    || !std::isfinite(parameters.midtones_hue)
                    || parameters.midtones_hue < 0.0 || parameters.midtones_hue > 360.0
                    || !std::isfinite(parameters.highlights_hue)
                    || parameters.highlights_hue < 0.0 || parameters.highlights_hue > 360.0
                ) {
                    throw_node_error(
                        EditErrorCode::invalid_parameter,
                        index,
                        node,
                        "Detail & Effects parameters are outside their declared bounds"
                    );
                }
            }
        },
        node.parameters
    );
    return prepared_curve;
}

[[nodiscard]] std::vector<PreparedCurveAdjustment> prepare_adjustment_nodes(
    const std::span<const AdjustmentNode> nodes
) {
    std::vector<PreparedCurveAdjustment> prepared_curves;
    prepared_curves.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        prepared_curves.push_back(validate_node(nodes[index], index));
    }
    return prepared_curves;
}

[[nodiscard]] bool adjustment_is_neutral(
    const AdjustmentParameters& parameters,
    const PreparedCurveAdjustment& prepared_curve
) {
    return std::visit(
        [&prepared_curve](const auto& value) {
            using Parameters = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Parameters, ExposureAdjustment>) {
                return value.stops == 0.0;
            } else if constexpr (std::is_same_v<Parameters, ContrastAdjustment>) {
                return value.factor == 1.0;
            } else if constexpr (std::is_same_v<Parameters, OklabLightnessToneCurve>) {
                return std::get<PreparedSmoothToneCurve>(prepared_curve).identity;
            } else if constexpr (std::is_same_v<Parameters, RgbWhiteBalanceAdjustment>) {
                return value.temperature == 0.0 && value.tint == 0.0;
            } else if constexpr (std::is_same_v<Parameters, SaturationAdjustment>) {
                return value.factor == 1.0;
            } else if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                return selective_tone_is_neutral(value);
            } else if constexpr (std::is_same_v<Parameters, PerceptualColorAdjustment>) {
                return perceptual_color_mapping_is_neutral(value)
                    && selective_color_is_neutral(value);
            } else if constexpr (std::is_same_v<Parameters, CubeLutAdjustment>) {
                return value.intensity == 0.0;
            } else {
                static_assert(std::is_same_v<Parameters, SharpenAdjustment>);
                switch (value.execution_pass) {
                case DetailEffectsExecutionPass::technical_detail:
                    return value.amount == 0.0
                        && value.denoise_luminance == 0.0
                        && value.denoise_color == 0.0
                        && value.dehaze == 0.0
                        && value.defringe_purple_amount == 0.0
                        && value.defringe_green_amount == 0.0;
                case DetailEffectsExecutionPass::color_grading:
                    return value.clarity == 0.0
                        && value.texture == 0.0
                        && value.shadows_saturation == 0.0
                        && value.shadows_luminance == 0.0
                        && value.midtones_saturation == 0.0
                        && value.midtones_luminance == 0.0
                        && value.highlights_saturation == 0.0
                        && value.highlights_luminance == 0.0;
                case DetailEffectsExecutionPass::finishing_effects:
                    return value.grain_amount == 0.0 && value.vignette_amount == 0.0;
                }
                return false;
            }
        },
        parameters
    );
}

#include "cpu_reference_detail.ipp"

void apply_node(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t index,
    const PreparedCurveAdjustment& prepared_curve,
    const AdjustmentExecutionContext& context
) {
    // The planner and executor intentionally share this exact classifier. Besides avoiding
    // redundant traversals in the CPU path, this prevents a GPU backend from eliding a node
    // that the reference executor would treat as observable.
    if (adjustment_is_neutral(node.parameters, prepared_curve)) {
        return;
    }
    std::visit(
        [&image, &node, index, &prepared_curve, &context](const auto& parameters) {
            using Parameters = std::decay_t<decltype(parameters)>;
            if constexpr (std::is_same_v<Parameters, ExposureAdjustment>) {
                const double gain = std::exp2(parameters.stops);
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [gain](const std::array<double, 3>& input) {
                        return std::array{
                            input[0] * gain,
                            input[1] * gain,
                            input[2] * gain,
                        };
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, ContrastAdjustment>) {
                // Avoid a complete image traversal merely to call the
                // identity branch of apply_perceptual_contrast for every
                // pixel. This is particularly important for the default node
                // stack, where contrast is always structurally present.
                if (parameters.factor == 1.0) {
                    return;
                }
                const WorkingSpaceTransform color_transform =
                    prepare_working_space_transform(image.working_space, node, index);
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&parameters, &color_transform](const Vector3& input) {
                        return apply_perceptual_contrast(input, color_transform, parameters);
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, OklabLightnessToneCurve>) {
                const WorkingSpaceTransform color_transform =
                    prepare_working_space_transform(image.working_space, node, index);
                apply_prepared_oklab_lightness_tone_curve(
                    image,
                    std::get<PreparedSmoothToneCurve>(prepared_curve),
                    color_transform,
                    [&node, index](const double value) {
                        return checked_float(value, index, node);
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, RgbWhiteBalanceAdjustment>) {
                if (parameters.temperature == 0.0 && parameters.tint == 0.0) {
                    return;
                }
                const Matrix3 adaptation = prepare_rgb_white_balance_matrix(
                    image.working_space,
                    parameters,
                    node,
                    index
                );
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&adaptation](const std::array<double, 3>& input) {
                        return multiply(adaptation, input);
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, SaturationAdjustment>) {
                // Saturation is a chroma operation, not a linear-RGB interpolation around a
                // working-space luma value.  The latter can make equal numeric changes look
                // very different across hues and gives a poor neutral axis for wide-gamut
                // working spaces.  Oklab lets this control scale perceptual chroma while
                // retaining lightness, and the working-space transforms keep the public node
                // independent of the particular D65 RGB primaries selected for the recipe.
                if (parameters.factor == 1.0) {
                    return;
                }
                const WorkingSpaceTransform color_transform =
                    prepare_working_space_transform(image.working_space, node, index);
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&parameters, &color_transform](const Vector3& input) {
                        // Preserve the D65 neutral axis exactly.  Besides avoiding a needless
                        // matrix round trip, this makes neutral grays invariant for every
                        // saturation value, including negative scene-linear values.
                        if (input[0] == input[1] && input[1] == input[2]) {
                            return input;
                        }
                        Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
                        lab[1] *= parameters.factor;
                        lab[2] *= parameters.factor;
                        // Do not clamp here. Scene-linear RGB can legitimately carry negative
                        // and super-white values, and gamut mapping belongs to the output
                        // transform. checked_float() below still fails closed on non-finite or
                        // unrepresentable results.
                        return multiply(color_transform.xyz_to_rgb, oklab_to_xyz(lab));
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                if (selective_tone_is_neutral(parameters)) {
                    return;
                }
                apply_guided_selective_tone(image, node, index, parameters);
            } else if constexpr (std::is_same_v<Parameters, PerceptualColorAdjustment>) {
                // Classify the two independent color stages once per node. In particular, do
                // not scan the Selective Color parameter grid or perform a second complete
                // RGB -> Oklab conversion for every pixel when its CMYK adjustments are neutral.
                const bool mapping_is_neutral = perceptual_color_mapping_is_neutral(parameters);
                const bool selective_is_neutral = selective_color_is_neutral(parameters);
                if (mapping_is_neutral && selective_is_neutral) {
                    return;
                }
                const WorkingSpaceTransform color_transform =
                    prepare_working_space_transform(image.working_space, node, index);
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&parameters, &color_transform, mapping_is_neutral, selective_is_neutral](
                        const Vector3& input
                    ) {
                        if (mapping_is_neutral) {
                            return apply_selective_color(input, parameters, color_transform);
                        }
                        Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
                        const double chroma = std::hypot(lab[1], lab[2]);
                        const double relative_chroma = chroma
                            / std::max(1.0e-6, std::abs(lab[0]));
                        Vector3 adjusted = input;
                        if (relative_chroma > perceptual_low_chroma_ratio_epsilon) {
                            const double source_hue = wrap_degrees(
                                std::atan2(lab[2], lab[1]) * 180.0 / pi
                            );
                            const auto band_weights = hue_band_weights(source_hue);
                            // Hue is numerically unstable near the neutral axis. Fade all
                            // hue-keyed controls there while leaving vibrance free to increase a
                            // real, muted chroma. The ratio keeps this behavior invariant under
                            // scene exposure.
                            const double hue_confidence = smoothstep(
                                0.002,
                                0.02,
                                relative_chroma
                            );
                            const double band_hue = hue_confidence
                                * weighted_sum(parameters.hue, band_weights);
                            const double band_saturation = weighted_sum(
                                parameters.saturation,
                                band_weights
                            ) * hue_confidence;
                            const double band_lightness = hue_confidence * weighted_sum(
                                parameters.lightness,
                                band_weights
                            );
                            const double range_weight = hue_confidence
                                * color_range_weight(parameters.color_range, source_hue);

                            const double vibrance_weight = 1.0
                                - smoothstep(0.05, 0.35, relative_chroma);
                            const double chroma_factor =
                                (1.0 + parameters.vibrance * vibrance_weight)
                                * (1.0 + band_saturation)
                                * (1.0 + range_weight * parameters.color_range.saturation);
                            const double hue_delta = 30.0 * band_hue
                                + range_weight * parameters.color_range.hue_shift_degrees;
                            const double lightness_delta = 0.15
                                * (band_lightness
                                   + range_weight * parameters.color_range.lightness);
                            bool changed = chroma_factor != 1.0 || hue_delta != 0.0
                                || lightness_delta != 0.0;
                            if (changed) {
                                const double adjusted_hue =
                                    (source_hue + hue_delta) * pi / 180.0;
                                const double adjusted_chroma = chroma * chroma_factor;
                                lab[0] += lightness_delta;
                                lab[1] = adjusted_chroma * std::cos(adjusted_hue);
                                lab[2] = adjusted_chroma * std::sin(adjusted_hue);
                            }
                            for (const auto& range : parameters.additional_color_ranges) {
                                changed = apply_ordered_color_range(lab, range) || changed;
                            }
                            if (changed) {
                                adjusted = multiply(
                                    color_transform.xyz_to_rgb,
                                    oklab_to_xyz(lab)
                                );
                            }
                        }
                        return selective_is_neutral
                            ? adjusted
                            : apply_selective_color(adjusted, parameters, color_transform);
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, CubeLutAdjustment>) {
                if (parameters.intensity == 0.0) {
                    return;
                }
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&parameters](const Vector3& input) {
                        const auto sampled = sample_cube_lut(parameters.lut, {
                            static_cast<float>(input[0]),
                            static_cast<float>(input[1]),
                            static_cast<float>(input[2]),
                        });
                        const double mix = parameters.intensity;
                        return Vector3{
                            input[0] + (static_cast<double>(sampled[0]) - input[0]) * mix,
                            input[1] + (static_cast<double>(sampled[1]) - input[1]) * mix,
                            input[2] + (static_cast<double>(sampled[2]) - input[2]) * mix,
                        };
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, SharpenAdjustment>) {
                switch (parameters.execution_pass) {
                case DetailEffectsExecutionPass::technical_detail:
                    // Technical recovery is deliberately scene-linear and
                    // pre-creative: it must not denoise or sharpen a LUT.
                    apply_edge_aware_denoise(image, node, index, parameters);
                    apply_dehaze_and_defringe(image, node, index, parameters);
                    apply_sharpen(image, node, index, parameters);
                    break;
                case DetailEffectsExecutionPass::color_grading:
                    // Perceptual clarity/texture and color wheels are creative
                    // transforms. The former only touches Oklab L; the latter
                    // works in hue/chroma, so they cooperate without an RGB
                    // channel-order dependency and both remain before a LUT.
                    apply_perceptual_detail(image, node, index, parameters);
                    apply_color_grading(image, node, index, parameters);
                    break;
                case DetailEffectsExecutionPass::finishing_effects:
                    // Grain and vignette are intentionally the last internal
                    // pass so LUT/grading do not alter their look.
                    apply_grain_and_vignette(image, node, index, parameters, context);
                    break;
                }
            }
        },
        node.parameters
    );
}

} // namespace

EditError::EditError(
    const EditErrorCode code,
    const std::optional<std::size_t> node_index,
    std::string message
)
    : std::runtime_error(std::move(message)), code_(code), node_index_(node_index) {}

EditErrorCode EditError::code() const noexcept {
    return code_;
}

std::optional<std::size_t> EditError::node_index() const noexcept {
    return node_index_;
}

AdjustmentOperation operation(const AdjustmentParameters& parameters) noexcept {
    return std::visit(
        [](const auto& value) {
            using Parameters = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Parameters, ExposureAdjustment>) {
                return AdjustmentOperation::exposure;
            } else if constexpr (std::is_same_v<Parameters, ContrastAdjustment>) {
                return AdjustmentOperation::contrast;
            } else if constexpr (std::is_same_v<Parameters, OklabLightnessToneCurve>) {
                return AdjustmentOperation::oklab_lightness_tone_curve;
            } else if constexpr (std::is_same_v<Parameters, RgbWhiteBalanceAdjustment>) {
                return AdjustmentOperation::rgb_white_balance;
            } else if constexpr (std::is_same_v<Parameters, SaturationAdjustment>) {
                return AdjustmentOperation::saturation;
            } else if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                return AdjustmentOperation::selective_tone;
            } else if constexpr (std::is_same_v<Parameters, PerceptualColorAdjustment>) {
                return AdjustmentOperation::perceptual_color;
            } else if constexpr (std::is_same_v<Parameters, CubeLutAdjustment>) {
                return AdjustmentOperation::lut_3d;
            } else {
                static_assert(std::is_same_v<Parameters, SharpenAdjustment>);
                return AdjustmentOperation::sharpen;
            }
        },
        parameters
    );
}

std::string_view operation_id(const AdjustmentOperation operation) noexcept {
    switch (operation) {
    case AdjustmentOperation::exposure:
        return "shadow.exposure";
    case AdjustmentOperation::contrast:
        return "shadow.contrast";
    case AdjustmentOperation::oklab_lightness_tone_curve:
        return "shadow.oklab_lightness_tone_curve";
    case AdjustmentOperation::rgb_white_balance:
        return "shadow.rgb_white_balance";
    case AdjustmentOperation::saturation:
        return "shadow.saturation";
    case AdjustmentOperation::selective_tone:
        return "shadow.selective_tone";
    case AdjustmentOperation::perceptual_color:
        return "shadow.perceptual_color";
    case AdjustmentOperation::lut_3d:
        return "shadow.lut_3d";
    case AdjustmentOperation::sharpen:
        return "shadow.sharpen";
    }
    return "shadow.unknown";
}

AdjustmentLocality locality(const AdjustmentOperation operation) noexcept {
    switch (operation) {
    case AdjustmentOperation::exposure:
    case AdjustmentOperation::contrast:
    case AdjustmentOperation::oklab_lightness_tone_curve:
    case AdjustmentOperation::rgb_white_balance:
    case AdjustmentOperation::saturation:
    case AdjustmentOperation::perceptual_color:
    case AdjustmentOperation::lut_3d:
        return AdjustmentLocality::pixel_local;
    case AdjustmentOperation::selective_tone:
    case AdjustmentOperation::sharpen:
        return AdjustmentLocality::neighborhood;
    }
    return AdjustmentLocality::pixel_local;
}

AdjustmentLocality locality(const AdjustmentParameters& parameters) noexcept {
    return std::visit(
        [](const auto& value) {
            using Parameters = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                return AdjustmentLocality::neighborhood;
            } else if constexpr (std::is_same_v<Parameters, SharpenAdjustment>) {
                return value.execution_pass == DetailEffectsExecutionPass::technical_detail
                        || (value.execution_pass == DetailEffectsExecutionPass::color_grading
                            && (value.clarity != 0.0 || value.texture != 0.0))
                    ? AdjustmentLocality::neighborhood
                    : AdjustmentLocality::pixel_local;
            } else {
                return AdjustmentLocality::pixel_local;
            }
        },
        parameters
    );
}

AdjustmentFootprint footprint(
    const AdjustmentParameters& parameters,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y
) {
    if (
        !std::isfinite(level_zero_to_raster_scale_x)
        || level_zero_to_raster_scale_x <= 0.0
        || !std::isfinite(level_zero_to_raster_scale_y)
        || level_zero_to_raster_scale_y <= 0.0
    ) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "adjustment footprint scales must be finite and positive"
        );
    }
    return std::visit(
        [level_zero_to_raster_scale_x, level_zero_to_raster_scale_y](const auto& value) {
            using Parameters = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Parameters, SelectiveToneAdjustment>) {
                if (!normalized_amount(value.highlights)
                    || !normalized_amount(value.shadows)
                    || !normalized_amount(value.whites)
                    || !normalized_amount(value.blacks)) {
                    throw EditError(
                        EditErrorCode::invalid_parameter,
                        std::nullopt,
                        "cannot calculate a footprint for malformed selective tone parameters"
                    );
                }
                if (selective_tone_is_neutral(value)) {
                    return AdjustmentFootprint{};
                }
                return AdjustmentFootprint{
                    .horizontal_radius = selective_tone_guided_filter_support_radius(
                        level_zero_to_raster_scale_x
                    ),
                    .vertical_radius = selective_tone_guided_filter_support_radius(
                        level_zero_to_raster_scale_y
                    ),
                };
            } else if constexpr (std::is_same_v<Parameters, SharpenAdjustment>) {
                if (value.execution_pass != DetailEffectsExecutionPass::technical_detail) {
                    if (value.execution_pass == DetailEffectsExecutionPass::color_grading) {
                        if (!normalized_amount(value.clarity)
                            || !normalized_amount(value.texture)) {
                            throw EditError(
                                EditErrorCode::invalid_parameter,
                                std::nullopt,
                                "cannot calculate a footprint for malformed perceptual detail parameters"
                            );
                        }
                        if (value.clarity == 0.0 && value.texture == 0.0) {
                            return AdjustmentFootprint{};
                        }
                        constexpr double texture_support_level_zero = 3.0 * 1.4;
                        constexpr double clarity_support_level_zero = 3.0 * 12.0;
                        const double support_level_zero = value.clarity == 0.0
                            ? texture_support_level_zero
                            : std::max(
                                clarity_support_level_zero,
                                value.texture == 0.0 ? 0.0 : texture_support_level_zero
                            );
                        const double horizontal = std::ceil(
                            support_level_zero * level_zero_to_raster_scale_x
                        );
                        const double vertical = std::ceil(
                            support_level_zero * level_zero_to_raster_scale_y
                        );
                        return AdjustmentFootprint{
                            .horizontal_radius = static_cast<std::uint32_t>(horizontal),
                            .vertical_radius = static_cast<std::uint32_t>(vertical),
                        };
                    }
                    if (value.execution_pass == DetailEffectsExecutionPass::finishing_effects) {
                        return AdjustmentFootprint{};
                    }
                    throw EditError(
                        EditErrorCode::invalid_parameter,
                        std::nullopt,
                        "cannot calculate a footprint for an unknown Detail & Effects pass"
                    );
                }
                if (
                    !std::isfinite(value.amount) || value.amount < 0.0 || value.amount > 2.0
                    || !std::isfinite(value.radius) || value.radius < 0.1
                    || value.radius > 5.0
                    || !std::isfinite(value.threshold) || value.threshold < 0.0
                    || value.threshold > 1.0
                    || !std::isfinite(value.masking) || value.masking < 0.0
                    || value.masking > 1.0
                ) {
                    throw EditError(
                        EditErrorCode::invalid_parameter,
                        std::nullopt,
                        "cannot calculate a footprint for malformed sharpen parameters"
                    );
                }
                const bool denoise_active = value.denoise_luminance > 0.0
                    || value.denoise_color > 0.0;
                if (value.amount == 0.0 && !denoise_active) {
                    return AdjustmentFootprint{};
                }
                const double sharpen_horizontal = value.amount == 0.0 ? 0.0 : std::ceil(
                    3.0 * value.radius * level_zero_to_raster_scale_x
                );
                const double sharpen_vertical = value.amount == 0.0 ? 0.0 : std::ceil(
                    3.0 * value.radius * level_zero_to_raster_scale_y
                );
                const double denoise_horizontal = denoise_active ? std::max(
                    1.0,
                    std::ceil(2.0 * level_zero_to_raster_scale_x)
                ) : 0.0;
                const double denoise_vertical = denoise_active ? std::max(
                    1.0,
                    std::ceil(2.0 * level_zero_to_raster_scale_y)
                ) : 0.0;
                const double horizontal = sharpen_horizontal + denoise_horizontal;
                const double vertical = sharpen_vertical + denoise_vertical;
                if (
                    horizontal > std::numeric_limits<std::uint32_t>::max()
                    || vertical > std::numeric_limits<std::uint32_t>::max()
                ) {
                    throw EditError(
                        EditErrorCode::numeric_overflow,
                        std::nullopt,
                        "adjustment footprint exceeds the supported integer range"
                    );
                }
                return AdjustmentFootprint{
                    .horizontal_radius = static_cast<std::uint32_t>(horizontal),
                    .vertical_radius = static_cast<std::uint32_t>(vertical),
                };
            } else {
                return AdjustmentFootprint{};
            }
        },
        parameters
    );
}

void validate_adjustment_nodes(const std::span<const AdjustmentNode> nodes) {
    static_cast<void>(prepare_adjustment_nodes(nodes));
}

EditExecutionPlan compile_edit_execution_plan(
    const std::span<const AdjustmentNode> nodes,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y
) {
    // Validation is deliberately complete and precedes enabled/neutral filtering or any
    // scheduling-specific checks.
    const auto prepared_curves = prepare_adjustment_nodes(nodes);
    // Validate scale arguments even when the recipe is empty or all nodes are omitted.
    static_cast<void>(footprint(
        AdjustmentParameters{ExposureAdjustment{}},
        level_zero_to_raster_scale_x,
        level_zero_to_raster_scale_y
    ));

    EditExecutionPlan plan{
        .source_node_count = nodes.size(),
    };
    const auto add_footprint = [&nodes](
        AdjustmentFootprint& destination,
        const AdjustmentFootprint addition,
        const std::size_t node_index
    ) {
        if (addition.horizontal_radius
                > std::numeric_limits<std::uint32_t>::max()
                    - destination.horizontal_radius
            || addition.vertical_radius
                > std::numeric_limits<std::uint32_t>::max()
                    - destination.vertical_radius) {
            throw_node_error(
                EditErrorCode::numeric_overflow,
                node_index,
                nodes[node_index],
                "compiled adjustment footprint exceeds the supported integer range"
            );
        }
        destination.horizontal_radius += addition.horizontal_radius;
        destination.vertical_radius += addition.vertical_radius;
    };

    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const AdjustmentNode& node = nodes[index];
        if (!node.enabled || adjustment_is_neutral(node.parameters, prepared_curves[index])) {
            continue;
        }

        const AdjustmentLocality node_locality = locality(node.parameters);
        if (plan.segments.empty() || plan.segments.back().locality != node_locality) {
            plan.segments.push_back(EditExecutionSegment{
                .locality = node_locality,
                .first_node_index = index,
                .past_last_node_index = index + 1U,
            });
        }
        EditExecutionSegment& segment = plan.segments.back();
        segment.past_last_node_index = index + 1U;
        segment.steps.push_back(EditExecutionStep{
            .node_index = index,
            .operation = operation(node.parameters),
        });

        AdjustmentFootprint node_footprint;
        try {
            node_footprint = footprint(
                node.parameters,
                level_zero_to_raster_scale_x,
                level_zero_to_raster_scale_y
            );
        } catch (const EditError& error) {
            throw_node_error(error.code(), index, node, error.what());
        }
        add_footprint(segment.cumulative_footprint, node_footprint, index);
        add_footprint(plan.cumulative_footprint, node_footprint, index);
    }
    return plan;
}

namespace detail {

MetalAdjustmentPreparationV1 prepare_metal_adjustment_v1(
    const FloatRgbImage& input,
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const AdjustmentExecutionContext context
) {
    validate_image(input);
    static_cast<void>(validate_execution_context(input, context));
    if (plan.source_node_count != nodes.size()) {
        return MetalAdjustmentPreparationV1{
            .program = std::nullopt,
            .diagnostic = "Metal adjustment plan no longer matches its source nodes",
        };
    }

    std::size_t step_count = 0U;
    for (const auto& segment : plan.segments) {
        if (segment.locality != AdjustmentLocality::pixel_local) {
            return MetalAdjustmentPreparationV1{
                .program = std::nullopt,
                .diagnostic = "Metal adjustment v1 cannot prepare a neighborhood segment",
            };
        }
        if (segment.steps.size() > std::numeric_limits<std::size_t>::max() - step_count) {
            return MetalAdjustmentPreparationV1{
                .program = std::nullopt,
                .diagnostic = "Metal adjustment operation count overflowed",
            };
        }
        step_count += segment.steps.size();
    }
    if (step_count == 0U || step_count > std::numeric_limits<std::uint32_t>::max()) {
        return MetalAdjustmentPreparationV1{
            .program = std::nullopt,
            .diagnostic = step_count == 0U
                ? "Metal adjustment v1 received no executable operations"
                : "Metal adjustment operation count exceeds its uint32 ABI",
        };
    }
    if (input.dimensions.width > std::numeric_limits<std::uint32_t>::max() / 3U) {
        return MetalAdjustmentPreparationV1{
            .program = std::nullopt,
            .diagnostic = "Metal adjustment row width exceeds its uint32 ABI",
        };
    }

    PreparedMetalAdjustmentV1 prepared;
    prepared.invocation.width = input.dimensions.width;
    prepared.invocation.height = input.dimensions.height;
    prepared.invocation.input_row_floats = input.dimensions.width * 3U;
    prepared.invocation.output_row_floats = input.dimensions.width * 3U;
    prepared.invocation.step_count = static_cast<std::uint32_t>(step_count);
    prepared.operations.reserve(step_count);

    const auto checked_parameter_float = [](const double value) -> std::optional<float> {
        if (!std::isfinite(value)
            || value > static_cast<double>(std::numeric_limits<float>::max())
            || value < -static_cast<double>(std::numeric_limits<float>::max())) {
            return std::nullopt;
        }
        const float converted = static_cast<float>(value);
        return std::isfinite(converted) ? std::optional<float>{converted} : std::nullopt;
    };
    const auto fill_matrix_rows = [&checked_parameter_float](
        const Matrix3& matrix,
        std::array<float, 4U>& row_0,
        std::array<float, 4U>& row_1,
        std::array<float, 4U>& row_2
    ) {
        std::array<std::array<float, 4U>*, 3U> rows{&row_0, &row_1, &row_2};
        for (std::size_t row = 0U; row < 3U; ++row) {
            for (std::size_t column = 0U; column < 3U; ++column) {
                const auto converted = checked_parameter_float(matrix[row][column]);
                if (!converted.has_value()) {
                    return false;
                }
                (*rows[row])[column] = *converted;
            }
        }
        return true;
    };

    std::optional<WorkingSpaceTransform> working_transform;
    for (const auto& segment : plan.segments) {
        for (const auto& step : segment.steps) {
            if (step.node_index >= nodes.size()
                || step.node_index > std::numeric_limits<std::uint32_t>::max()) {
                return MetalAdjustmentPreparationV1{
                    .program = std::nullopt,
                    .diagnostic =
                        "Metal adjustment source-node index exceeds its uint32 ABI",
                };
            }
            const AdjustmentNode& node = nodes[step.node_index];
            if (operation(node.parameters) != step.operation) {
                return MetalAdjustmentPreparationV1{
                    .program = std::nullopt,
                    .diagnostic =
                        "Metal adjustment plan operation no longer matches its source node",
                };
            }

            MetalAdjustmentOpV1 operation_record{
                .source_node_index = static_cast<std::uint32_t>(step.node_index),
            };
            switch (step.operation) {
            case AdjustmentOperation::rgb_white_balance: {
                operation_record.opcode = static_cast<std::uint32_t>(
                    MetalAdjustmentOpcodeV1::rgb_white_balance
                );
                const auto& parameters =
                    std::get<RgbWhiteBalanceAdjustment>(node.parameters);
                const Matrix3 adaptation = prepare_rgb_white_balance_matrix(
                    input.working_space,
                    parameters,
                    node,
                    step.node_index
                );
                if (!fill_matrix_rows(
                        adaptation,
                        operation_record.parameter_0,
                        operation_record.parameter_1,
                        operation_record.parameter_2
                    )) {
                    return MetalAdjustmentPreparationV1{
                        .program = std::nullopt,
                        .diagnostic =
                            "Metal white-balance matrix exceeds finite fp32 range",
                    };
                }
                break;
            }
            case AdjustmentOperation::exposure: {
                operation_record.opcode = static_cast<std::uint32_t>(
                    MetalAdjustmentOpcodeV1::exposure
                );
                const auto& parameters = std::get<ExposureAdjustment>(node.parameters);
                const auto gain = checked_parameter_float(std::exp2(parameters.stops));
                if (!gain.has_value() || *gain <= 0.0F) {
                    return MetalAdjustmentPreparationV1{
                        .program = std::nullopt,
                        .diagnostic = "Metal exposure gain exceeds finite fp32 range",
                    };
                }
                operation_record.parameter_0[0] = *gain;
                break;
            }
            case AdjustmentOperation::contrast: {
                operation_record.opcode = static_cast<std::uint32_t>(
                    MetalAdjustmentOpcodeV1::contrast
                );
                const auto& parameters = std::get<ContrastAdjustment>(node.parameters);
                const double pivot = std::cbrt(std::max(parameters.pivot, 1.0e-9));
                const double amount = parameters.factor == 0.0
                    ? 0.0
                    : std::clamp(std::log2(parameters.factor) * 0.20, -0.45, 0.45);
                const auto pivot_float = checked_parameter_float(pivot);
                const auto amount_float = checked_parameter_float(amount);
                if (!pivot_float.has_value() || !amount_float.has_value()) {
                    return MetalAdjustmentPreparationV1{
                        .program = std::nullopt,
                        .diagnostic = "Metal contrast parameters exceed finite fp32 range",
                    };
                }
                operation_record.parameter_0 = {
                    *pivot_float,
                    *amount_float,
                    parameters.factor == 0.0 ? 1.0F : 0.0F,
                    0.0F,
                };
                if (!working_transform.has_value()) {
                    working_transform = prepare_working_space_transform(
                        input.working_space,
                        node,
                        step.node_index
                    );
                }
                break;
            }
            case AdjustmentOperation::saturation: {
                operation_record.opcode = static_cast<std::uint32_t>(
                    MetalAdjustmentOpcodeV1::saturation
                );
                const auto& parameters = std::get<SaturationAdjustment>(node.parameters);
                const auto factor = checked_parameter_float(parameters.factor);
                if (!factor.has_value()) {
                    return MetalAdjustmentPreparationV1{
                        .program = std::nullopt,
                        .diagnostic = "Metal saturation factor exceeds finite fp32 range",
                    };
                }
                operation_record.parameter_0[0] = *factor;
                if (!working_transform.has_value()) {
                    working_transform = prepare_working_space_transform(
                        input.working_space,
                        node,
                        step.node_index
                    );
                }
                break;
            }
            case AdjustmentOperation::oklab_lightness_tone_curve:
            case AdjustmentOperation::selective_tone:
            case AdjustmentOperation::perceptual_color:
            case AdjustmentOperation::lut_3d:
            case AdjustmentOperation::sharpen:
                return MetalAdjustmentPreparationV1{
                    .program = std::nullopt,
                    .diagnostic = "Metal adjustment v1 received an unsupported operation",
                };
            }
            prepared.operations.push_back(operation_record);
        }
    }

    if (working_transform.has_value()
        && (!fill_matrix_rows(
                working_transform->rgb_to_xyz,
                prepared.invocation.rgb_to_xyz_row_0,
                prepared.invocation.rgb_to_xyz_row_1,
                prepared.invocation.rgb_to_xyz_row_2
            )
            || !fill_matrix_rows(
                working_transform->xyz_to_rgb,
                prepared.invocation.xyz_to_rgb_row_0,
                prepared.invocation.xyz_to_rgb_row_1,
                prepared.invocation.xyz_to_rgb_row_2
            ))) {
        return MetalAdjustmentPreparationV1{
            .program = std::nullopt,
            .diagnostic = "Metal working-space transform exceeds finite fp32 range",
        };
    }
    return MetalAdjustmentPreparationV1{
        .program = std::move(prepared),
        .diagnostic = {},
    };
}

} // namespace detail

FloatRgbImage execute_adjustment_nodes(
    const FloatRgbImage& input,
    const std::span<const AdjustmentNode> nodes,
    AdjustmentExecutionContext context
) {
    validate_image(input);
    const auto prepared_curves = prepare_adjustment_nodes(nodes);
    context = validate_execution_context(input, context);
    FloatRgbImage output = input;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        if (nodes[index].enabled) {
            apply_node(output, nodes[index], index, prepared_curves[index], context);
        }
    }
    return output;
}

FloatRgbImage apply_oklab_lightness_tone_curve(
    const FloatRgbImage& input,
    const OklabLightnessToneCurve& curve
) {
    validate_image(input);
    const PreparedSmoothToneCurve prepared = prepare_oklab_lightness_tone_curve(curve);
    if (prepared.identity) {
        return input;
    }

    // The standalone API deliberately uses a standard working-space transform,
    // matching the default FloatRgbImage contract used by the existing tone
    // curve helpers. The full graph path selects the image's configured working
    // space immediately before applying the same evaluator.
    const WorkingSpaceTransform color_transform = prepare_working_space_transform(
        input.working_space,
        AdjustmentNode{},
        0U
    );
    FloatRgbImage output = input;
    apply_prepared_oklab_lightness_tone_curve(
        output,
        prepared,
        color_transform,
        checked_tone_curve_float
    );
    return output;
}

std::vector<ToneCurvePoint> sample_smooth_tone_curve(
    const ToneCurveSet& curve,
    const std::size_t sample_count
) {
    if (sample_count < 2U || sample_count > maximum_tone_curve_preview_samples) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "smooth tone curve preview sample count must be between 2 and 4097"
        );
    }
    const PreparedSmoothToneCurve prepared = prepare_smooth_tone_curve(curve);
    std::vector<ToneCurvePoint> samples;
    samples.reserve(sample_count);
    const double denominator = static_cast<double>(sample_count - 1U);
    for (std::size_t index = 0U; index < sample_count; ++index) {
        const double x = static_cast<double>(index) / denominator;
        const double y = evaluate_smooth_tone_curve(prepared, x);
        if (!std::isfinite(y)) {
            throw EditError(
                EditErrorCode::numeric_overflow,
                std::nullopt,
                "smooth tone curve preview exceeded finite double range"
            );
        }
        samples.push_back({.x = x, .y = y});
    }
    return samples;
}

} // namespace shadow::image
