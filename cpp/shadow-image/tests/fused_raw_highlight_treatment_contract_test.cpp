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

void cfa_white_balance_retains_editable_headroom_before_display_h0() {
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
            == "sensor-highlights=editable-cfa-confidence@20260821.6;recovery=none;"
               "source=measured-cfa-wb;display=libraw-h0",
        "the source receipt identifies editable CFA headroom and final LibRaw H=0 display"
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
    float maximum_near_white_delta = 0.0F;
    float maximum_near_white_value = 0.0F;
    for (std::size_t index = 0U; index < near_white.scene_linear.samples.size(); ++index) {
        maximum_near_white_delta = std::max(
            maximum_near_white_delta,
            std::abs(
                near_white.scene_linear.samples[index]
                - near_white_disabled.scene_linear.samples[index]
            )
        );
        maximum_near_white_value =
            std::max(maximum_near_white_value, near_white.scene_linear.samples[index]);
    }
    expect(
        maximum_near_white_delta <= 1.0e-6F && maximum_near_white_value > 1.5F,
        "a sub-white CFA sample retains white-balance-induced headroom for later highlight edits"
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
        "Metal matches the CPU LibRaw H=0 measured-CFA path"
    );
}

void sensor_clipped_highlights_respect_h0_boundaries() {
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
        std::abs(maximum_multi_site_chroma - maximum_multi_site_disabled_chroma) <= 1.0e-6F,
        "a saturated multi-site colour remains measured when no post-WB component exceeds white"
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
        maximum_shoulder_delta <= 1.0e-6F
            && std::abs(maximum_shoulder_chroma - maximum_shoulder_disabled_chroma) <= 1.0e-6F,
        "the source path does not alter measured camera colour below physical sensor white"
    );

    const auto saturated_red = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        saturated_red_frame(),
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    for (std::size_t index = 0U; index < saturated_red.scene_linear.samples.size(); index += 3U) {
        expect(
            saturated_red.scene_linear.samples[index]
                    > saturated_red.scene_linear.samples[index + 1U] + 0.5F
                && saturated_red.scene_linear.samples[index]
                       > saturated_red.scene_linear.samples[index + 2U] + 0.5F,
            "sensor-clipped saturated colour is not mistaken for a neutral highlight plateau"
        );
    }

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
    cfa_white_balance_retains_editable_headroom_before_display_h0();
    sensor_clipped_highlights_respect_h0_boundaries();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
