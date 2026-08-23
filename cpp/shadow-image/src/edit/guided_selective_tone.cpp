#include "guided_selective_tone.hpp"

#include "adjustment_node_diagnostics.hpp"
#include "rgb_pixel_traversal.hpp"
#include "scalar_neighborhood_filters.hpp"
#include "working_color_math.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/sensor_clipping.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::image::detail {

namespace {

constexpr std::size_t rgb_channels = 3U;

[[nodiscard]] bool normalized_amount(const double value) noexcept {
    return std::isfinite(value) && value >= -1.0 && value <= 1.0;
}

} // namespace

PreparedGuidedSelectiveTone::PreparedGuidedSelectiveTone(
    const bool neutral,
    const double highlights,
    const double shadows,
    const double whites,
    const double blacks,
    const std::uint32_t mask_radius_x,
    const std::uint32_t mask_radius_y,
    const std::uint32_t support_radius_x,
    const std::uint32_t support_radius_y
) noexcept :
    neutral_(neutral), highlights_(highlights), shadows_(shadows), whites_(whites), blacks_(blacks),
    mask_radius_x_(mask_radius_x), mask_radius_y_(mask_radius_y),
    support_radius_x_(support_radius_x), support_radius_y_(support_radius_y) {}

AdjustmentFootprint PreparedGuidedSelectiveTone::footprint() const noexcept {
    return AdjustmentFootprint{
        .horizontal_radius = support_radius_x_,
        .vertical_radius = support_radius_y_,
    };
}

bool guided_selective_tone_is_neutral(const SelectiveToneAdjustment& parameters) noexcept {
    return parameters.highlights == 0.0 && parameters.shadows == 0.0 && parameters.whites == 0.0
           && parameters.blacks == 0.0;
}

void validate_guided_selective_tone(
    const SelectiveToneAdjustment& parameters,
    const AdjustmentNode& node,
    const std::size_t node_index
) {
    if (!normalized_amount(parameters.highlights) || !normalized_amount(parameters.shadows)
        || !normalized_amount(parameters.whites) || !normalized_amount(parameters.blacks)) {
        throw_node_error(
            EditErrorCode::invalid_parameter,
            node_index,
            node,
            "selective tone amounts must be finite and within [-1, 1]"
        );
    }
}

namespace {

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
[[nodiscard]] double
lower_ev_hinge(const double value, const double boundary, const double softness) noexcept {
    return softness * log2_one_plus_exp2((boundary - value) / softness);
}

[[nodiscard]] double
upper_ev_hinge(const double value, const double boundary, const double softness) noexcept {
    return softness * log2_one_plus_exp2((value - boundary) / softness);
}

// A compact C2 transition with exactly zero influence before the start and a finite plateau
// after the span. Unlike an unbounded upper hinge, its derivative returns to zero in
// super-white scene data, so an extreme recovery cannot collapse every brighter source value
// onto the same display shoulder.
[[nodiscard]] double
smootherstep_window(const double value, const double start, const double span) noexcept {
    const double normalized = std::clamp((value - start) / span, 0.0, 1.0);
    return normalized * normalized * normalized * (normalized * (normalized * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] double adjusted_selective_tone_ev(
    const double mask_ev,
    const PreparedGuidedSelectiveTone& prepared
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
    // The original endpoint fields were deliberately defensive, but they made Blacks/Whites
    // feel like a final one-percent trim on ordinary RAW detail.  A photographic control needs
    // to have a clear effect before the signal is at its absolute floor/ceiling, while still
    // rolling smoothly enough that it cannot create a contour.  The endpoint has a narrower
    // support than the recovery fields below; it contributes stronger toe/shoulder placement
    // without replacing Shadows/Highlights as the broad recovery tools.
    constexpr double endpoint_strength = 0.86;
    constexpr double endpoint_boundary_ev = 1.45;
    constexpr double endpoint_softness_ev = 0.55;
    // Keep every stage's derivative strictly positive at its extreme (1-strength), so even a
    // fully combined Blacks + Shadows or Highlights + Whites edit remains monotonic.  The
    // slightly wider range gives the recovery controls enough practical latitude for a RAW
    // file without asking exposure or a curve to do all of the work.
    constexpr double recovery_strength = 0.76;
    constexpr double shadow_boundary_ev = -0.15;
    constexpr double highlight_boundary_ev = 0.75;
    constexpr double recovery_softness_ev = 0.95;

    // Never sum several hinge derivatives from the same source EV.  Although each field is
    // monotonic by itself, an additive combination can fold when Black and Shadow (or Highlight
    // and White) are both at an extreme.  Sequential composition keeps every stage strictly
    // positive-slope because each field strength is below one.
    const auto apply_lower = [](const double source_ev,
                                const double amount,
                                const double boundary,
                                const double softness,
                                const double strength) {
        return source_ev + strength * amount * lower_ev_hinge(source_ev, boundary, softness);
    };
    const auto apply_upper = [](const double source_ev,
                                const double amount,
                                const double boundary,
                                const double softness,
                                const double strength) {
        return source_ev + strength * amount * upper_ev_hinge(source_ev, boundary, softness);
    };

    double adjusted_ev = mask_ev;
    adjusted_ev = apply_lower(
        adjusted_ev,
        prepared.blacks(),
        -endpoint_boundary_ev,
        endpoint_softness_ev,
        endpoint_strength
    );
    adjusted_ev = apply_lower(
        adjusted_ev,
        prepared.shadows(),
        shadow_boundary_ev,
        recovery_softness_ev,
        recovery_strength
    );
    // Positive Highlight/White edits retain the established open-ended fields. Negative
    // recovery is different: composing two open-ended hinges leaves only 3.36% of the source
    // slope when both controls are at -100, which darkens ordinary highlights and packs all
    // super-white values into a narrow band. Couple the negative controls into two overlapping
    // finite shoulders instead. Highlights starts at middle gray and reaches a 0.75-stop budget
    // over 3.4 EV; Whites starts at +0.3 EV and contributes another 0.85 stop over 5.5 EV. The
    // reduced capacities retain a bright highlight core instead of making an extreme recovery
    // look like global underexposure. The quintic windows are C2 at both ends, have exactly zero
    // midtone tail, and their combined derivative is at most 0.601 at full strength. The mapping
    // therefore remains monotonic with at least 39.9% local slope before returning to 1:1 in the
    // super-whites.
    adjusted_ev = apply_upper(
        adjusted_ev,
        std::max(0.0, prepared.highlights()),
        highlight_boundary_ev,
        recovery_softness_ev,
        recovery_strength
    );
    adjusted_ev = apply_upper(
        adjusted_ev,
        std::max(0.0, prepared.whites()),
        endpoint_boundary_ev,
        endpoint_softness_ev,
        endpoint_strength
    );
    constexpr double negative_highlight_capacity_ev = 0.75;
    constexpr double negative_highlight_start_ev = 0.0;
    constexpr double negative_highlight_span_ev = 3.4;
    constexpr double negative_white_capacity_ev = 0.85;
    constexpr double negative_white_start_ev = 0.3;
    constexpr double negative_white_span_ev = 5.5;
    const double negative_highlights = std::max(0.0, -prepared.highlights());
    const double negative_whites = std::max(0.0, -prepared.whites());
    const double shoulder_source_ev = adjusted_ev;
    adjusted_ev -= negative_highlights * negative_highlight_capacity_ev
                   * smootherstep_window(
                       shoulder_source_ev,
                       negative_highlight_start_ev,
                       negative_highlight_span_ev
                   );
    adjusted_ev -=
        negative_whites * negative_white_capacity_ev
        * smootherstep_window(shoulder_source_ev, negative_white_start_ev, negative_white_span_ev);

    return adjusted_ev;
}

[[nodiscard]] Vector3 apply_selective_tone_at_mask(
    const Vector3& input,
    const WorkingSpaceTransform& color_transform,
    const PreparedGuidedSelectiveTone& prepared,
    const double mask_ev,
    const double highlight_chroma_risk
) noexcept {
    Vector3 lab = working_rgb_to_oklab(color_transform, input);
    if (!(lab[0] > 0.0) || !std::isfinite(lab[0])) {
        return input;
    }

    // The EV field is evaluated against the guided mask, not individual pixel luminance. Any
    // clipped-surface continuity has already been established by RAW source preparation; keeping
    // a separate core-luminance override here would reveal that source mask again under a strong
    // negative recovery.
    const double requested_adjusted_ev = adjusted_selective_tone_ev(mask_ev, prepared);
    const double recovered_ev = std::max(0.0, mask_ev - requested_adjusted_ev);
    constexpr double recovery_start_ev = 0.05;
    constexpr double recovery_width_ev = 0.50;
    const double recovery_normalized =
        std::clamp((recovered_ev - recovery_start_ev) / recovery_width_ev, 0.0, 1.0);
    const double recovery_pull =
        recovery_normalized * recovery_normalized * (3.0 - 2.0 * recovery_normalized);
    const double stops = requested_adjusted_ev - mask_ev;
    const double gain = std::exp2(stops);
    if (!(gain > 0.0) || !std::isfinite(gain)) {
        return input;
    }
    lab[0] *= std::cbrt(gain);

    // Oklab L is the right default for ordinary scene-linear grade work: it avoids changing
    // hue just because brightness changes. A RAW pixel with either calibrated sensor white or
    // a source-side two-channel headroom disagreement is different: once a strong negative
    // highlight/white edit pulls it back, preserved a/b can reveal false magenta or red. Fade
    // only that unmeasured chroma, only in proportion to recovered stops. This intentionally
    // leaves source-trustworthy pixels, positive edits, and the default rendition untouched.
    if (highlight_chroma_risk > 0.0) {
        // Begin the source-evidenced neutralisation with the first meaningful recovery rather
        // than leaving a mid-strength dead zone.  Otherwise a full slider pull can look neutral
        // while the same clipped highlight becomes falsely magenta halfway through the gesture.
        // The C1 smoothstep still preserves a continuous, non-destructive transition.
        const double chroma_pull =
            recovery_pull * std::sqrt(std::clamp(highlight_chroma_risk, 0.0, 1.0));
        lab[1] *= 1.0 - chroma_pull;
        lab[2] *= 1.0 - chroma_pull;
    }
    return oklab_to_working_rgb(color_transform, lab);
}

[[nodiscard]] std::uint32_t selective_tone_mask_radius(const double level_zero_to_raster_scale) {
    const double scaled = selective_tone_guided_mask_radius_level_zero * level_zero_to_raster_scale;
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
    if (static_cast<std::size_t>(radius) > (std::numeric_limits<std::size_t>::max() - 1U) / 2U) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter box window exceeds the address space"
        );
    }
    return static_cast<std::size_t>(radius) * 2U + 1U;
}

[[nodiscard]] std::uint32_t
selective_tone_guided_filter_support_radius(const std::uint32_t local_radius) {
    if (local_radius
        > std::numeric_limits<std::uint32_t>::max() / selective_tone_guided_filter_box_passes) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter support exceeds the supported integer range"
        );
    }
    return local_radius * selective_tone_guided_filter_box_passes;
}

} // namespace

PreparedGuidedSelectiveTone prepare_guided_selective_tone(
    const SelectiveToneAdjustment& parameters,
    const double level_zero_to_raster_scale_x,
    const double level_zero_to_raster_scale_y
) {
    if (!std::isfinite(level_zero_to_raster_scale_x) || level_zero_to_raster_scale_x <= 0.0
        || !std::isfinite(level_zero_to_raster_scale_y) || level_zero_to_raster_scale_y <= 0.0) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "guided selective tone raster scales must be finite and positive"
        );
    }
    if (!normalized_amount(parameters.highlights) || !normalized_amount(parameters.shadows)
        || !normalized_amount(parameters.whites) || !normalized_amount(parameters.blacks)) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "cannot calculate a footprint for malformed selective tone parameters"
        );
    }

    const bool neutral = guided_selective_tone_is_neutral(parameters);
    if (neutral) {
        return PreparedGuidedSelectiveTone{
            true,
            parameters.highlights,
            parameters.shadows,
            parameters.whites,
            parameters.blacks,
            0U,
            0U,
            0U,
            0U,
        };
    }

    const std::uint32_t mask_radius_x = selective_tone_mask_radius(level_zero_to_raster_scale_x);
    const std::uint32_t mask_radius_y = selective_tone_mask_radius(level_zero_to_raster_scale_y);
    return PreparedGuidedSelectiveTone{
        false,
        parameters.highlights,
        parameters.shadows,
        parameters.whites,
        parameters.blacks,
        mask_radius_x,
        mask_radius_y,
        selective_tone_guided_filter_support_radius(mask_radius_x),
        selective_tone_guided_filter_support_radius(mask_radius_y),
    };
}

namespace {

struct SelectiveToneGuidedCoefficients final {
    std::vector<float> a;
    std::vector<float> b;
};

[[nodiscard]] float
checked_guided_filter_coefficient(const double value, const std::string_view label) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maximum || value > maximum) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "selective tone guided-filter " + std::string(label) + " exceeded finite float32 range"
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
    const std::array<double, 3>& luminance_weights,
    const PreparedGuidedSelectiveTone& prepared
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
    if (pixels > std::vector<float>{}.max_size()
        || pixels > std::numeric_limits<std::size_t>::max() / (2U * sizeof(float))) {
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
    const std::uint32_t radius_x = prepared.mask_radius_x();
    const std::uint32_t radius_y = prepared.mask_radius_y();
    const std::size_t window_width = selective_tone_box_window_length(radius_x);
    const std::size_t window_height = selective_tone_box_window_length(radius_y);
    const auto log_luminance_at = [&image, luminance_weights, stride, minimum_positive_luminance](
                                      const std::size_t x,
                                      const std::size_t y
                                  ) {
        const std::size_t sample = y * stride + x * rgb_channels;
        const double luminance =
            static_cast<double>(image.samples[sample]) * luminance_weights[0]
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

    if (width > std::vector<double>{}.max_size()
        || width > std::numeric_limits<std::size_t>::max() / (4U * sizeof(double))) {
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
    const auto accumulate_row =
        [&make_horizontal_row, &row_mean, &row_mean_square, &vertical_sum, &vertical_sum_square](
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
            const double mean_square = vertical_sum_square[x] / static_cast<double>(window_height);
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

} // namespace

void apply_prepared_guided_selective_tone_cpu(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const PreparedGuidedSelectiveTone& prepared,
    const AdjustmentExecutionContext& context
) {
    if (prepared.neutral()) {
        return;
    }
    const auto valid_context_map = [&context](const auto* map) {
        return map == nullptr || (map->valid() && map->dimensions == context.full_dimensions);
    };
    if (!valid_context_map(context.sensor_clipping_mask)
        || !valid_context_map(context.highlight_chroma_risk_map)
        || static_cast<std::uint64_t>(context.origin_x) + image.dimensions.width
               > context.full_dimensions.width
        || static_cast<std::uint64_t>(context.origin_y) + image.dimensions.height
               > context.full_dimensions.height) {
        throw_node_error(
            EditErrorCode::invalid_parameter,
            node_index,
            node,
            "selective tone source evidence does not match the rendered region"
        );
    }
    const auto luminance_weights = image.working_space.luminance_coefficients;
    const WorkingSpaceTransform color_transform =
        prepare_working_space_transform(image.working_space, node, node_index);
    const auto coefficients =
        selective_tone_guided_coefficients(image, luminance_weights, prepared);
    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    const std::uint32_t radius_x = prepared.mask_radius_x();
    const std::uint32_t radius_y = prepared.mask_radius_y();
    const std::size_t window_width = selective_tone_box_window_length(radius_x);
    const std::size_t window_height = selective_tone_box_window_length(radius_y);
    if (width > std::vector<double>{}.max_size()
        || width > std::numeric_limits<std::size_t>::max() / (4U * sizeof(double))) {
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
    const auto accumulate_row =
        [&make_horizontal_row, &row_mean_a, &row_mean_b, &vertical_sum_a, &vertical_sum_b](
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
                                            + input[1] * luminance_weights[1]
                                            + input[2] * luminance_weights[2];
            const double source_ev =
                std::log2(std::max(source_luminance, minimum_positive_luminance) / 0.18);
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
            const std::uint64_t full_x = static_cast<std::uint64_t>(context.origin_x) + x;
            const std::uint64_t full_y = static_cast<std::uint64_t>(context.origin_y) + y;
            const auto full_index =
                static_cast<std::size_t>(full_y * context.full_dimensions.width + full_x);
            const bool source_surface_reconstructed =
                context.highlight_chroma_risk_map != nullptr
                && context.highlight_chroma_risk_map->source_surface_reconstructed;
            const bool physically_highlight_clipped =
                !source_surface_reconstructed && context.sensor_clipping_mask != nullptr
                && (context.sensor_clipping_mask->samples[full_index] & sensor_highlight_clipped)
                       != 0U;
            const double continuous_highlight_risk =
                context.highlight_chroma_risk_map != nullptr
                    ? static_cast<double>(context.highlight_chroma_risk_map->samples[full_index])
                          / 255.0
                    : 0.0;
            const Vector3 output = apply_selective_tone_at_mask(
                input,
                color_transform,
                prepared,
                mask_ev,
                physically_highlight_clipped ? 1.0 : continuous_highlight_risk
            );
            image.samples[sample] = checked_edit_pixel_float(output[0], node_index, node);
            image.samples[sample + 1U] = checked_edit_pixel_float(output[1], node_index, node);
            image.samples[sample + 2U] = checked_edit_pixel_float(output[2], node_index, node);
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

} // namespace shadow::image::detail
