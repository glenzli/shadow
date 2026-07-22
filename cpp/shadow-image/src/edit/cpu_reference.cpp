#include <shadow/image/edit.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <sstream>
#include <type_traits>
#include <utility>

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

using Vector3 = std::array<double, 3>;
using Matrix3 = std::array<Vector3, 3>;

struct WorkingSpaceTransform final {
    Matrix3 rgb_to_xyz{};
    Matrix3 xyz_to_rgb{};
};

struct PreparedToneCurve final {
    const ToneCurve* curve = nullptr;
    std::vector<double> segment_slopes;
};

struct PreparedSmoothToneCurve final {
    const ToneCurveSet* curve = nullptr;
    std::vector<double> knot_derivatives;
    bool identity = false;
};

struct PreparedSmoothRgbToneCurve final {
    PreparedSmoothToneCurve master;
    PreparedSmoothToneCurve red;
    PreparedSmoothToneCurve green;
    PreparedSmoothToneCurve blue;
    bool identity = false;
};

using PreparedCurveAdjustment = std::variant<
    std::monostate,
    PreparedToneCurve,
    PreparedSmoothRgbToneCurve>;

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

[[nodiscard]] double smoothstep(
    const double lower,
    const double upper,
    const double value
) noexcept {
    if (value <= lower) {
        return 0.0;
    }
    if (value >= upper) {
        return 1.0;
    }
    const double normalized = (value - lower) / (upper - lower);
    return normalized * normalized * (3.0 - 2.0 * normalized);
}

// A stable base-2 softplus.  It is useful for scene-EV tone fields because, unlike a hard
// threshold or a hand-spliced spline, it remains C-infinity through the point where a tonal
// range hands off to the midtones.  The branch form avoids overflowing exp2() for perfectly
// valid super-white float samples.
[[nodiscard]] double log2_one_plus_exp2(const double value) noexcept {
    constexpr double reciprocal_ln2 = 1.4426950408889634074;
    if (value >= 0.0) {
        return value + reciprocal_ln2 * std::log1p(std::exp2(-value));
    }
    return reciprocal_ln2 * std::log1p(std::exp2(value));
}

// A smooth non-negative EV field which is approximately (boundary - value) below the
// boundary and decays continuously above it.  Its derivative is always in [-1, 0], so a
// bounded multiple can lift/deepen a tonal range without ever folding the scene-linear tone
// mapping back on itself.  The mirrored form below has the opposite derivative.
[[nodiscard]] double lower_ev_hinge(
    const double value,
    const double boundary,
    const double softness
) noexcept {
    return softness * log2_one_plus_exp2((boundary - value) / softness);
}

[[nodiscard]] double upper_ev_hinge(
    const double value,
    const double boundary,
    const double softness
) noexcept {
    return softness * log2_one_plus_exp2((value - boundary) / softness);
}

// Map scene-linear luminance through a bounded contrast curve while keeping its RGB chromatic
// ratios intact.  The pivot is first mapped into a finite "display-like" domain, so even a
// strong contrast setting never drives a positive input below zero or turns a bright RAW value
// into a hard clip.  This is deliberately a global, per-pixel operation: it must give the same
// result for a full image and for an independently rendered detail tile.
[[nodiscard]] Vector3 apply_scene_contrast(
    const Vector3& input,
    const std::array<double, 3>& luminance_weights,
    const ContrastAdjustment& parameters
) noexcept {
    if (parameters.factor == 1.0) {
        return input;
    }

    const double luminance = input[0] * luminance_weights[0]
        + input[1] * luminance_weights[1]
        + input[2] * luminance_weights[2];
    if (!(luminance > 0.0)) {
        return input;
    }

    // The public UI uses 0.18. Keep a finite denominator for programmatic requests that use a
    // zero pivot, rather than risking a divide-by-zero in the curved representation.
    const double pivot = std::max(parameters.pivot, 1.0e-6);
    if (parameters.factor == 0.0) {
        const double gain = pivot / luminance;
        return {input[0] * gain, input[1] * gain, input[2] * gain};
    }

    const double normalized = luminance / (luminance + pivot);
    // Factor is multiplicative in the public contract, but maps to a restrained signed amount
    // internally. The clamp protects scripted factor values (the bridge allows up to 8x) from
    // producing an unstable shoulder.
    // A previous coefficient made the upper half of the UI range behave like a dramatic
    // S-curve: factor 2.5 could more than double a one-stop highlight. Map the multiplicative
    // public control to a deliberately gentler log-domain amount instead. This keeps contrast
    // visibly directional around middle gray while preserving recoverable highlight headroom and
    // avoiding crushed shadows before the dedicated regional controls get a chance to act.
    const double amount = std::clamp(std::log2(parameters.factor) * 0.20, -0.45, 0.45);
    const double shaped = normalized
        + amount * 2.0 * normalized * (1.0 - normalized) * (2.0 * normalized - 1.0);
    const double bounded = std::clamp(shaped, 1.0e-7, 1.0 - 1.0e-7);
    const double adjusted_luminance = pivot * bounded / (1.0 - bounded);
    const double gain = adjusted_luminance / luminance;
    return {input[0] * gain, input[1] * gain, input[2] * gain};
}

[[nodiscard]] double adjusted_selective_tone_ev(
    const double mask_ev,
    const SelectiveToneAdjustment& parameters
) noexcept {
    // Work in a fixed scene-EV coordinate system relative to 18% middle gray.  These are
    // photographer controls, not an auto-exposure system: translating all four zones according
    // to the current image median made the same slider value act differently on every frame.
    // Keeping their anchors fixed makes Recipes portable between photos and makes black/white
    // endpoints visibly distinct from the wider shadow/highlight recovery controls.
    // A soft logarithmic hinge has continuous derivatives and an explicit infinite tail.  The
    // endpoint controls are anchored farther from middle gray and are narrower; the recovery
    // controls deliberately reach into the adjacent midtones.  This separates their useful
    // ranges while avoiding a hard mask boundary that would show as a contour in a gradient.
    constexpr double endpoint_strength = 0.78;
    constexpr double endpoint_boundary_ev = 1.75;
    constexpr double endpoint_softness_ev = 0.62;
    constexpr double recovery_strength = 0.68;
    constexpr double shadow_boundary_ev = -0.15;
    constexpr double highlight_boundary_ev = 0.75;
    constexpr double recovery_softness_ev = 0.95;

    // Never sum several hinge derivatives from the same source EV.  Although each field is
    // monotonic by itself, an additive combination can fold when Black and Shadow (or Highlight
    // and White) are both at an extreme.  Sequential composition keeps every stage strictly
    // positive-slope because each field strength is below one.
    const auto apply_lower = [](const double source_ev, const double amount,
                                const double boundary, const double softness,
                                const double strength) {
        return source_ev + strength * amount * lower_ev_hinge(source_ev, boundary, softness);
    };
    const auto apply_upper = [](const double source_ev, const double amount,
                                const double boundary, const double softness,
                                const double strength) {
        return source_ev + strength * amount * upper_ev_hinge(source_ev, boundary, softness);
    };

    double adjusted_ev = mask_ev;
    adjusted_ev = apply_lower(
        adjusted_ev,
        parameters.blacks,
        -endpoint_boundary_ev,
        endpoint_softness_ev,
        endpoint_strength
    );
    adjusted_ev = apply_lower(
        adjusted_ev,
        parameters.shadows,
        shadow_boundary_ev,
        recovery_softness_ev,
        recovery_strength
    );
    adjusted_ev = apply_upper(
        adjusted_ev,
        parameters.highlights,
        highlight_boundary_ev,
        recovery_softness_ev,
        recovery_strength
    );
    adjusted_ev = apply_upper(
        adjusted_ev,
        parameters.whites,
        endpoint_boundary_ev,
        endpoint_softness_ev,
        endpoint_strength
    );

    return adjusted_ev;
}

[[nodiscard]] std::size_t reflect101_index(
    std::int64_t index,
    std::size_t extent
) noexcept;

[[nodiscard]] float checked_float(
    double value,
    std::size_t node_index,
    const AdjustmentNode& node
);

[[nodiscard]] Vector3 apply_selective_tone_at_mask(
    const Vector3& input,
    const std::array<double, 3>& luminance_weights,
    const SelectiveToneAdjustment& parameters,
    const double mask_ev
) noexcept {
    const double luminance = input[0] * luminance_weights[0]
        + input[1] * luminance_weights[1]
        + input[2] * luminance_weights[2];
    if (!(luminance > 0.0)) {
        return input;
    }

    // The EV field is evaluated against the guided mask, not individual pixel luminance. The
    // original pixel still receives a single common RGB gain, which keeps chromatic ratios
    // intact and avoids a per-channel halo at high-contrast boundaries.
    const double adjusted_ev = adjusted_selective_tone_ev(mask_ev, parameters);
    const double stops = adjusted_ev - mask_ev;
    const double gain = std::exp2(stops);
    if (!(gain > 0.0) || !std::isfinite(gain)) {
        return input;
    }
    return {input[0] * gain, input[1] * gain, input[2] * gain};
}

[[nodiscard]] std::uint32_t selective_tone_mask_radius(
    const double level_zero_to_raster_scale
) {
    const double scaled = selective_tone_guided_mask_radius_level_zero
        * level_zero_to_raster_scale;
    if (!std::isfinite(scaled) || scaled <= 0.0
        || scaled > static_cast<double>(std::numeric_limits<std::uint32_t>::max() - 1U)) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-mask radius exceeds the supported integer range"
        );
    }
    return static_cast<std::uint32_t>(std::max(1.0, std::ceil(scaled)));
}

[[nodiscard]] std::size_t selective_tone_box_window_length(const std::uint32_t radius) {
    if (static_cast<std::size_t>(radius)
        > (std::numeric_limits<std::size_t>::max() - 1U) / 2U) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter box window exceeds the address space"
        );
    }
    return static_cast<std::size_t>(radius) * 2U + 1U;
}

[[nodiscard]] std::uint32_t selective_tone_guided_filter_support_radius(
    const double level_zero_to_raster_scale
) {
    const std::uint32_t local_radius = selective_tone_mask_radius(level_zero_to_raster_scale);
    if (local_radius
        > std::numeric_limits<std::uint32_t>::max()
            / selective_tone_guided_filter_box_passes) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter support exceeds the supported integer range"
        );
    }
    return local_radius * selective_tone_guided_filter_box_passes;
}

struct SelectiveToneGuidedCoefficients final {
    std::vector<float> a;
    std::vector<float> b;
};

[[nodiscard]] float checked_guided_filter_coefficient(
    const double value,
    const std::string_view label
) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maximum || value > maximum) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter " + std::string(label)
                + " exceeded finite float32 range"
        );
    }
    return static_cast<float>(value);
}

// Calculate local coefficients for the complete self-guided filter used by regional tone. The
// guide and source are both log2 scene luminance relative to 18% gray. In every box window,
// a = variance/(variance + epsilon) and b = mean - a*mean. A second reflected box pass averages
// those coefficients before q = mean(a)*I + mean(b) is evaluated. This is the canonical guided
// filter construction, rather than the earlier one-pass local-linear response. A global exposure
// gain is an additive offset in this domain, so the smoothing behaviour is exposure-independent.
//
// Both box stages use rolling horizontal sums plus streaming vertical sums: O(width*height)
// arithmetic, O(width*height) coefficient storage, and O(width) temporary rows. Keeping a and b
// in float32 bounds the transient workspace to two scalar rasters and lets the second pass apply
// its result directly to RGB without materializing a third full-frame mask. Reflected borders make
// full-frame and apron-expanded tile execution deterministic when the scheduler supplies the
// complete two-box support.
[[nodiscard]] SelectiveToneGuidedCoefficients selective_tone_guided_coefficients(
    const FloatRgbImage& image,
    const std::array<double, 3>& luminance_weights
) {
    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter pixel count exceeds the address space"
        );
    }
    const std::size_t pixels = width * height;
    if (
        pixels > std::vector<float>{}.max_size()
        || pixels > std::numeric_limits<std::size_t>::max() / (2U * sizeof(float))
    ) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter workspace exceeds the address space"
        );
    }
    SelectiveToneGuidedCoefficients coefficients;
    try {
        coefficients.a.resize(pixels);
        coefficients.b.resize(pixels);
    } catch (const std::bad_alloc&) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter workspace could not be allocated"
        );
    }

    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    constexpr double minimum_positive_luminance = 5.9604644775390625e-8; // 2^-24
    constexpr double mask_edge_threshold_ev = 0.12;
    constexpr double epsilon = mask_edge_threshold_ev * mask_edge_threshold_ev;
    const std::uint32_t radius_x = selective_tone_mask_radius(
        image.level_zero_to_raster_scale_x
    );
    const std::uint32_t radius_y = selective_tone_mask_radius(
        image.level_zero_to_raster_scale_y
    );
    const std::size_t window_width = selective_tone_box_window_length(radius_x);
    const std::size_t window_height = selective_tone_box_window_length(radius_y);
    const auto log_luminance_at = [&image, luminance_weights, stride,
                                   minimum_positive_luminance](
                                      const std::size_t x,
                                      const std::size_t y
                                  ) {
        const std::size_t sample = y * stride + x * rgb_channels;
        const double luminance = static_cast<double>(image.samples[sample])
                * luminance_weights[0]
            + static_cast<double>(image.samples[sample + 1U]) * luminance_weights[1]
            + static_cast<double>(image.samples[sample + 2U]) * luminance_weights[2];
        const double value = std::log2(std::max(luminance, minimum_positive_luminance) / 0.18);
        if (!std::isfinite(value)) {
            throw EditError(
                EditErrorCode::numeric_overflow,
                std::nullopt,
                "selective tone guided-filter log luminance is non-finite"
            );
        }
        return value;
    };

    if (
        width > std::vector<double>{}.max_size()
        || width > std::numeric_limits<std::size_t>::max() / (4U * sizeof(double))
    ) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter row workspace exceeds the address space"
        );
    }
    std::vector<double> row_mean;
    std::vector<double> row_mean_square;
    std::vector<double> vertical_sum;
    std::vector<double> vertical_sum_square;
    try {
        row_mean.resize(width);
        row_mean_square.resize(width);
        vertical_sum.assign(width, 0.0);
        vertical_sum_square.assign(width, 0.0);
    } catch (const std::bad_alloc&) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter row workspace could not be allocated"
        );
    }

    const auto make_horizontal_row = [&](const std::int64_t unbounded_y) {
        const std::size_t source_y = reflect101_index(unbounded_y, height);
        double sum = 0.0;
        double sum_square = 0.0;
        for (std::int64_t offset = -static_cast<std::int64_t>(radius_x);
             offset <= static_cast<std::int64_t>(radius_x);
             ++offset) {
            const double value = log_luminance_at(reflect101_index(offset, width), source_y);
            sum += value;
            sum_square += value * value;
        }
        for (std::size_t x = 0U; x < width; ++x) {
            row_mean[x] = sum / static_cast<double>(window_width);
            row_mean_square[x] = sum_square / static_cast<double>(window_width);
            if (x + 1U == width) {
                continue;
            }
            const double removed = log_luminance_at(
                reflect101_index(
                    static_cast<std::int64_t>(x) - static_cast<std::int64_t>(radius_x),
                    width
                ),
                source_y
            );
            const double added = log_luminance_at(
                reflect101_index(
                    static_cast<std::int64_t>(x) + static_cast<std::int64_t>(radius_x) + 1,
                    width
                ),
                source_y
            );
            sum += added - removed;
            sum_square += added * added - removed * removed;
        }
    };
    const auto accumulate_row = [&make_horizontal_row, &row_mean, &row_mean_square,
                                 &vertical_sum, &vertical_sum_square](
                                    const std::int64_t source_y,
                                    const double factor
                                ) {
        make_horizontal_row(source_y);
        for (std::size_t x = 0U; x < row_mean.size(); ++x) {
            vertical_sum[x] += factor * row_mean[x];
            vertical_sum_square[x] += factor * row_mean_square[x];
        }
    };

    for (std::int64_t offset = -static_cast<std::int64_t>(radius_y);
         offset <= static_cast<std::int64_t>(radius_y);
         ++offset) {
        accumulate_row(offset, 1.0);
    }
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            const double mean = vertical_sum[x] / static_cast<double>(window_height);
            const double mean_square = vertical_sum_square[x]
                / static_cast<double>(window_height);
            // Cancellation can make a mathematically non-negative variance a few ulps below
            // zero on a flat field. Clamp only that roundoff, never the source luminance.
            const double variance = std::max(0.0, mean_square - mean * mean);
            const double a = std::clamp(variance / (variance + epsilon), 0.0, 1.0);
            const double b = mean - a * mean;
            const std::size_t pixel = y * width + x;
            coefficients.a[pixel] = checked_guided_filter_coefficient(a, "a coefficient");
            coefficients.b[pixel] = checked_guided_filter_coefficient(b, "b coefficient");
        }
        if (y + 1U < height) {
            accumulate_row(
                static_cast<std::int64_t>(y) - static_cast<std::int64_t>(radius_y),
                -1.0
            );
            accumulate_row(
                static_cast<std::int64_t>(y) + static_cast<std::int64_t>(radius_y) + 1,
                1.0
            );
        }
    }
    return coefficients;
}

void apply_guided_selective_tone(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SelectiveToneAdjustment& parameters
) {
    const auto luminance_weights = image.working_space.luminance_coefficients;
    const auto coefficients = selective_tone_guided_coefficients(image, luminance_weights);
    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    const std::uint32_t radius_x = selective_tone_mask_radius(
        image.level_zero_to_raster_scale_x
    );
    const std::uint32_t radius_y = selective_tone_mask_radius(
        image.level_zero_to_raster_scale_y
    );
    const std::size_t window_width = selective_tone_box_window_length(radius_x);
    const std::size_t window_height = selective_tone_box_window_length(radius_y);
    if (
        width > std::vector<double>{}.max_size()
        || width > std::numeric_limits<std::size_t>::max() / (4U * sizeof(double))
    ) {
        throw_node_error(
            EditErrorCode::numeric_overflow,
            node_index,
            node,
            "selective tone guided-filter row workspace exceeds the address space"
        );
    }
    std::vector<double> row_mean_a;
    std::vector<double> row_mean_b;
    std::vector<double> vertical_sum_a;
    std::vector<double> vertical_sum_b;
    try {
        row_mean_a.resize(width);
        row_mean_b.resize(width);
        vertical_sum_a.assign(width, 0.0);
        vertical_sum_b.assign(width, 0.0);
    } catch (const std::bad_alloc&) {
        throw_node_error(
            EditErrorCode::numeric_overflow,
            node_index,
            node,
            "selective tone guided-filter row workspace could not be allocated"
        );
    }

    const auto make_horizontal_row = [&](const std::int64_t unbounded_y) {
        const std::size_t source_y = reflect101_index(unbounded_y, height);
        double sum_a = 0.0;
        double sum_b = 0.0;
        for (std::int64_t offset = -static_cast<std::int64_t>(radius_x);
             offset <= static_cast<std::int64_t>(radius_x);
             ++offset) {
            const std::size_t source = source_y * width + reflect101_index(offset, width);
            sum_a += coefficients.a[source];
            sum_b += coefficients.b[source];
        }
        for (std::size_t x = 0U; x < width; ++x) {
            row_mean_a[x] = sum_a / static_cast<double>(window_width);
            row_mean_b[x] = sum_b / static_cast<double>(window_width);
            if (x + 1U == width) {
                continue;
            }
            const std::size_t removed_x = reflect101_index(
                static_cast<std::int64_t>(x) - static_cast<std::int64_t>(radius_x),
                width
            );
            const std::size_t added_x = reflect101_index(
                static_cast<std::int64_t>(x) + static_cast<std::int64_t>(radius_x) + 1,
                width
            );
            const std::size_t removed = source_y * width + removed_x;
            const std::size_t added = source_y * width + added_x;
            sum_a += static_cast<double>(coefficients.a[added])
                - static_cast<double>(coefficients.a[removed]);
            sum_b += static_cast<double>(coefficients.b[added])
                - static_cast<double>(coefficients.b[removed]);
        }
    };
    const auto accumulate_row = [&make_horizontal_row, &row_mean_a, &row_mean_b,
                                 &vertical_sum_a, &vertical_sum_b](
                                    const std::int64_t source_y,
                                    const double factor
                                ) {
        make_horizontal_row(source_y);
        for (std::size_t x = 0U; x < row_mean_a.size(); ++x) {
            vertical_sum_a[x] += factor * row_mean_a[x];
            vertical_sum_b[x] += factor * row_mean_b[x];
        }
    };
    for (std::int64_t offset = -static_cast<std::int64_t>(radius_y);
         offset <= static_cast<std::int64_t>(radius_y);
         ++offset) {
        accumulate_row(offset, 1.0);
    }

    constexpr double minimum_positive_luminance = 5.9604644775390625e-8; // 2^-24
    for (std::uint32_t y = 0U; y < image.dimensions.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0U; x < image.dimensions.width; ++x) {
            const std::size_t sample = row + static_cast<std::size_t>(x) * rgb_channels;
            const Vector3 input{
                image.samples[sample],
                image.samples[sample + 1U],
                image.samples[sample + 2U],
            };
            const double source_luminance = input[0] * luminance_weights[0]
                + input[1] * luminance_weights[1] + input[2] * luminance_weights[2];
            const double source_ev = std::log2(
                std::max(source_luminance, minimum_positive_luminance) / 0.18
            );
            const double mean_a = vertical_sum_a[x] / static_cast<double>(window_height);
            const double mean_b = vertical_sum_b[x] / static_cast<double>(window_height);
            const double mask_ev = mean_a * source_ev + mean_b;
            if (!std::isfinite(mask_ev)) {
                throw_node_error(
                    EditErrorCode::numeric_overflow,
                    node_index,
                    node,
                    "selective tone guided-filter output is non-finite"
                );
            }
            const Vector3 output = apply_selective_tone_at_mask(
                input,
                luminance_weights,
                parameters,
                mask_ev
            );
            image.samples[sample] = checked_float(output[0], node_index, node);
            image.samples[sample + 1U] = checked_float(output[1], node_index, node);
            image.samples[sample + 2U] = checked_float(output[2], node_index, node);
        }
        if (y + 1U < image.dimensions.height) {
            accumulate_row(
                static_cast<std::int64_t>(y) - static_cast<std::int64_t>(radius_y),
                -1.0
            );
            accumulate_row(
                static_cast<std::int64_t>(y) + static_cast<std::int64_t>(radius_y) + 1,
                1.0
            );
        }
    }
}

[[nodiscard]] double wrap_degrees(const double degrees) noexcept {
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

[[nodiscard]] double signed_hue_distance(
    const double hue,
    const double center
) noexcept {
    return std::remainder(hue - center, 360.0);
}

[[nodiscard]] std::array<double, perceptual_hue_band_count> hue_band_weights(
    const double hue
) noexcept {
    std::array<double, perceptual_hue_band_count> weights{};

    const double wrapped_hue = wrap_degrees(hue);
    const auto upper = std::upper_bound(
        perceptual_hue_anchors.begin(),
        perceptual_hue_anchors.end(),
        wrapped_hue
    );
    const std::size_t right = upper == perceptual_hue_anchors.end()
        ? 0U
        : static_cast<std::size_t>(upper - perceptual_hue_anchors.begin());
    const std::size_t left = right == 0U ? weights.size() - 1U : right - 1U;

    const double left_hue = perceptual_hue_anchors[left];
    const double right_hue = right == 0U
        ? perceptual_hue_anchors.front() + 360.0
        : perceptual_hue_anchors[right];
    const double unwrapped_hue = right == 0U && wrapped_hue < left_hue
        ? wrapped_hue + 360.0
        : wrapped_hue;
    const double position = std::clamp(
        (unwrapped_hue - left_hue) / (right_hue - left_hue),
        0.0,
        1.0
    );

    // A raised-cosine crossfade is C1 at every non-uniform anchor. Assigning
    // the left weight as the complement makes the periodic pair an exact
    // partition of unity (including the magenta/red seam).
    const double right_weight = 0.5 * (1.0 - std::cos(pi * position));
    weights[left] = 1.0 - right_weight;
    weights[right] = right_weight;
    if (left == right) {
        weights[left] = 1.0;
    }
    return weights;
}

[[nodiscard]] double color_range_weight(
    const PerceptualColorRange& range,
    const double hue
) noexcept {
    if (!range.enabled) {
        return 0.0;
    }
    const double distance = std::abs(signed_hue_distance(hue, range.center_degrees));
    const double feather = range.width_degrees * range.softness;
    if (feather == 0.0) {
        return distance <= range.width_degrees ? 1.0 : 0.0;
    }
    const double fully_selected = range.width_degrees - feather;
    return 1.0 - smoothstep(fully_selected, range.width_degrees, distance);
}

[[nodiscard]] bool apply_ordered_color_range(
    Vector3& lab,
    const PerceptualColorRange& range
) noexcept {
    if (!range.enabled) {
        return false;
    }
    const double chroma = std::hypot(lab[1], lab[2]);
    const double relative_chroma = chroma / std::max(1.0e-6, std::abs(lab[0]));
    const double confidence = smoothstep(0.002, 0.02, relative_chroma);
    if (confidence == 0.0) {
        return false;
    }
    const double hue = wrap_degrees(std::atan2(lab[2], lab[1]) * 180.0 / pi);
    const double weight = confidence * color_range_weight(range, hue);
    if (weight == 0.0) {
        return false;
    }
    const double adjusted_hue = (hue + weight * range.hue_shift_degrees) * pi / 180.0;
    const double adjusted_chroma = chroma * (1.0 + weight * range.saturation);
    lab[0] += 0.15 * weight * range.lightness;
    lab[1] = adjusted_chroma * std::cos(adjusted_hue);
    lab[2] = adjusted_chroma * std::sin(adjusted_hue);
    return range.hue_shift_degrees != 0.0 || range.saturation != 0.0
        || range.lightness != 0.0;
}

template <std::size_t Size>
[[nodiscard]] double weighted_sum(
    const std::array<double, Size>& values,
    const std::array<double, Size>& weights
) noexcept {
    double result = 0.0;
    for (std::size_t index = 0U; index < Size; ++index) {
        result += values[index] * weights[index];
    }
    return result;
}

[[nodiscard]] bool normalized_amount(const double value) noexcept {
    return std::isfinite(value) && value >= -1.0 && value <= 1.0;
}

[[nodiscard]] bool selective_tone_is_neutral(
    const SelectiveToneAdjustment& parameters
) noexcept {
    return parameters.highlights == 0.0 && parameters.shadows == 0.0
        && parameters.whites == 0.0 && parameters.blacks == 0.0;
}

[[nodiscard]] bool perceptual_color_is_neutral(
    const PerceptualColorAdjustment& parameters
) noexcept {
    const bool bands_are_neutral = std::ranges::all_of(parameters.hue, [](const double value) {
        return value == 0.0;
    }) && std::ranges::all_of(parameters.saturation, [](const double value) {
        return value == 0.0;
    }) && std::ranges::all_of(parameters.lightness, [](const double value) {
        return value == 0.0;
    });
    const auto range_is_neutral = [](const PerceptualColorRange& range) {
        return !range.enabled
            || (range.hue_shift_degrees == 0.0 && range.saturation == 0.0
                && range.lightness == 0.0);
    };
    const bool ranges_are_neutral = range_is_neutral(parameters.color_range)
        && std::ranges::all_of(parameters.additional_color_ranges, range_is_neutral);
    return parameters.vibrance == 0.0 && bands_are_neutral && ranges_are_neutral;
}

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
        || image.reference != ImageReference::scene_referred
    ) {
        throw EditError(
            EditErrorCode::incompatible_color_encoding,
            std::nullopt,
            "edit input must be scene-referred linear RGB"
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

[[nodiscard]] PreparedToneCurve prepare_tone_curve(const ToneCurve& curve) {
    if (
        curve.parameter_schema_version != tone_curve_parameter_schema_version
        || curve.implementation_version != tone_curve_implementation_version
    ) {
        throw EditError(
            EditErrorCode::unsupported_version,
            std::nullopt,
            "tone curve supports only parameter schema 1 and implementation 1"
        );
    }
    if (curve.points.size() < 2U || curve.points.size() > maximum_tone_curve_points) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "tone curve must contain between 2 and 256 control points"
        );
    }
    if (curve.points.front().x != 0.0 || curve.points.back().x != 1.0) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "tone curve x coordinates must start at zero and end at one"
        );
    }

    PreparedToneCurve prepared{
        .curve = &curve,
        .segment_slopes = {},
    };
    prepared.segment_slopes.reserve(curve.points.size() - 1U);
    for (std::size_t index = 0; index < curve.points.size(); ++index) {
        const ToneCurvePoint point = curve.points[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "tone curve control points must contain only finite values"
            );
        }
        if (index == 0U) {
            continue;
        }

        const ToneCurvePoint previous = curve.points[index - 1U];
        if (point.x <= previous.x) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "tone curve x coordinates must be strictly increasing"
            );
        }
        const double slope = (point.y - previous.y) / (point.x - previous.x);
        if (!std::isfinite(slope)) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "tone curve segment slopes must be finite"
            );
        }
        prepared.segment_slopes.push_back(slope);
    }
    return prepared;
}

[[nodiscard]] double evaluate_tone_curve(
    const PreparedToneCurve& prepared,
    const double value
) {
    const auto& points = prepared.curve->points;
    const auto upper = std::upper_bound(
        points.begin(),
        points.end(),
        value,
        [](const double sample, const ToneCurvePoint& point) { return sample < point.x; }
    );

    std::size_t segment = 0U;
    if (upper == points.end()) {
        segment = points.size() - 2U;
    } else if (upper != points.begin()) {
        segment = static_cast<std::size_t>(upper - points.begin()) - 1U;
    }

    return points[segment].y
        + (value - points[segment].x) * prepared.segment_slopes[segment];
}

[[nodiscard]] bool same_nonzero_sign(const double left, const double right) noexcept {
    return (left > 0.0 && right > 0.0) || (left < 0.0 && right < 0.0);
}

[[nodiscard]] bool identity_curve_set(const ToneCurveSet& curve) noexcept {
    return curve.points.size() == 2U
        && curve.points[0] == ToneCurvePoint{0.0, 0.0}
        && curve.points[1] == ToneCurvePoint{1.0, 1.0};
}

[[nodiscard]] double pchip_endpoint_derivative(
    const double first_width,
    const double second_width,
    const double first_slope,
    const double second_slope
) {
    const double numerator = (2.0 * first_width + second_width) * first_slope
        - first_width * second_slope;
    double derivative = numerator / (first_width + second_width);
    if (first_slope == 0.0 || !same_nonzero_sign(derivative, first_slope)) {
        return 0.0;
    }
    if (
        !same_nonzero_sign(first_slope, second_slope)
        && std::abs(derivative) > 3.0 * std::abs(first_slope)
    ) {
        derivative = 3.0 * first_slope;
    }
    return derivative;
}

[[nodiscard]] PreparedSmoothToneCurve prepare_smooth_tone_curve(
    const ToneCurveSet& curve
) {
    if (curve.points.size() < 2U || curve.points.size() > maximum_tone_curve_points) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "smooth tone curve must contain between 2 and 256 control points"
        );
    }
    if (curve.points.front().x != 0.0 || curve.points.back().x != 1.0) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "smooth tone curve x coordinates must start at zero and end at one"
        );
    }

    std::vector<double> interval_widths;
    std::vector<double> secant_slopes;
    interval_widths.reserve(curve.points.size() - 1U);
    secant_slopes.reserve(curve.points.size() - 1U);
    for (std::size_t index = 0U; index < curve.points.size(); ++index) {
        const ToneCurvePoint point = curve.points[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "smooth tone curve control points must contain only finite values"
            );
        }
        if (index == 0U) {
            continue;
        }
        const ToneCurvePoint previous = curve.points[index - 1U];
        const double width = point.x - previous.x;
        if (width <= 0.0) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "smooth tone curve x coordinates must be strictly increasing"
            );
        }
        const double slope = (point.y - previous.y) / width;
        if (!std::isfinite(slope)) {
            throw EditError(
                EditErrorCode::invalid_parameter,
                std::nullopt,
                "smooth tone curve secant slopes must be finite"
            );
        }
        interval_widths.push_back(width);
        secant_slopes.push_back(slope);
    }

    PreparedSmoothToneCurve prepared{
        .curve = &curve,
        .knot_derivatives = std::vector<double>(curve.points.size(), 0.0),
        .identity = identity_curve_set(curve),
    };
    if (curve.points.size() == 2U) {
        prepared.knot_derivatives[0] = secant_slopes[0];
        prepared.knot_derivatives[1] = secant_slopes[0];
        return prepared;
    }

    prepared.knot_derivatives.front() = pchip_endpoint_derivative(
        interval_widths[0],
        interval_widths[1],
        secant_slopes[0],
        secant_slopes[1]
    );
    for (std::size_t index = 1U; index + 1U < curve.points.size(); ++index) {
        const double previous_slope = secant_slopes[index - 1U];
        const double next_slope = secant_slopes[index];
        // A sign change is an authored local extremum. A zero knot derivative keeps it at
        // that knot rather than inventing an extra oscillation inside either interval.
        if (!same_nonzero_sign(previous_slope, next_slope)) {
            prepared.knot_derivatives[index] = 0.0;
            continue;
        }
        const double previous_width = interval_widths[index - 1U];
        const double next_width = interval_widths[index];
        const double first_weight = 2.0 * next_width + previous_width;
        const double second_weight = next_width + 2.0 * previous_width;
        // Fritsch-Butland's weighted harmonic mean preserves the monotonicity of both
        // adjoining intervals while keeping the first derivative continuous.
        prepared.knot_derivatives[index] = (first_weight + second_weight)
            / (first_weight / previous_slope + second_weight / next_slope);
    }
    const std::size_t last_interval = interval_widths.size() - 1U;
    prepared.knot_derivatives.back() = pchip_endpoint_derivative(
        interval_widths[last_interval],
        interval_widths[last_interval - 1U],
        secant_slopes[last_interval],
        secant_slopes[last_interval - 1U]
    );
    if (!std::ranges::all_of(prepared.knot_derivatives, [](const double derivative) {
            return std::isfinite(derivative);
        })) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "smooth tone curve PCHIP derivatives must be finite"
        );
    }
    return prepared;
}

[[nodiscard]] double evaluate_smooth_tone_curve(
    const PreparedSmoothToneCurve& prepared,
    const double value
) {
    if (prepared.identity) {
        return value;
    }
    const auto& points = prepared.curve->points;
    if (value <= points.front().x) {
        return points.front().y
            + (value - points.front().x) * prepared.knot_derivatives.front();
    }
    if (value >= points.back().x) {
        return points.back().y
            + (value - points.back().x) * prepared.knot_derivatives.back();
    }

    const auto upper = std::upper_bound(
        points.begin(),
        points.end(),
        value,
        [](const double sample, const ToneCurvePoint& point) { return sample < point.x; }
    );
    const std::size_t segment = static_cast<std::size_t>(upper - points.begin()) - 1U;
    const ToneCurvePoint left = points[segment];
    const ToneCurvePoint right = points[segment + 1U];
    const double width = right.x - left.x;
    const double t = (value - left.x) / width;
    const double t_squared = t * t;
    const double t_cubed = t_squared * t;
    const double h00 = 2.0 * t_cubed - 3.0 * t_squared + 1.0;
    const double h10 = t_cubed - 2.0 * t_squared + t;
    const double h01 = -2.0 * t_cubed + 3.0 * t_squared;
    const double h11 = t_cubed - t_squared;
    return h00 * left.y + h10 * width * prepared.knot_derivatives[segment]
        + h01 * right.y + h11 * width * prepared.knot_derivatives[segment + 1U];
}

[[nodiscard]] PreparedSmoothRgbToneCurve prepare_smooth_rgb_tone_curve(
    const SmoothRgbToneCurve& curve
) {
    if (
        curve.parameter_schema_version != smooth_rgb_tone_curve_parameter_schema_version
        || curve.implementation_version != smooth_rgb_tone_curve_implementation_version
    ) {
        throw EditError(
            EditErrorCode::unsupported_version,
            std::nullopt,
            "smooth RGB tone curve supports only parameter schema 2 and implementation 2"
        );
    }
    PreparedSmoothRgbToneCurve prepared{
        .master = prepare_smooth_tone_curve(curve.master),
        .red = prepare_smooth_tone_curve(curve.red),
        .green = prepare_smooth_tone_curve(curve.green),
        .blue = prepare_smooth_tone_curve(curve.blue),
    };
    prepared.identity = prepared.master.identity && prepared.red.identity
        && prepared.green.identity && prepared.blue.identity;
    return prepared;
}

[[nodiscard]] float checked_tone_curve_float(const double value) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maximum || value > maximum) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "tone curve pixel result exceeded finite float32 range"
        );
    }
    return static_cast<float>(value);
}

template <typename CheckedConversion>
void apply_prepared_tone_curve(
    FloatRgbImage& image,
    const PreparedToneCurve& prepared,
    CheckedConversion&& checked_conversion
) {
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    for (std::uint32_t y = 0; y < image.dimensions.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < image.dimensions.width; ++x) {
            const std::size_t sample = row + static_cast<std::size_t>(x) * rgb_channels;
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                image.samples[sample + channel] = checked_conversion(
                    evaluate_tone_curve(
                        prepared,
                        static_cast<double>(image.samples[sample + channel])
                    )
                );
            }
        }
    }
}

template <typename CheckedConversion>
void apply_prepared_smooth_rgb_tone_curve(
    FloatRgbImage& image,
    const PreparedSmoothRgbToneCurve& prepared,
    CheckedConversion&& checked_conversion
) {
    if (prepared.identity) {
        return;
    }
    const std::array<const PreparedSmoothToneCurve*, rgb_channels> channels{
        &prepared.red,
        &prepared.green,
        &prepared.blue,
    };
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    for (std::uint32_t y = 0U; y < image.dimensions.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0U; x < image.dimensions.width; ++x) {
            const std::size_t sample = row + static_cast<std::size_t>(x) * rgb_channels;
            for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                const double master_value = evaluate_smooth_tone_curve(
                    prepared.master,
                    static_cast<double>(image.samples[sample + channel])
                );
                image.samples[sample + channel] = checked_conversion(
                    evaluate_smooth_tone_curve(*channels[channel], master_value)
                );
            }
        }
    }
}

[[nodiscard]] PreparedToneCurve prepare_tone_curve_node(
    const ToneCurve& curve,
    const AdjustmentNode& node,
    const std::size_t index
) {
    try {
        return prepare_tone_curve(curve);
    } catch (const EditError& error) {
        throw_node_error(error.code(), index, node, error.what());
    }
}

[[nodiscard]] PreparedSmoothRgbToneCurve prepare_smooth_rgb_tone_curve_node(
    const SmoothRgbToneCurve& curve,
    const AdjustmentNode& node,
    const std::size_t index
) {
    try {
        return prepare_smooth_rgb_tone_curve(curve);
    } catch (const EditError& error) {
        throw_node_error(error.code(), index, node, error.what());
    }
}

[[nodiscard]] PreparedCurveAdjustment validate_node(
    const AdjustmentNode& node,
    const std::size_t index
) {
    const bool smooth_rgb_tone_curve = std::holds_alternative<SmoothRgbToneCurve>(node.parameters);
    const bool selective_tone = std::holds_alternative<SelectiveToneAdjustment>(
        node.parameters
    );
    const bool perceptual_color = std::holds_alternative<PerceptualColorAdjustment>(
        node.parameters
    );
    const bool detail_effects = std::holds_alternative<SharpenAdjustment>(node.parameters);
    const std::uint32_t expected_parameter_schema = smooth_rgb_tone_curve
        ? smooth_rgb_tone_curve_parameter_schema_version
        : selective_tone ? selective_tone_v3_parameter_schema_version
        : perceptual_color ? perceptual_color_v2_parameter_schema_version
        : detail_effects ? detail_effects_v3_parameter_schema_version
                         : adjustment_parameter_schema_version;
    const std::uint32_t expected_implementation = smooth_rgb_tone_curve
        ? smooth_rgb_tone_curve_implementation_version
        : selective_tone ? selective_tone_v3_implementation_version
        : perceptual_color ? perceptual_color_v2_implementation_version
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
            smooth_rgb_tone_curve
                ? "smooth RGB tone curve requires parameter schema 2 and implementation 2"
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
            } else if constexpr (std::is_same_v<Parameters, ToneCurve>) {
                prepared_curve = prepare_tone_curve_node(parameters, node, index);
            } else if constexpr (std::is_same_v<Parameters, SmoothRgbToneCurve>) {
                prepared_curve = prepare_smooth_rgb_tone_curve_node(parameters, node, index);
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
                if (!normalized_amount(parameters.vibrance) || !valid_bands || !valid_ranges) {
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

[[nodiscard]] float checked_float(
    const double value,
    const std::size_t node_index,
    const AdjustmentNode& node
) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maximum || value > maximum) {
        throw_node_error(
            EditErrorCode::numeric_overflow,
            node_index,
            node,
            "pixel result exceeded finite float32 range"
        );
    }
    return static_cast<float>(value);
}

template <typename Transform>
void transform_rgb_pixels(
    FloatRgbImage& image,
    const std::size_t node_index,
    const AdjustmentNode& node,
    Transform&& transform
) {
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    for (std::uint32_t y = 0; y < image.dimensions.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < image.dimensions.width; ++x) {
            const std::size_t sample = row + static_cast<std::size_t>(x) * rgb_channels;
            const std::array<double, 3> input{
                static_cast<double>(image.samples[sample]),
                static_cast<double>(image.samples[sample + 1U]),
                static_cast<double>(image.samples[sample + 2U]),
            };
            const std::array<double, 3> output = transform(input);
            image.samples[sample] = checked_float(output[0], node_index, node);
            image.samples[sample + 1U] = checked_float(output[1], node_index, node);
            image.samples[sample + 2U] = checked_float(output[2], node_index, node);
        }
    }
}

[[nodiscard]] std::size_t reflect101_index(
    std::int64_t index,
    const std::size_t extent
) noexcept {
    if (extent <= 1U) {
        return 0U;
    }
    const auto signed_extent = static_cast<std::int64_t>(extent);
    while (index < 0 || index >= signed_extent) {
        if (index < 0) {
            index = -index;
        } else {
            index = 2 * signed_extent - 2 - index;
        }
    }
    return static_cast<std::size_t>(index);
}

[[nodiscard]] std::vector<double> gaussian_kernel(
    const double sigma,
    const std::uint32_t radius
) {
    const std::size_t size = static_cast<std::size_t>(radius) * 2U + 1U;
    std::vector<double> kernel(size);
    const double inverse_two_sigma_squared = 1.0 / (2.0 * sigma * sigma);
    double sum = 0.0;
    for (std::int64_t offset = -static_cast<std::int64_t>(radius);
         offset <= static_cast<std::int64_t>(radius);
         ++offset) {
        const double coordinate = static_cast<double>(offset);
        const double value = std::exp(-(coordinate * coordinate) * inverse_two_sigma_squared);
        kernel[static_cast<std::size_t>(offset + static_cast<std::int64_t>(radius))] = value;
        sum += value;
    }
    for (double& value : kernel) {
        value /= sum;
    }
    return kernel;
}

void apply_sharpen(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters
) {
    if (parameters.amount == 0.0) {
        return;
    }

    const AdjustmentFootprint support = footprint(
        parameters,
        image.level_zero_to_raster_scale_x,
        image.level_zero_to_raster_scale_y
    );
    const double sigma_x = parameters.radius * image.level_zero_to_raster_scale_x;
    const double sigma_y = parameters.radius * image.level_zero_to_raster_scale_y;
    const auto kernel_x = gaussian_kernel(sigma_x, support.horizontal_radius);
    const auto kernel_y = gaussian_kernel(sigma_y, support.vertical_radius);

    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        throw_node_error(
            EditErrorCode::numeric_overflow,
            node_index,
            node,
            "sharpen working buffer exceeds the address space"
        );
    }
    const std::size_t pixels = width * height;
    std::vector<double> log_luminance(pixels);
    std::vector<double> horizontal_blur(pixels);
    std::vector<double> blurred(pixels);
    const auto weights = image.working_space.luminance_coefficients;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    constexpr double minimum_positive_luminance = 5.9604644775390625e-8; // 2^-24

    for (std::size_t y = 0U; y < height; ++y) {
        const std::size_t row = y * stride;
        for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t sample = row + x * rgb_channels;
            const double luminance = static_cast<double>(image.samples[sample]) * weights[0]
                + static_cast<double>(image.samples[sample + 1U]) * weights[1]
                + static_cast<double>(image.samples[sample + 2U]) * weights[2];
            log_luminance[y * width + x] = std::log2(
                std::max(luminance, minimum_positive_luminance)
            );
        }
    }

    const auto radius_x = static_cast<std::int64_t>(support.horizontal_radius);
    const auto radius_y = static_cast<std::int64_t>(support.vertical_radius);
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            double sum = 0.0;
            for (std::int64_t offset = -radius_x; offset <= radius_x; ++offset) {
                const std::size_t source_x = reflect101_index(
                    static_cast<std::int64_t>(x) + offset,
                    width
                );
                sum += log_luminance[y * width + source_x]
                    * kernel_x[static_cast<std::size_t>(offset + radius_x)];
            }
            horizontal_blur[y * width + x] = sum;
        }
    }
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            double sum = 0.0;
            for (std::int64_t offset = -radius_y; offset <= radius_y; ++offset) {
                const std::size_t source_y = reflect101_index(
                    static_cast<std::int64_t>(y) + offset,
                    height
                );
                sum += horizontal_blur[source_y * width + x]
                    * kernel_y[static_cast<std::size_t>(offset + radius_y)];
            }
            blurred[y * width + x] = sum;
        }
    }

    const double threshold_ev = parameters.threshold * 0.25;
    for (std::size_t y = 0U; y < height; ++y) {
        const std::size_t row = y * stride;
        for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t pixel = y * width + x;
            const std::size_t sample = row + x * rgb_channels;
            const double luminance = static_cast<double>(image.samples[sample]) * weights[0]
                + static_cast<double>(image.samples[sample + 1U]) * weights[1]
                + static_cast<double>(image.samples[sample + 2U]) * weights[2];
            if (luminance <= minimum_positive_luminance) {
                continue;
            }

            const double detail = log_luminance[pixel] - blurred[pixel];
            // The tiny guard makes a mathematically flat field exactly neutral despite the
            // unavoidable roundoff of a normalized separable convolution.
            if (std::abs(detail) <= 1.0e-12) {
                continue;
            }
            const double thresholded = std::copysign(
                std::max(0.0, std::abs(detail) - threshold_ev),
                detail
            );
            if (thresholded == 0.0) {
                continue;
            }
            const double edge_confidence = smoothstep(
                threshold_ev,
                threshold_ev + 0.25,
                std::abs(detail)
            );
            const double mask = (1.0 - parameters.masking)
                + parameters.masking * edge_confidence;
            const double gain = std::exp2(parameters.amount * thresholded * mask);
            image.samples[sample] = checked_float(
                static_cast<double>(image.samples[sample]) * gain,
                node_index,
                node
            );
            image.samples[sample + 1U] = checked_float(
                static_cast<double>(image.samples[sample + 1U]) * gain,
                node_index,
                node
            );
            image.samples[sample + 2U] = checked_float(
                static_cast<double>(image.samples[sample + 2U]) * gain,
                node_index,
                node
            );
        }
    }
}

void apply_edge_aware_denoise(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters
) {
    if (parameters.denoise_luminance == 0.0 && parameters.denoise_color == 0.0) {
        return;
    }
    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    const auto source = image.samples;
    const auto weights = image.working_space.luminance_coefficients;
    const double range_sigma = 0.025 + 0.18 * (1.0 - parameters.denoise_detail);
    const double inverse_range = 1.0 / (2.0 * range_sigma * range_sigma);
    // The UI defines detail radius in level-zero (native RAW) pixels. A fixed 5x5 kernel on a
    // 1200px warm proxy would otherwise denoise a much larger physical region than the same
    // setting on a full-detail tile. Preserve the native sigma, then convert it separately to
    // each raster axis just as capture sharpening already does.
    constexpr double denoise_native_sigma = 1.5;
    constexpr double denoise_native_support = 2.0;
    const double sigma_x = std::max(
        0.20,
        denoise_native_sigma * image.level_zero_to_raster_scale_x
    );
    const double sigma_y = std::max(
        0.20,
        denoise_native_sigma * image.level_zero_to_raster_scale_y
    );
    const std::int64_t radius_x = std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(std::ceil(
            denoise_native_support * image.level_zero_to_raster_scale_x
        ))
    );
    const std::int64_t radius_y = std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(std::ceil(
            denoise_native_support * image.level_zero_to_raster_scale_y
        ))
    );
    const double inverse_two_sigma_x_squared = 1.0 / (2.0 * sigma_x * sigma_x);
    const double inverse_two_sigma_y_squared = 1.0 / (2.0 * sigma_y * sigma_y);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t center = y * stride + x * rgb_channels;
            const Vector3 original{
                source[center], source[center + 1U], source[center + 2U],
            };
            const double original_luma = original[0] * weights[0]
                + original[1] * weights[1] + original[2] * weights[2];
            Vector3 filtered{};
            double weight_sum = 0.0;
            for (std::int64_t dy = -radius_y; dy <= radius_y; ++dy) {
                const std::size_t source_y = reflect101_index(
                    static_cast<std::int64_t>(y) + dy, height
                );
                for (std::int64_t dx = -radius_x; dx <= radius_x; ++dx) {
                    const std::size_t source_x = reflect101_index(
                        static_cast<std::int64_t>(x) + dx, width
                    );
                    const std::size_t sample = source_y * stride + source_x * rgb_channels;
                    const Vector3 neighbor{
                        source[sample], source[sample + 1U], source[sample + 2U],
                    };
                    const double neighbor_luma = neighbor[0] * weights[0]
                        + neighbor[1] * weights[1] + neighbor[2] * weights[2];
                    const double delta = neighbor_luma - original_luma;
                    const double spatial = static_cast<double>(dx * dx)
                            * inverse_two_sigma_x_squared
                        + static_cast<double>(dy * dy) * inverse_two_sigma_y_squared;
                    const double weight = std::exp(-spatial - delta * delta * inverse_range);
                    for (std::size_t channel = 0; channel < rgb_channels; ++channel) {
                        filtered[channel] += neighbor[channel] * weight;
                    }
                    weight_sum += weight;
                }
            }
            for (double& channel : filtered) {
                channel /= weight_sum;
            }
            const double filtered_luma = filtered[0] * weights[0]
                + filtered[1] * weights[1] + filtered[2] * weights[2];
            const double luminance = std::lerp(
                original_luma, filtered_luma, parameters.denoise_luminance
            );
            for (std::size_t channel = 0; channel < rgb_channels; ++channel) {
                const double original_chroma = original[channel] - original_luma;
                const double filtered_chroma = filtered[channel] - filtered_luma;
                const double output = luminance + std::lerp(
                    original_chroma, filtered_chroma, parameters.denoise_color
                );
                image.samples[center + channel] = checked_float(output, node_index, node);
            }
        }
    }
}

void apply_dehaze_and_defringe(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters
) {
    if (parameters.dehaze == 0.0
        && parameters.defringe_purple_amount == 0.0
        && parameters.defringe_green_amount == 0.0) {
        return;
    }
    const auto luma_weights = image.working_space.luminance_coefficients;
    const WorkingSpaceTransform color_transform = prepare_working_space_transform(
        image.working_space, node, node_index
    );
    transform_rgb_pixels(
        image,
        node_index,
        node,
        [&parameters, luma_weights, &color_transform](Vector3 input) {
            const double luma = input[0] * luma_weights[0]
                + input[1] * luma_weights[1] + input[2] * luma_weights[2];
            if (parameters.dehaze > 0.0) {
                const double veil = std::max(0.0, std::min({input[0], input[1], input[2]}));
                const double maximum = std::max({0.0, input[0], input[1], input[2]});
                const double veil_fraction = std::clamp(veil / (maximum + 0.18), 0.0, 1.0);
                const double transmission = std::max(
                    0.2, 1.0 - 0.88 * parameters.dehaze * veil_fraction
                );
                for (double& channel : input) {
                    channel = (channel - parameters.dehaze * 0.65 * veil) / transmission;
                }
            } else if (parameters.dehaze < 0.0) {
                const double amount = -parameters.dehaze;
                const double atmosphere = std::max(0.18, luma + 0.28);
                for (double& channel : input) {
                    channel = std::lerp(channel, atmosphere, 0.55 * amount);
                }
            }

            Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
            const double chroma = std::hypot(lab[1], lab[2]);
            if ((parameters.defringe_purple_amount > 0.0
                    || parameters.defringe_green_amount > 0.0)
                && chroma > 1.0e-8) {
                const double hue = wrap_degrees(std::atan2(lab[2], lab[1]) * 180.0 / pi);
                constexpr double range_feather_degrees = 10.0;
                const auto range_weight = [hue](const double low, const double high) {
                    return smoothstep(low - range_feather_degrees, low, hue)
                        * (1.0 - smoothstep(high, high + range_feather_degrees, hue));
                };
                const double purple = parameters.defringe_purple_amount * range_weight(
                    parameters.defringe_purple_hue_low,
                    parameters.defringe_purple_hue_high
                );
                const double green = parameters.defringe_green_amount * range_weight(
                    parameters.defringe_green_hue_low,
                    parameters.defringe_green_hue_high
                );
                const double reduction = std::max(purple, green);
                lab[1] *= 1.0 - 0.9 * reduction;
                lab[2] *= 1.0 - 0.9 * reduction;
            }

            return multiply(color_transform.xyz_to_rgb, oklab_to_xyz(lab));
        }
    );
}

void apply_color_grading(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters
) {
    const bool grading = parameters.shadows_saturation != 0.0
        || parameters.shadows_luminance != 0.0
        || parameters.midtones_saturation != 0.0
        || parameters.midtones_luminance != 0.0
        || parameters.highlights_saturation != 0.0
        || parameters.highlights_luminance != 0.0;
    if (!grading) {
        return;
    }
    const auto luma_weights = image.working_space.luminance_coefficients;
    const WorkingSpaceTransform color_transform = prepare_working_space_transform(
        image.working_space, node, node_index
    );
    transform_rgb_pixels(
        image,
        node_index,
        node,
        [&parameters, luma_weights, &color_transform](const Vector3& input) {
            const double luma = input[0] * luma_weights[0]
                + input[1] * luma_weights[1] + input[2] * luma_weights[2];
            Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
            const double normalized = std::max(0.0, luma) / (std::max(0.0, luma) + 0.18);
            const double center = std::clamp(
                0.5 + 0.22 * parameters.grading_balance, 0.18, 0.82
            );
            const double width = 0.08 + 0.30 * parameters.grading_blending;
            double shadow_weight = 1.0 - smoothstep(center - width, center + width, normalized);
            double highlight_weight = smoothstep(center - width, center + width, normalized);
            double midtone_weight = 1.0 - std::abs(normalized - center)
                / std::max(0.12, 0.5 + width);
            midtone_weight = std::clamp(midtone_weight, 0.0, 1.0);
            const double total = shadow_weight + midtone_weight + highlight_weight;
            shadow_weight /= total;
            midtone_weight /= total;
            highlight_weight /= total;
            const auto wheel = [&lab](
                const double hue,
                const double saturation,
                const double luminance,
                const double weight
            ) {
                const double angle = hue * pi / 180.0;
                lab[0] += 0.12 * luminance * weight;
                lab[1] += 0.09 * saturation * weight * std::cos(angle);
                lab[2] += 0.09 * saturation * weight * std::sin(angle);
            };
            wheel(parameters.shadows_hue, parameters.shadows_saturation,
                  parameters.shadows_luminance, shadow_weight);
            wheel(parameters.midtones_hue, parameters.midtones_saturation,
                  parameters.midtones_luminance, midtone_weight);
            wheel(parameters.highlights_hue, parameters.highlights_saturation,
                  parameters.highlights_luminance, highlight_weight);
            return multiply(color_transform.xyz_to_rgb, oklab_to_xyz(lab));
        }
    );
}

[[nodiscard]] double coordinate_noise(std::uint32_t x, std::uint32_t y, std::uint32_t seed) noexcept {
    std::uint32_t value = x * 0x9e3779b9U ^ y * 0x85ebca6bU ^ seed;
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return static_cast<double>(value) / static_cast<double>(std::numeric_limits<std::uint32_t>::max())
        * 2.0 - 1.0;
}

void apply_grain_and_vignette(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters,
    const AdjustmentExecutionContext& context
) {
    if (parameters.grain_amount == 0.0 && parameters.vignette_amount == 0.0) {
        return;
    }
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    const auto weights = image.working_space.luminance_coefficients;
    const double full_width = context.full_dimensions.width;
    const double full_height = context.full_dimensions.height;
    const std::uint32_t grain_block = 1U + static_cast<std::uint32_t>(
        std::round(parameters.grain_size * 3.0)
    );
    for (std::uint32_t y = 0; y < image.dimensions.height; ++y) {
        for (std::uint32_t x = 0; x < image.dimensions.width; ++x) {
            const std::size_t sample = static_cast<std::size_t>(y) * stride
                + static_cast<std::size_t>(x) * rgb_channels;
            const std::uint32_t global_x = context.origin_x + x;
            const std::uint32_t global_y = context.origin_y + y;
            const double luma = image.samples[sample] * weights[0]
                + image.samples[sample + 1U] * weights[1]
                + image.samples[sample + 2U] * weights[2];
            double gain = 1.0;
            double additive = 0.0;
            if (parameters.grain_amount > 0.0) {
                const double coarse = coordinate_noise(
                    global_x / grain_block, global_y / grain_block, 0x51ed270bU
                );
                const double fine = coordinate_noise(global_x, global_y, 0xa54ff53aU);
                const double noise = std::lerp(coarse, fine, parameters.grain_roughness);
                const double visibility = 0.45 + 0.55 * (1.0 - smoothstep(0.0, 1.0, luma));
                additive = noise * parameters.grain_amount
                    * (0.012 + 0.035 * parameters.grain_roughness) * visibility;
            }
            if (parameters.vignette_amount != 0.0) {
                double nx = (static_cast<double>(global_x) + 0.5) / full_width * 2.0 - 1.0;
                double ny = (static_cast<double>(global_y) + 0.5) / full_height * 2.0 - 1.0;
                nx *= full_width / std::max(full_width, full_height);
                ny *= full_height / std::max(full_width, full_height);
                const double circle = std::hypot(nx, ny);
                const double square = std::max(std::abs(nx), std::abs(ny));
                const double round_mix = 0.5 * (parameters.vignette_roundness + 1.0);
                const double radius = std::lerp(square, circle, round_mix);
                const double start = 0.15 + 0.65 * parameters.vignette_midpoint;
                const double feather = 0.04 + 0.50 * parameters.vignette_feather;
                const double mask = smoothstep(start, start + feather, radius);
                double stops = 2.0 * parameters.vignette_amount * mask;
                if (stops < 0.0) {
                    const double highlight = smoothstep(0.6, 1.6, luma);
                    stops *= 1.0 - parameters.vignette_highlights * highlight;
                }
                gain = std::exp2(stops);
            }
            for (std::size_t channel = 0; channel < rgb_channels; ++channel) {
                image.samples[sample + channel] = checked_float(
                    static_cast<double>(image.samples[sample + channel]) * gain + additive,
                    node_index,
                    node
                );
            }
        }
    }
}

void apply_node(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t index,
    const PreparedCurveAdjustment& prepared_curve,
    const AdjustmentExecutionContext& context
) {
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
                const auto luminance_weights = image.working_space.luminance_coefficients;
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&parameters, luminance_weights](const Vector3& input) {
                        return apply_scene_contrast(input, luminance_weights, parameters);
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, ToneCurve>) {
                apply_prepared_tone_curve(
                    image,
                    std::get<PreparedToneCurve>(prepared_curve),
                    [&node, index](const double value) {
                        return checked_float(value, index, node);
                    }
                );
            } else if constexpr (std::is_same_v<Parameters, SmoothRgbToneCurve>) {
                apply_prepared_smooth_rgb_tone_curve(
                    image,
                    std::get<PreparedSmoothRgbToneCurve>(prepared_curve),
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
                if (perceptual_color_is_neutral(parameters)) {
                    return;
                }
                const WorkingSpaceTransform color_transform =
                    prepare_working_space_transform(image.working_space, node, index);
                transform_rgb_pixels(
                    image,
                    index,
                    node,
                    [&parameters, &color_transform](const Vector3& input) {
                        Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
                        const double chroma = std::hypot(lab[1], lab[2]);
                        const double relative_chroma = chroma
                            / std::max(1.0e-6, std::abs(lab[0]));
                        if (relative_chroma <= perceptual_low_chroma_ratio_epsilon) {
                            return input;
                        }

                        const double source_hue = wrap_degrees(
                            std::atan2(lab[2], lab[1]) * 180.0 / pi
                        );
                        const auto band_weights = hue_band_weights(source_hue);
                        // Hue is numerically unstable near the neutral axis. Fade all hue-keyed
                        // controls there while leaving vibrance free to increase a real, muted
                        // chroma. The ratio keeps this behavior invariant under scene exposure.
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
                        if (!changed) {
                            return input;
                        }
                        return multiply(color_transform.xyz_to_rgb, oklab_to_xyz(lab));
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
                    // Color wheels are a creative transform, independent of
                    // technical recovery and still before a selected LUT.
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
            } else if constexpr (std::is_same_v<Parameters, ToneCurve>) {
                return AdjustmentOperation::tone_curve;
            } else if constexpr (std::is_same_v<Parameters, SmoothRgbToneCurve>) {
                return AdjustmentOperation::smooth_rgb_tone_curve;
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
    case AdjustmentOperation::tone_curve:
    case AdjustmentOperation::smooth_rgb_tone_curve:
        return "shadow.tone_curve";
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
    case AdjustmentOperation::tone_curve:
    case AdjustmentOperation::smooth_rgb_tone_curve:
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
                    if (value.execution_pass == DetailEffectsExecutionPass::color_grading
                        || value.execution_pass == DetailEffectsExecutionPass::finishing_effects) {
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

FloatRgbImage execute_adjustment_nodes(
    const FloatRgbImage& input,
    const std::span<const AdjustmentNode> nodes,
    AdjustmentExecutionContext context
) {
    validate_image(input);
    const auto prepared_curves = prepare_adjustment_nodes(nodes);

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
    FloatRgbImage output = input;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        if (nodes[index].enabled) {
            apply_node(output, nodes[index], index, prepared_curves[index], context);
        }
    }
    return output;
}

FloatRgbImage apply_tone_curve(const FloatRgbImage& input, const ToneCurve& curve) {
    validate_image(input);
    const PreparedToneCurve prepared = prepare_tone_curve(curve);

    FloatRgbImage output = input;
    apply_prepared_tone_curve(output, prepared, checked_tone_curve_float);
    return output;
}

FloatRgbImage apply_smooth_rgb_tone_curve(
    const FloatRgbImage& input,
    const SmoothRgbToneCurve& curve
) {
    validate_image(input);
    const PreparedSmoothRgbToneCurve prepared = prepare_smooth_rgb_tone_curve(curve);

    FloatRgbImage output = input;
    apply_prepared_smooth_rgb_tone_curve(output, prepared, checked_tone_curve_float);
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
