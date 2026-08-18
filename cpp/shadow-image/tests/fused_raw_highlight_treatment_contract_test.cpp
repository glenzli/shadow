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
            const auto colour = frame.descriptor.bayer_2x2[site];
            // Red and blue are at sensor white, while green remains close enough to make the
            // old independent u16 clipping produce a magenta false highlight after a camera
            // matrix. This models the clipped-sun failure seen in real CR3 files.
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                static_cast<std::uint16_t>(
                    colour == image::RawCfaColor::green ? 980U : frame.descriptor.white_levels[site]
                );
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
                    colour == image::RawCfaColor::red ? frame.descriptor.white_levels[site] : 970U
                );
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame canon_single_channel_clipped_frame() {
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

[[nodiscard]] image::RawFrame near_white_but_unclipped_frame() {
    auto frame = synthetic_frame(0);
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            const std::uint16_t sample = colour == image::RawCfaColor::red     ? 990U
                                         : colour == image::RawCfaColor::green ? 985U
                                                                               : 980U;
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                sample;
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
                    colour == image::RawCfaColor::red ? frame.descriptor.white_levels[site]
                                                       : 100U
                );
        }
    }
    return frame;
}

void cfa_white_balance_preserves_scene_linear_headroom() {
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
            == "sensor-highlights=scene-linear-cfa-headroom-through-demosaic-20260819.1",
        "the default source treatment identifies its unbounded CFA-white-balance contract"
    );

    float maximum_default_delta = 0.0F;
    float maximum_red = 0.0F;
    float maximum_blue = 0.0F;
    for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); index += 3U) {
        maximum_red = std::max(maximum_red, cpu.scene_linear.samples[index]);
        maximum_blue = std::max(maximum_blue, cpu.scene_linear.samples[index + 2U]);
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
        maximum_red > 1.9F && maximum_blue > 1.4F,
        "CFA white-balance gains retain calibrated RAW headroom through demosaic"
    );
    expect(
        maximum_default_delta <= 1.0e-6F,
        "the default source treatment does not apply a hidden CFA-channel clip"
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
        "Metal retains the same CFA white-balance headroom as the CPU path"
    );
}

void sensor_clipped_highlights_preserve_measured_source_colour() {
    const image::RawFrameLinearTransform transform{
        {
            2.0,
            0.0,
            0.00,
            0.00,
            1.00,
            0.00,
            0.0,
            0.0,
            1.5384615384615385,
        },
        {0.5, 1.0, 0.65},
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
            "default fused development records measured source colour treatment"
        );
        const auto disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
            sensor_clipped_frame(),
            transform,
            max_edge,
            image::RawDevelopmentBackendMode::cpu,
            image::RawHighlightRecoveryIntent::disabled
        );
        float maximum_default_delta = 0.0F;
        float maximum_measured_chroma = 0.0F;
        for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); index += 3U) {
            const auto red = cpu.scene_linear.samples[index];
            const auto green = cpu.scene_linear.samples[index + 1U];
            const auto blue = cpu.scene_linear.samples[index + 2U];
            maximum_measured_chroma = std::max(
                maximum_measured_chroma,
                std::max({red, green, blue}) - std::min({red, green, blue})
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
            maximum_default_delta <= 1.0e-6F,
            "default source development does not rewrite a clipped camera colour"
        );
        expect(
            maximum_measured_chroma > 0.20F,
            "sensor-clipped camera colour remains measured until a later explicit rendering stage"
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
        "disabled source treatment preserves measured clipped sensor colours"
    );

    const auto one_channel = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        single_channel_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    for (std::size_t index = 0U; index < one_channel.scene_linear.samples.size(); index += 3U) {
        const auto red = one_channel.scene_linear.samples[index];
        const auto green = one_channel.scene_linear.samples[index + 1U];
        const auto blue = one_channel.scene_linear.samples[index + 2U];
        const auto min_channel = std::min({red, green, blue});
        const auto max_channel = std::max({red, green, blue});
        expect(
            max_channel > min_channel + 0.50F,
            "a single clipped CFA colour is not desaturated during source reconstruction"
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
        canon_single_channel_clipped_frame(),
        canon_transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto canon_disabled = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        canon_single_channel_clipped_frame(),
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
        "a Canon-like clipped sun keeps its measured source chroma until explicit rendering"
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
        "near-white source samples are not changed by a parser-stage highlight policy"
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
            "sensor-clipped saturated colour is not mistaken for a white highlight"
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
        sensor_clipped_frame(),
        transform,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu
    );
    const auto metal = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        sensor_clipped_frame(),
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
        "Metal preserves measured sensor colour within bounded fp32 CPU parity"
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
    cfa_white_balance_preserves_scene_linear_headroom();
    sensor_clipped_highlights_preserve_measured_source_colour();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
