#include "contract_test_assertions.hpp"
#include "fused_raw_contract_test_support.hpp"

#include <shadow/image/fused_raw_development.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

[[nodiscard]] bool environment_enabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
}

[[nodiscard]] image::RawFrame sensor_clipped_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            // All four CFA sites are at calibrated sensor white. The editable source may limit
            // this true plateau, unlike a sub-white sample amplified above one by RAW white
            // balance.
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                static_cast<std::uint16_t>(frame.descriptor.white_levels[site]);
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame single_channel_clipped_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                static_cast<std::uint16_t>(
                    colour == image::RawCfaColor::red ? frame.descriptor.white_levels[site] : 700U
                );
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame canon_two_green_sites_clipped_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            std::uint16_t sample = 700U;
            if (colour == image::RawCfaColor::green) {
                sample = static_cast<std::uint16_t>(frame.descriptor.white_levels[site]);
            } else if (colour == image::RawCfaColor::blue) {
                sample = 940U;
            }
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                sample;
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame multi_channel_clipped_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                static_cast<std::uint16_t>(
                    colour == image::RawCfaColor::red || colour == image::RawCfaColor::blue
                        ? frame.descriptor.white_levels[site]
                        : 700U
                );
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame two_channel_shoulder_frame(const std::uint16_t signal) {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                colour == image::RawCfaColor::red || colour == image::RawCfaColor::blue ? signal
                                                                                        : 700U;
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame near_white_but_unclipped_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                static_cast<std::uint16_t>(frame.descriptor.white_levels[site] - 1U);
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame two_channel_linear_response_boundary_frame() {
    auto frame = two_channel_shoulder_frame(850U);
    frame.descriptor.linear_response_limits = {850U, 860U, 870U, 880U};
    frame.descriptor.has_linear_response_limits = true;
    return frame;
}

[[nodiscard]] image::RawFrame saturated_red_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                static_cast<std::uint16_t>(
                    colour == image::RawCfaColor::red ? frame.descriptor.white_levels[site] : 100U
                );
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame mixed_highlight_boundary_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            const bool clipped_side = x >= frame.descriptor.storage_dimensions.width / 2U;
            const std::uint16_t sample =
                colour == image::RawCfaColor::red
                    ? static_cast<std::uint16_t>(
                          clipped_side ? frame.descriptor.white_levels[site]
                                       : frame.descriptor.white_levels[site] - 8U
                      )
                    : static_cast<std::uint16_t>(frame.descriptor.white_levels[site] - 80U);
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                sample;
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame slanted_shared_clipping_boundary_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const bool clipped_side = 2U * x + y >= 9U;
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                static_cast<std::uint16_t>(
                    clipped_side ? frame.descriptor.white_levels[site]
                                 : frame.descriptor.black_levels[site] + 180U
                );
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame nikon_near_limit_green_boundary_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            const bool bright_side = 2U * x + y >= 9U;
            std::uint16_t sample =
                static_cast<std::uint16_t>(frame.descriptor.black_levels[site] + 180U);
            if (bright_side) {
                if (colour == image::RawCfaColor::red) {
                    sample = static_cast<std::uint16_t>(frame.descriptor.black_levels[site] + 890U);
                } else if (colour == image::RawCfaColor::green) {
                    sample = static_cast<std::uint16_t>(frame.descriptor.white_levels[site] - 2U);
                } else {
                    sample = static_cast<std::uint16_t>(frame.descriptor.black_levels[site] + 690U);
                }
            }
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                sample;
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame nikon_near_limit_green_flat_top_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            std::uint16_t sample = 0U;
            if (colour == image::RawCfaColor::red) {
                sample = static_cast<std::uint16_t>(frame.descriptor.black_levels[site] + 890U);
            } else if (colour == image::RawCfaColor::green) {
                sample = static_cast<std::uint16_t>(frame.descriptor.white_levels[site] - 2U);
            } else {
                sample = static_cast<std::uint16_t>(frame.descriptor.black_levels[site] + 690U);
            }
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                sample;
        }
    }
    return frame;
}

void cfa_white_balance_retains_editable_headroom() {
    const image::RawFrameLinearTransform transform{
        .camera_to_linear_srgb_d65 =
            {
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
            },
        .camera_rgb_to_linear_srgb_d65 =
            {
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
            },
        .camera_neutral = {1.0, 1.0, 1.0},
        .cfa_white_balance = {2.0, 1.0, 1.0, 1.5},
        .apply_cfa_white_balance = true,
    };
    expect(transform.valid(), "the boosted-gain headroom fixture has a valid camera transform");
    if (!transform.valid()) {
        return;
    }
    const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        sensor_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        sensor_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    expect(
        image::raw_highlight_treatment_identity(cpu.highlight_recovery)
            == "sensor-highlights=cfa-opposed-linear-limit@20260823.4;"
               "recovery=one-sided+shared-chroma;headroom=sub-white-fp32;"
               "clipped-highlight-surface=low-frequency-push-pull-luminance-protected-v2",
        "the default source receipt identifies continuous clipped-highlight reconstruction"
    );

    float maximum_default_delta = 0.0F;
    float maximum_default_chroma = 0.0F;
    float maximum_disabled_chroma = 0.0F;
    for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); index += 3U) {
        maximum_default_chroma = std::max(
            maximum_default_chroma,
            std::max({
                cpu.scene_linear.samples[index],
                cpu.scene_linear.samples[index + 1U],
                cpu.scene_linear.samples[index + 2U],
            })
                - std::min({
                    cpu.scene_linear.samples[index],
                    cpu.scene_linear.samples[index + 1U],
                    cpu.scene_linear.samples[index + 2U],
                })
        );
        maximum_disabled_chroma = std::max(
            maximum_disabled_chroma,
            std::max({
                disabled.scene_linear.samples[index],
                disabled.scene_linear.samples[index + 1U],
                disabled.scene_linear.samples[index + 2U],
            })
                - std::min({
                    disabled.scene_linear.samples[index],
                    disabled.scene_linear.samples[index + 1U],
                    disabled.scene_linear.samples[index + 2U],
                })
        );
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            maximum_default_delta = std::max(
                maximum_default_delta,
                std::abs(
                    cpu.scene_linear.samples[index + channel]
                    - disabled.scene_linear.samples[index + channel]
                )
            );
        }
    }
    expect(
        maximum_disabled_chroma > 0.4F,
        "disabled mode retains the measured CFA-white-balance channel separation"
    );
    expect(
        maximum_default_delta > 0.4F && maximum_default_chroma <= 1.0e-6F,
        "physical sensor-white CFA samples reach the white ceiling without a colour rebuild"
    );

    const auto near_white = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        near_white_but_unclipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto near_white_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        near_white_but_unclipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    float maximum_near_white_value = 0.0F;
    bool near_white_was_never_lowered = true;
    for (std::size_t index = 0U; index < near_white.scene_linear.samples.size(); ++index) {
        near_white_was_never_lowered =
            near_white_was_never_lowered
            && near_white.scene_linear.samples[index]
                   >= near_white_disabled.scene_linear.samples[index] - 1.0e-6F;
        maximum_near_white_value =
            std::max(maximum_near_white_value, near_white.scene_linear.samples[index]);
    }
    expect(
        near_white_was_never_lowered && maximum_near_white_value > 1.5F,
        "near-limit opposed repair never clips white-balance-induced fp32 headroom"
    );

    const auto response_limited = two_channel_linear_response_boundary_frame();
    const auto response_default = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        response_limited,
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto response_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        response_limited,
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    float response_default_chroma = 0.0F;
    float response_disabled_chroma = 0.0F;
    for (std::size_t index = 0U; index < response_default.scene_linear.samples.size();
         index += 3U) {
        const auto& default_samples = response_default.scene_linear.samples;
        const auto& disabled_samples = response_disabled.scene_linear.samples;
        response_default_chroma = std::max(
            response_default_chroma,
            std::max({
                default_samples[index],
                default_samples[index + 1U],
                default_samples[index + 2U],
            })
                - std::min({
                    default_samples[index],
                    default_samples[index + 1U],
                    default_samples[index + 2U],
                })
        );
        response_disabled_chroma = std::max(
            response_disabled_chroma,
            std::max({
                disabled_samples[index],
                disabled_samples[index + 1U],
                disabled_samples[index + 2U],
            })
                - std::min({
                    disabled_samples[index],
                    disabled_samples[index + 1U],
                    disabled_samples[index + 2U],
                })
        );
    }
    expect(
        response_default_chroma + 1.0e-4F < response_disabled_chroma,
        "a calibrated response boundary continuously lowers only untrustworthy two-channel "
        "highlight chroma"
    );

    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        return;
    }
    const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        sensor_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::metal
    );
    float maximum_metal_difference = 0.0F;
    for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); ++index) {
        maximum_metal_difference = std::max(
            maximum_metal_difference,
            std::abs(cpu.scene_linear.samples[index] - metal.scene_linear.samples[index])
        );
    }
    expect(
        maximum_metal_difference <= 4.0e-5F,
        "Metal matches the CPU physical-white opposed reconstruction path"
    );
}

void sensor_clipped_highlights_reconstruct_false_chroma() {
    const image::RawFrameLinearTransform transform{
        .camera_to_linear_srgb_d65 =
            {
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
            },
        .camera_rgb_to_linear_srgb_d65 =
            {
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
            },
        .camera_neutral = {1.0, 1.0, 1.0},
        .cfa_white_balance = {1.8, 1.0, 1.0, 1.5},
        .apply_cfa_white_balance = true,
    };
    const image::RawFrameLinearTransform identity{{
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    }};
    const image::RawFrameLinearTransform nikon_flat_top_transform{
        .camera_to_linear_srgb_d65 = identity.camera_to_linear_srgb_d65,
        .camera_rgb_to_linear_srgb_d65 = identity.camera_rgb_to_linear_srgb_d65,
        .camera_neutral = {1.0, 1.0, 1.0},
        .cfa_white_balance = {1.65, 1.0, 1.0, 1.70},
        .apply_cfa_white_balance = true,
    };
    for (const auto max_edge : {
             std::optional<std::uint32_t>{},
             std::optional<std::uint32_t>{3U},
         }) {
        const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            sensor_clipped_frame(),
            transform,
            max_edge,
            image::RawDevelopmentBackendMode::cpu
        );
        expect(
            cpu.highlight_recovery == image::RawHighlightRecoveryIntent::provider_default,
            "default fused development records Shadow's highlight treatment"
        );
        const auto disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            sensor_clipped_frame(),
            transform,
            max_edge,
            image::RawDevelopmentBackendMode::cpu,
            image::RawHighlightRecoveryIntent::disabled
        );
        float maximum_default_delta = 0.0F;
        float maximum_reconstructed_chroma = 0.0F;
        float maximum_disabled_chroma = 0.0F;
        for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); index += 3U) {
            const auto red = cpu.scene_linear.samples[index];
            const auto green = cpu.scene_linear.samples[index + 1U];
            const auto blue = cpu.scene_linear.samples[index + 2U];
            maximum_reconstructed_chroma = std::max(
                maximum_reconstructed_chroma,
                std::max({red, green, blue}) - std::min({red, green, blue})
            );
            const auto disabled_red = disabled.scene_linear.samples[index];
            const auto disabled_green = disabled.scene_linear.samples[index + 1U];
            const auto disabled_blue = disabled.scene_linear.samples[index + 2U];
            maximum_disabled_chroma = std::max(
                maximum_disabled_chroma,
                std::max({disabled_red, disabled_green, disabled_blue})
                    - std::min({disabled_red, disabled_green, disabled_blue})
            );
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                maximum_default_delta = std::max(
                    maximum_default_delta,
                    std::abs(
                        cpu.scene_linear.samples[index + channel]
                        - disabled.scene_linear.samples[index + channel]
                    )
                );
            }
        }
        expect(
            maximum_default_delta > 0.4F,
            "the physical sensor-white ceiling is distinct from the unbounded diagnostic path"
        );
        expect(
            maximum_reconstructed_chroma <= 1.0e-6F && maximum_disabled_chroma > 0.4F,
            "the physical sensor-white CFA plateau reaches the source white ceiling before demosaic"
        );
    }

    const auto disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        sensor_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    expect(
        disabled.valid()
            && disabled.highlight_recovery == image::RawHighlightRecoveryIntent::disabled
            && image::raw_highlight_treatment_identity(disabled.highlight_recovery)
                   == "sensor-highlights=disabled",
        "disabled source treatment remains explicit in the fused result"
    );
    bool disabled_preserves_channel_difference = false;
    for (std::size_t index = 0U; index < disabled.scene_linear.samples.size(); index += 3U) {
        const auto red = disabled.scene_linear.samples[index];
        const auto green = disabled.scene_linear.samples[index + 1U];
        const auto blue = disabled.scene_linear.samples[index + 2U];
        disabled_preserves_channel_difference =
            disabled_preserves_channel_difference
            || std::min({red, green, blue}) + 1.0e-3F < std::max({red, green, blue});
    }
    expect(
        disabled_preserves_channel_difference,
        "disabled source treatment preserves measured clipped sensor colours for diagnostics"
    );

    const auto mixed_boundary = mixed_highlight_boundary_frame();
    const auto default_boundary = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        mixed_boundary,
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto aggressive_boundary = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        mixed_boundary,
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::aggressive
    );
    expect(
        aggressive_boundary.valid()
            && image::raw_highlight_treatment_identity(aggressive_boundary.highlight_recovery)
                   == "sensor-highlights=cfa-opposed-linear-limit-feathered@20260823.4;"
                      "recovery=one-sided+spatial-chroma;headroom=sub-white-fp32;"
                      "clipped-highlight-surface=low-frequency-push-pull-luminance-protected-v2",
        "legacy aggressive source-surface reconstruction remains cache-visible"
    );
    float default_boundary_chroma = 0.0F;
    float aggressive_boundary_chroma = 0.0F;
    for (std::size_t index = 0U; index < default_boundary.scene_linear.samples.size();
         index += 3U) {
        const auto& default_samples = default_boundary.scene_linear.samples;
        const auto& aggressive_samples = aggressive_boundary.scene_linear.samples;
        default_boundary_chroma += std::max({
                                       default_samples[index],
                                       default_samples[index + 1U],
                                       default_samples[index + 2U],
                                   })
                                   - std::min({
                                       default_samples[index],
                                       default_samples[index + 1U],
                                       default_samples[index + 2U],
                                   });
        aggressive_boundary_chroma += std::max({
                                          aggressive_samples[index],
                                          aggressive_samples[index + 1U],
                                          aggressive_samples[index + 2U],
                                      })
                                      - std::min({
                                          aggressive_samples[index],
                                          aggressive_samples[index + 1U],
                                          aggressive_samples[index + 2U],
                                      });
    }
    expect(
        aggressive_boundary_chroma + 1.0e-3F < default_boundary_chroma,
        "aggressive repair feathering reduces false highlight chroma across a mixed CFA boundary"
    );

    const auto slanted_default = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        slanted_shared_clipping_boundary_frame(),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto slanted_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        slanted_shared_clipping_boundary_frame(),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    float slanted_default_chroma = 0.0F;
    float slanted_disabled_chroma = 0.0F;
    for (std::size_t index = 0U; index < slanted_default.scene_linear.samples.size(); index += 3U) {
        const auto& repaired = slanted_default.scene_linear.samples;
        const auto& measured = slanted_disabled.scene_linear.samples;
        slanted_default_chroma += std::max({
                                      repaired[index],
                                      repaired[index + 1U],
                                      repaired[index + 2U],
                                  })
                                  - std::min({
                                      repaired[index],
                                      repaired[index + 1U],
                                      repaired[index + 2U],
                                  });
        slanted_disabled_chroma += std::max({
                                       measured[index],
                                       measured[index + 1U],
                                       measured[index + 2U],
                                   })
                                   - std::min({
                                       measured[index],
                                       measured[index + 1U],
                                       measured[index + 2U],
                                   });
    }
    expect(
        slanted_default_chroma + 0.05F < slanted_disabled_chroma,
        "shared physical-white topology suppresses Bayer-phase false chroma on a slanted edge"
    );

    const auto nikon_flat_top = nikon_near_limit_green_boundary_frame();
    const auto nikon_default = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        nikon_flat_top,
        nikon_flat_top_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto nikon_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        nikon_flat_top,
        nikon_flat_top_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    const auto nikon_aggressive = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        nikon_flat_top,
        nikon_flat_top_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::aggressive
    );
    float nikon_default_maximum_chroma = 0.0F;
    float nikon_disabled_maximum_chroma = 0.0F;
    float nikon_aggressive_maximum_chroma = 0.0F;
    float nikon_default_bright_maximum_chroma = 0.0F;
    float nikon_disabled_bright_maximum_chroma = 0.0F;
    bool nikon_default_never_lowered = true;
    for (std::size_t index = 0U; index < nikon_default.scene_linear.samples.size(); index += 3U) {
        const auto& repaired = nikon_default.scene_linear.samples;
        const auto& measured = nikon_disabled.scene_linear.samples;
        const auto& softened = nikon_aggressive.scene_linear.samples;
        const float repaired_chroma =
            std::max({repaired[index], repaired[index + 1U], repaired[index + 2U]})
            - std::min({repaired[index], repaired[index + 1U], repaired[index + 2U]});
        const float measured_chroma =
            std::max({measured[index], measured[index + 1U], measured[index + 2U]})
            - std::min({measured[index], measured[index + 1U], measured[index + 2U]});
        const float softened_chroma =
            std::max({softened[index], softened[index + 1U], softened[index + 2U]})
            - std::min({softened[index], softened[index + 1U], softened[index + 2U]});
        nikon_default_maximum_chroma = std::max(nikon_default_maximum_chroma, repaired_chroma);
        nikon_disabled_maximum_chroma = std::max(nikon_disabled_maximum_chroma, measured_chroma);
        nikon_aggressive_maximum_chroma =
            std::max(nikon_aggressive_maximum_chroma, softened_chroma);
        const auto pixel = index / 3U;
        const auto output_x =
            static_cast<std::uint32_t>(pixel % nikon_default.scene_linear.dimensions.width);
        const auto output_y =
            static_cast<std::uint32_t>(pixel / nikon_default.scene_linear.dimensions.width);
        const auto raw_x = output_x + nikon_flat_top.descriptor.active_margins.left;
        const auto raw_y = output_y + nikon_flat_top.descriptor.active_margins.top;
        if (2U * raw_x + raw_y >= 13U) {
            nikon_default_bright_maximum_chroma =
                std::max(nikon_default_bright_maximum_chroma, repaired_chroma);
            nikon_disabled_bright_maximum_chroma =
                std::max(nikon_disabled_bright_maximum_chroma, measured_chroma);
        }
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            nikon_default_never_lowered =
                nikon_default_never_lowered
                && repaired[index + channel] >= measured[index + channel] - 1.0e-6F;
        }
    }
    const bool nikon_flat_top_repaired =
        nikon_default_never_lowered
        && nikon_default_bright_maximum_chroma + 0.05F < nikon_disabled_bright_maximum_chroma
        && nikon_aggressive_maximum_chroma + 0.05F < nikon_default_maximum_chroma;
    if (!nikon_flat_top_repaired) {
        std::cerr << "Nikon near-limit diagnostic: default-max-chroma="
                  << nikon_default_maximum_chroma
                  << " disabled-max-chroma=" << nikon_disabled_maximum_chroma
                  << " aggressive-max-chroma=" << nikon_aggressive_maximum_chroma
                  << " default-bright-max-chroma=" << nikon_default_bright_maximum_chroma
                  << " disabled-bright-max-chroma=" << nikon_disabled_bright_maximum_chroma
                  << " never-lowered=" << nikon_default_never_lowered << '\n';
    }
    expect(
        nikon_flat_top_repaired,
        "near-limit opposed repair lowers the worst Nikon-style green-flat-top false chroma "
        "without lowering a measured channel"
    );

    const auto one_channel = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        single_channel_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto one_channel_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        single_channel_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    for (std::size_t index = 0U; index < one_channel.scene_linear.samples.size(); index += 3U) {
        const auto red = one_channel.scene_linear.samples[index];
        const auto green = one_channel.scene_linear.samples[index + 1U];
        const auto blue = one_channel.scene_linear.samples[index + 2U];
        const auto min_channel = std::min({red, green, blue});
        const auto max_channel = std::max({red, green, blue});
        expect(
            max_channel > min_channel + 0.25F,
            "a single clipped CFA colour is not mistaken for a neutral highlight plateau"
        );
        expect(
            one_channel.scene_linear.samples[index]
                < one_channel_disabled.scene_linear.samples[index] - 0.5F,
            "a physically clipped CFA colour is limited at the source white point"
        );
    }

    const image::RawFrameLinearTransform canon_transform{
        {
            3.358267716535433,
            -0.7750,
            0.106317411402157,
            -0.312992125984252,
            1.633,
            -0.730354391371341,
            0.0,
            -0.6149,
            2.488443759630200,
        },
        {0.508, 1.0, 0.649},
    };
    const auto canon_highlight = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        canon_two_green_sites_clipped_frame(),
        canon_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto canon_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        canon_two_green_sites_clipped_frame(),
        canon_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    float canon_maximum_chroma = 0.0F;
    float canon_disabled_maximum_chroma = 0.0F;
    for (std::size_t index = 0U; index < canon_highlight.scene_linear.samples.size(); index += 3U) {
        const auto red = canon_highlight.scene_linear.samples[index];
        const auto green = canon_highlight.scene_linear.samples[index + 1U];
        const auto blue = canon_highlight.scene_linear.samples[index + 2U];
        canon_maximum_chroma = std::max(
            canon_maximum_chroma,
            std::max({red, green, blue}) - std::min({red, green, blue})
        );
        const auto disabled_red = canon_disabled.scene_linear.samples[index];
        const auto disabled_green = canon_disabled.scene_linear.samples[index + 1U];
        const auto disabled_blue = canon_disabled.scene_linear.samples[index + 2U];
        canon_disabled_maximum_chroma = std::max(
            canon_disabled_maximum_chroma,
            std::max({disabled_red, disabled_green, disabled_blue})
                - std::min({disabled_red, disabled_green, disabled_blue})
        );
    }
    expect(
        std::abs(canon_maximum_chroma - canon_disabled_maximum_chroma) <= 1.0e-6F,
        "two saturated green CFA sites do not invent a neutral or rewrite measured sub-white colour"
    );

    const auto near_white_default = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        near_white_but_unclipped_frame(),
        canon_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto near_white_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        near_white_but_unclipped_frame(),
        canon_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    float maximum_default_delta = 0.0F;
    for (std::size_t index = 0U; index < near_white_default.scene_linear.samples.size(); ++index) {
        maximum_default_delta = std::max(
            maximum_default_delta,
            std::abs(
                near_white_default.scene_linear.samples[index]
                - near_white_disabled.scene_linear.samples[index]
            )
        );
    }
    expect(
        maximum_default_delta <= 1.0e-6F,
        "sub-white CFA samples stay exactly measured until the physical sensor reaches white"
    );

    const auto multi_site = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        multi_channel_clipped_frame(),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto multi_site_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        multi_channel_clipped_frame(),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    float maximum_multi_site_chroma = 0.0F;
    float maximum_multi_site_disabled_chroma = 0.0F;
    for (std::size_t index = 0U; index < multi_site.scene_linear.samples.size(); index += 3U) {
        const auto default_red = multi_site.scene_linear.samples[index];
        const auto default_green = multi_site.scene_linear.samples[index + 1U];
        const auto default_blue = multi_site.scene_linear.samples[index + 2U];
        const auto disabled_red = multi_site_disabled.scene_linear.samples[index];
        const auto disabled_green = multi_site_disabled.scene_linear.samples[index + 1U];
        const auto disabled_blue = multi_site_disabled.scene_linear.samples[index + 2U];
        maximum_multi_site_chroma = std::max(
            maximum_multi_site_chroma,
            std::max({default_red, default_green, default_blue})
                - std::min({default_red, default_green, default_blue})
        );
        maximum_multi_site_disabled_chroma = std::max(
            maximum_multi_site_disabled_chroma,
            std::max({disabled_red, disabled_green, disabled_blue})
                - std::min({disabled_red, disabled_green, disabled_blue})
        );
    }
    expect(
        maximum_multi_site_chroma + 0.25F < maximum_multi_site_disabled_chroma,
        "two independently saturated CFA colours lose their untrustworthy shared chroma before a "
        "grade can amplify it"
    );

    const auto shoulder = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        two_channel_shoulder_frame(990U),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto shoulder_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        two_channel_shoulder_frame(990U),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    float maximum_shoulder_delta = 0.0F;
    float maximum_shoulder_chroma = 0.0F;
    float maximum_shoulder_disabled_chroma = 0.0F;
    for (std::size_t index = 0U; index < shoulder.scene_linear.samples.size(); index += 3U) {
        const auto& developed = shoulder.scene_linear.samples;
        const auto& measured = shoulder_disabled.scene_linear.samples;
        maximum_shoulder_delta = std::max(
            maximum_shoulder_delta,
            std::max({
                std::abs(developed[index] - measured[index]),
                std::abs(developed[index + 1U] - measured[index + 1U]),
                std::abs(developed[index + 2U] - measured[index + 2U]),
            })
        );
        maximum_shoulder_chroma = std::max(
            maximum_shoulder_chroma,
            std::max({developed[index], developed[index + 1U], developed[index + 2U]})
                - std::min({developed[index], developed[index + 1U], developed[index + 2U]})
        );
        maximum_shoulder_disabled_chroma = std::max(
            maximum_shoulder_disabled_chroma,
            std::max({measured[index], measured[index + 1U], measured[index + 2U]})
                - std::min({measured[index], measured[index + 1U], measured[index + 2U]})
        );
    }
    expect(
        maximum_shoulder_delta > 1.0e-3F
            && maximum_shoulder_chroma + 1.0e-3F < maximum_shoulder_disabled_chroma,
        "a near-white two-channel CFA shoulder begins a continuous neutral pull before a hard clip "
        "contour forms"
    );

    const auto saturated_red = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        saturated_red_frame(),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto saturated_red_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        saturated_red_frame(),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled
    );
    float saturated_red_maximum_delta = 0.0F;
    for (std::size_t index = 0U; index < saturated_red.scene_linear.samples.size(); index += 3U) {
        expect(
            saturated_red.scene_linear.samples[index]
                    > saturated_red.scene_linear.samples[index + 1U] + 0.5F
                && saturated_red.scene_linear.samples[index]
                       > saturated_red.scene_linear.samples[index + 2U] + 0.5F,
            "sensor-clipped saturated colour is not mistaken for a neutral highlight plateau"
        );
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            saturated_red_maximum_delta = std::max(
                saturated_red_maximum_delta,
                std::abs(
                    saturated_red.scene_linear.samples[index + channel]
                    - saturated_red_disabled.scene_linear.samples[index + channel]
                )
            );
        }
    }
    expect(
        saturated_red_maximum_delta <= 1.0e-6F,
        "one-sided repair preserves a uniformly saturated red emitter exactly"
    );

    if (!image::raw_development_backend_available(image::RawDevelopmentBackend::metal)) {
        expect(
            !environment_enabled("SHADOW_TEST_REQUIRE_METAL"),
            "Metal was required for highlight-treatment validation but is unavailable"
        );
        return;
    }
    const auto cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        two_channel_shoulder_frame(990U),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        two_channel_shoulder_frame(990U),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::metal
    );
    float maximum_enabled_difference = 0.0F;
    float maximum_enabled_magnitude = 0.0F;
    for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); ++index) {
        maximum_enabled_difference = std::max(
            maximum_enabled_difference,
            std::abs(cpu.scene_linear.samples[index] - metal.scene_linear.samples[index])
        );
        maximum_enabled_magnitude = std::max(
            maximum_enabled_magnitude,
            std::max(
                std::abs(cpu.scene_linear.samples[index]),
                std::abs(metal.scene_linear.samples[index])
            )
        );
    }
    const float enabled_parity_tolerance =
        8.0F * std::numeric_limits<float>::epsilon() * std::max(1.0F, maximum_enabled_magnitude);
    if (maximum_enabled_difference > enabled_parity_tolerance) {
        std::cerr << "enabled highlight CPU/Metal maximum difference: "
                  << maximum_enabled_difference << '\n';
    }
    expect(
        maximum_enabled_difference <= enabled_parity_tolerance,
        "Metal matches the CPU editable CFA-headroom path within bounded fp32 parity"
    );

    const auto slanted_metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        slanted_shared_clipping_boundary_frame(),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::metal
    );
    float maximum_slanted_difference = 0.0F;
    float maximum_slanted_magnitude = 0.0F;
    for (std::size_t index = 0U; index < slanted_default.scene_linear.samples.size(); ++index) {
        maximum_slanted_difference = std::max(
            maximum_slanted_difference,
            std::abs(
                slanted_default.scene_linear.samples[index]
                - slanted_metal.scene_linear.samples[index]
            )
        );
        maximum_slanted_magnitude = std::max(
            maximum_slanted_magnitude,
            std::max(
                std::abs(slanted_default.scene_linear.samples[index]),
                std::abs(slanted_metal.scene_linear.samples[index])
            )
        );
    }
    const float slanted_parity_tolerance =
        8.0F * std::numeric_limits<float>::epsilon() * std::max(1.0F, maximum_slanted_magnitude);
    expect(
        maximum_slanted_difference <= slanted_parity_tolerance,
        "Metal matches CPU opposed reconstruction on a slanted shared-clipping boundary"
    );

    const auto scaled_cpu_metal_parity = [](const auto& reference, const auto& candidate) {
        float maximum_difference = 0.0F;
        float maximum_magnitude = 0.0F;
        for (std::size_t index = 0U; index < reference.scene_linear.samples.size(); ++index) {
            maximum_difference = std::max(
                maximum_difference,
                std::abs(
                    reference.scene_linear.samples[index] - candidate.scene_linear.samples[index]
                )
            );
            maximum_magnitude = std::max(
                maximum_magnitude,
                std::max(
                    std::abs(reference.scene_linear.samples[index]),
                    std::abs(candidate.scene_linear.samples[index])
                )
            );
        }
        return maximum_difference
               <= 8.0F * std::numeric_limits<float>::epsilon() * std::max(1.0F, maximum_magnitude);
    };
    const auto nikon_metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        nikon_flat_top,
        nikon_flat_top_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::metal
    );
    expect(
        scaled_cpu_metal_parity(nikon_default, nikon_metal),
        "Metal matches CPU near-limit opposed repair on a Nikon-style flat top"
    );

    const auto nikon_area_cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        nikon_flat_top,
        nikon_flat_top_transform,
        3U,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto nikon_area_metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        nikon_flat_top,
        nikon_flat_top_transform,
        3U,
        image::RawDevelopmentBackendMode::metal
    );
    expect(
        scaled_cpu_metal_parity(nikon_area_cpu, nikon_area_metal),
        "Metal matches CPU clipped near-limit CFA area-preview integration"
    );

    const auto nikon_high_cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        nikon_flat_top,
        nikon_flat_top_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::high
    );
    const auto nikon_uniform_high_cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        nikon_near_limit_green_flat_top_frame(),
        nikon_flat_top_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::high
    );
    const auto nikon_uniform_high_disabled =
        image::develop_bayer_linear_srgb_f32_fused_with_backend(
            nikon_near_limit_green_flat_top_frame(),
            nikon_flat_top_transform,
            std::nullopt,
            image::RawDevelopmentBackendMode::cpu,
            image::RawHighlightRecoveryIntent::disabled,
            image::RawDevelopmentQuality::high
        );
    const auto nikon_high_metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        nikon_flat_top,
        nikon_flat_top_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::metal,
        image::RawHighlightRecoveryIntent::provider_default,
        image::RawDevelopmentQuality::high
    );
    const auto inner_bright_maximum_chroma = [&](const auto& development) {
        float maximum = 0.0F;
        const auto width = development.scene_linear.dimensions.width;
        for (std::size_t index = 0U; index < development.scene_linear.samples.size(); index += 3U) {
            const auto pixel = index / 3U;
            const auto raw_x = static_cast<std::uint32_t>(pixel % width)
                               + nikon_flat_top.descriptor.active_margins.left;
            const auto raw_y = static_cast<std::uint32_t>(pixel / width)
                               + nikon_flat_top.descriptor.active_margins.top;
            if (2U * raw_x + raw_y < 13U) {
                continue;
            }
            const auto& samples = development.scene_linear.samples;
            maximum = std::max(
                maximum,
                std::max({samples[index], samples[index + 1U], samples[index + 2U]})
                    - std::min({samples[index], samples[index + 1U], samples[index + 2U]})
            );
        }
        return maximum;
    };
    const float nikon_high_repaired_chroma = inner_bright_maximum_chroma(nikon_uniform_high_cpu);
    const float nikon_high_disabled_chroma =
        inner_bright_maximum_chroma(nikon_uniform_high_disabled);
    const bool nikon_high_repaired =
        nikon_high_repaired_chroma + 0.05F < nikon_high_disabled_chroma;
    if (!nikon_high_repaired) {
        std::cerr << "Nikon high-quality diagnostic: default-inner-chroma="
                  << nikon_high_repaired_chroma
                  << " disabled-inner-chroma=" << nikon_high_disabled_chroma << '\n';
    }
    expect(
        nikon_high_repaired,
        "high-quality detail applies near-limit opposed repair over its complete directional halo"
    );
    expect(
        scaled_cpu_metal_parity(nikon_high_cpu, nikon_high_metal),
        "Metal matches CPU near-limit opposed repair in high-quality detail/export"
    );

    const auto saturated_red_metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        saturated_red_frame(),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::metal
    );
    expect(
        scaled_cpu_metal_parity(saturated_red, saturated_red_metal),
        "Metal preserves the same saturated-red one-sided repair boundary as CPU"
    );

    const auto aggressive_cpu = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        mixed_highlight_boundary_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::aggressive
    );
    const auto aggressive_metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        mixed_highlight_boundary_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::metal,
        image::RawHighlightRecoveryIntent::aggressive
    );
    float maximum_aggressive_difference = 0.0F;
    for (std::size_t index = 0U; index < aggressive_cpu.scene_linear.samples.size(); ++index) {
        maximum_aggressive_difference = std::max(
            maximum_aggressive_difference,
            std::abs(
                aggressive_cpu.scene_linear.samples[index]
                - aggressive_metal.scene_linear.samples[index]
            )
        );
    }
    expect(
        maximum_aggressive_difference <= 4.0e-5F,
        "Metal matches the CPU aggressive CFA-confidence feathering path"
    );

    const auto disabled_metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        sensor_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::metal,
        image::RawHighlightRecoveryIntent::disabled
    );
    expect(
        disabled_metal.highlight_recovery == image::RawHighlightRecoveryIntent::disabled,
        "Metal records disabled source treatment"
    );
    float maximum_disabled_difference = 0.0F;
    for (std::size_t index = 0U; index < disabled.scene_linear.samples.size(); ++index) {
        maximum_disabled_difference = std::max(
            maximum_disabled_difference,
            std::abs(
                disabled.scene_linear.samples[index] - disabled_metal.scene_linear.samples[index]
            )
        );
    }
    expect(
        maximum_disabled_difference <= 4.0e-5F,
        "Metal disabled-source output stays within fp32 CPU tolerance"
    );
}

} // namespace

int main() {
    cfa_white_balance_retains_editable_headroom();
    sensor_clipped_highlights_reconstruct_false_chroma();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
