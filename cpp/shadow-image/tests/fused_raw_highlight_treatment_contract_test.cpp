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

void sensor_clipped_highlights_are_neutral_before_u16_clipping() {
    const image::RawFrameLinearTransform transform{{
        1.60,
        -0.40,
        0.00,
        0.00,
        0.85,
        0.00,
        0.10,
        -0.20,
        1.50,
    }};
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
            "default fused development records sensor-highlight neutralization"
        );
        for (std::size_t index = 0U; index < cpu.scene_linear.samples.size(); index += 3U) {
            const auto red = cpu.scene_linear.samples[index];
            const auto green = cpu.scene_linear.samples[index + 1U];
            const auto blue = cpu.scene_linear.samples[index + 2U];
            const auto min_channel = std::min({red, green, blue});
            const auto max_channel = std::max({red, green, blue});
            expect(
                min_channel > 1.0F && max_channel - min_channel <= 1.0e-4F,
                "sensor-clipped highlights carry neutral scene-linear luminance without a hue"
            );
        }
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
        "disabled highlight treatment remains explicit in the fused result"
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
        "disabled highlight treatment does not silently neutralize clipped sensor colours"
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
            min_channel > 0.5F && max_channel - min_channel <= 0.25F,
            "a single clipped CFA colour with near-white companions stays bounded before output "
            "mapping"
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
    float maximum_metal_chroma = 0.0F;
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
    for (std::size_t index = 0U; index < metal.scene_linear.samples.size(); index += 3U) {
        const auto red = metal.scene_linear.samples[index];
        const auto green = metal.scene_linear.samples[index + 1U];
        const auto blue = metal.scene_linear.samples[index + 2U];
        maximum_metal_chroma = std::max(
            maximum_metal_chroma,
            std::max({red, green, blue}) - std::min({red, green, blue})
        );
    }
    const float enabled_parity_tolerance =
        8.0F * std::numeric_limits<float>::epsilon() * std::max(1.0F, maximum_enabled_magnitude);
    if (maximum_enabled_difference > enabled_parity_tolerance) {
        std::cerr << "enabled highlight CPU/Metal maximum difference: "
                  << maximum_enabled_difference << '\n';
    }
    expect(
        maximum_metal_chroma <= enabled_parity_tolerance
            && maximum_enabled_difference <= enabled_parity_tolerance,
        "Metal applies neutral sensor-highlight recovery within bounded fp32 CPU parity"
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
        "Metal records disabled sensor-highlight treatment"
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
        "Metal disabled-highlight output stays within fp32 CPU tolerance"
    );
}

} // namespace

int main() {
    sensor_clipped_highlights_are_neutral_before_u16_clipping();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
