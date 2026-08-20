#include "raw_highlight_reconstruction.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace shadow::image::detail {

namespace {

[[nodiscard]] float smoothstep(const float edge0, const float edge1, const float value) noexcept {
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float highlight_white_ceiling_strength(const RawCfaFootprint& cfa_risk) noexcept {
    float exhausted_fraction = 0.0F;
    for (const float risk : cfa_risk) {
        exhausted_fraction += std::clamp(risk, 0.0F, 1.0F);
    }
    exhausted_fraction /= static_cast<float>(cfa_risk.size());

    // One colour can be a real coloured light. The first second-site evidence turns on a
    // continuous shoulder; a complete two-site plateau reaches ordinary H=0-style clipping.
    return smoothstep(0.25F, 0.50F, exhausted_fraction);
}

[[nodiscard]] std::array<float, 3U>
scene_neutral_camera_direction(const RawFrameLinearTransform& transform) noexcept {
    const auto& m = transform.camera_to_linear_srgb_d65;
    const double a = m[0U];
    const double b = m[1U];
    const double c = m[2U];
    const double d = m[3U];
    const double e = m[4U];
    const double f = m[5U];
    const double g = m[6U];
    const double h = m[7U];
    const double i = m[8U];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12) {
        return {};
    }
    const std::array<float, 3U> direction{
        static_cast<float>(((e * i - f * h) + (c * h - b * i) + (b * f - c * e)) / determinant),
        static_cast<float>(((f * g - d * i) + (a * i - c * g) + (c * d - a * f)) / determinant),
        static_cast<float>(((d * h - e * g) + (b * g - a * h) + (a * e - b * d)) / determinant),
    };
    for (const float component : direction) {
        if (!std::isfinite(component) || component <= 0.0F) {
            return {};
        }
    }
    return direction;
}

[[nodiscard]] float scene_linear_energy(
    const CameraRgbSample& camera,
    const RawFrameLinearTransform& transform
) noexcept {
    float total = 0.0F;
    for (std::size_t output = 0U; output < 3U; ++output) {
        for (std::size_t input = 0U; input < 3U; ++input) {
            total += static_cast<float>(transform.camera_to_linear_srgb_d65[output * 3U + input])
                     * camera.values[input];
        }
    }
    return std::max(0.0F, total / 3.0F);
}

} // namespace

CameraRgbSample reconstruct_cfa_highlights(
    CameraRgbSample camera,
    const RawFrameLinearTransform& transform,
    const RawHighlightRecoveryIntent recovery
) noexcept {
    if (recovery != RawHighlightRecoveryIntent::provider_default) {
        return camera;
    }
    const float white_ceiling_strength =
        highlight_white_ceiling_strength(camera.cfa_highlight_risk);
    if (white_ceiling_strength <= 0.0F) {
        return camera;
    }

    // cfa_highlight_risk is formed from physical black/white-normalized headroom before white
    // balance and before preview downsampling; gains cannot create sensor evidence. First use
    // ordinary H=0-style component clipping. This does not estimate a missing channel or rebuild
    // spatial detail. A strictly bounded residual neutral pull then prevents a two-green plateau
    // from retaining false magenta/green after the camera matrix; it is deliberately much weaker
    // than the earlier reconstruction policy and cannot affect a single-site coloured light.
    const float peak_before_ceiling =
        std::max({camera.values[0U], camera.values[1U], camera.values[2U]});
    if (peak_before_ceiling <= 1.0F) {
        return camera;
    }
    for (float& component : camera.values) {
        component += (std::min(component, 1.0F) - component) * white_ceiling_strength;
    }
    const auto neutral_direction = scene_neutral_camera_direction(transform);
    if (neutral_direction == std::array<float, 3U>{}) {
        return camera;
    }
    constexpr float maximum_residual_chroma_suppression = 0.40F;
    const float residual_chroma_suppression =
        maximum_residual_chroma_suppression * white_ceiling_strength;
    const float energy = scene_linear_energy(camera, transform);
    for (std::size_t channel = 0U; channel < camera.values.size(); ++channel) {
        const float neutral_target = neutral_direction[channel] * energy;
        camera.values[channel] +=
            (neutral_target - camera.values[channel]) * residual_chroma_suppression;
    }
    return camera;
}

} // namespace shadow::image::detail
