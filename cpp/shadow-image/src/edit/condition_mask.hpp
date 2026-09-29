#pragma once

#include "perceptual_hue_selection.hpp"
#include "working_color_math.hpp"
#include <shadow/image/adjustment_layers.hpp>
#include <shadow/image/edit_error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>

namespace shadow::image::detail {

inline void validate_condition_program(const std::span<const std::array<double, 8U>> program) {
    const auto invalid = [] {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "condition mask has an invalid bounded postfix program"
        );
    };
    if (program.empty() || program.size() > 32U)
        invalid();
    std::size_t depth = 0U;
    std::size_t leaves = 0U;
    for (const auto& p : program) {
        for (const double value : p)
            if (!std::isfinite(value))
                invalid();
        const auto unit = [](const double v) { return v >= 0.0 && v <= 1.0; };
        if (p[0] == 0.0 || p[0] == 2.0) {
            if (!unit(p[1]) || !unit(p[2]) || !unit(p[3]) || p[1] > p[2])
                invalid();
            ++depth;
            ++leaves;
        } else if (p[0] == 1.0) {
            if (p[1] < 0.0 || p[1] >= 360.0 || p[2] < 1.0 || p[2] > 180.0 || !unit(p[3])
                || !unit(p[4]) || !unit(p[5]) || (p[4] == 0.0 && p[5] != 0.0))
                invalid();
            ++depth;
            ++leaves;
        } else if (p[0] == 4.0 || p[0] == 5.0) {
            if (depth < 2U)
                invalid();
            --depth;
        } else if (p[0] == 6.0) {
            if (depth < 1U)
                invalid();
        } else
            invalid();
    }
    if (depth != 1U || leaves > 8U)
        invalid();
}

inline double condition_smoother(const double x) noexcept {
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

inline double
condition_range(double value, const double low, const double high, const double feather) noexcept {
    value = std::clamp(value, 0.0, 1.0);
    if (feather == 0.0)
        return value >= low && value <= high ? 1.0 : 0.0;
    return std::min(
        condition_smoother((value - low + feather) / feather),
        1.0 - condition_smoother((value - high) / feather)
    );
}

inline double condition_coverage(
    const std::span<const std::array<double, 8U>> program,
    const Vector3& lab
) noexcept {
    std::array<double, 8U> stack{};
    std::size_t depth = 0U;
    const double chroma = std::clamp(std::hypot(lab[1], lab[2]) / 0.4, 0.0, 1.0);
    for (const auto& p : program) {
        if (p[0] == 0.0 || p[0] == 2.0) {
            stack[depth++] = condition_range(p[0] == 0.0 ? lab[0] : chroma, p[1], p[2], p[3]);
        } else if (p[0] == 1.0) {
            const PerceptualHueSample hue = sample_oklab_hue(lab);
            double gate = 1.0;
            if (p[4] != 0.0) {
                if (p[5] == 0.0)
                    gate = chroma >= p[4] ? 1.0 : 0.0;
                else {
                    const double lower = std::max(0.0, p[4] - p[5]);
                    const double t = std::clamp((chroma - lower) / (p[4] - lower), 0.0, 1.0);
                    gate = t * t * (3.0 - 2.0 * t);
                }
            }
            stack[depth++] =
                hue.confidence * perceptual_hue_range_weight(hue.degrees, p[1], p[2], p[3]) * gate;
        } else if (p[0] == 6.0)
            stack[depth - 1U] = 1.0 - stack[depth - 1U];
        else {
            const double right = stack[--depth];
            stack[depth - 1U] = p[0] == 4.0 ? std::min(stack[depth - 1U], right)
                                            : std::max(stack[depth - 1U], right);
        }
    }
    return std::clamp(stack[0], 0.0, 1.0);
}

} // namespace shadow::image::detail
