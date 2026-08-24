#include "../src/raw/bayer_sampling.hpp"
#include "../tools/raw_highlight_reference_pipeline.hpp"
#include "contract_test_assertions.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

[[nodiscard]] int rgb_channel(const image::RawCfaColor color) noexcept {
    switch (color) {
    case image::RawCfaColor::red:
        return 0;
    case image::RawCfaColor::green:
        return 1;
    case image::RawCfaColor::blue:
        return 2;
    case image::RawCfaColor::unknown:
        return -1;
    }
    return -1;
}

[[nodiscard]] image::RawFrame isolated_clipped_red_frame(
    const std::uint32_t extent = 18U,
    const std::uint32_t clipped_radius = 1U
) {
    image::RawFrame frame;
    auto& descriptor = frame.descriptor;
    descriptor.provider_id = "darktable-reference-contract";
    descriptor.provider_version = "v1";
    descriptor.storage_dimensions = {extent, extent};
    descriptor.active_dimensions = descriptor.storage_dimensions;
    descriptor.orientation = 0;
    descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    descriptor.cfa_pattern = "RGGB";
    descriptor.bits_per_sample = 12U;
    descriptor.black_levels = {0U, 0U, 0U, 0U};
    descriptor.white_levels = {1'000U, 1'000U, 1'000U, 1'000U};
    descriptor.as_shot_neutral = {1.0, 1.0, 1.0, 1.0};
    frame.samples.resize(static_cast<std::size_t>(descriptor.storage_dimensions.pixel_count()));
    for (std::uint32_t y = 0U; y < descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < descriptor.storage_dimensions.width; ++x) {
            const auto site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto color = descriptor.bayer_2x2[site];
            std::uint16_t value = color == image::RawCfaColor::red ? 500U : 800U;
            const std::uint32_t center = extent / 2U;
            if (color == image::RawCfaColor::red && x + clipped_radius >= center
                && x <= center + clipped_radius && y + clipped_radius >= center
                && y <= center + clipped_radius) {
                value = 1'000U;
            }
            frame.samples[static_cast<std::size_t>(y) * descriptor.storage_dimensions.width + x] =
                value;
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrameLinearTransform reference_transform() {
    return image::RawFrameLinearTransform{
        .camera_to_linear_srgb_d65 = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0},
        .camera_rgb_to_linear_srgb_d65 = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0},
        .camera_neutral = {1.0, 1.0, 1.0},
        .cfa_white_balance = {1.0, 2.0, 2.0, 2.0},
        .apply_cfa_white_balance = true,
    };
}

void opposed_reference_changes_only_factually_clipped_photosites() {
    const auto plane = image::probe_detail::make_darktable_opposed_reference_plane(
        isolated_clipped_red_frame(),
        reference_transform()
    );
    expect(plane.valid(), "darktable reference must own valid measured and reconstructed planes");
    expect(plane.clipped_photosites > 0U, "fixture must contain factual clipped photosites");
    expect(plane.changed_photosites > 0U, "opposed reference must lift the clipped red phase");
    expect(
        plane.changed_photosites <= plane.clipped_photosites,
        "opposed reference must never expand repair beyond factual clipping"
    );

    std::uint64_t observed_changes = 0U;
    for (std::uint32_t y = 0U; y < plane.dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < plane.dimensions.width; ++x) {
            const auto index = static_cast<std::size_t>(y) * plane.dimensions.width + x;
            const auto color = plane.bayer_2x2[(y & 1U) * 2U + (x & 1U)];
            const int channel = rgb_channel(color);
            expect(channel >= 0, "reference CFA must contain only RGB photosites");
            if (channel < 0) {
                continue;
            }
            const bool clipped = plane.measured_samples[index]
                                 >= plane.clip_values[static_cast<std::size_t>(channel)];
            const bool changed = plane.reconstructed_samples[index] > plane.measured_samples[index];
            observed_changes += changed ? 1U : 0U;
            expect(
                clipped || !changed,
                "an unclipped photosite must remain byte-for-byte numerically unchanged"
            );
            expect(
                plane.reconstructed_samples[index] >= plane.measured_samples[index],
                "opposed reconstruction must be one-sided"
            );
        }
    }
    expect(
        observed_changes == plane.changed_photosites,
        "reference change accounting must match its owned CFA plane"
    );
}

void opposed_reference_area_output_is_finite() {
    const auto plane = image::probe_detail::make_darktable_opposed_reference_plane(
        isolated_clipped_red_frame(),
        reference_transform()
    );
    const auto camera_rgb = image::probe_detail::darktable_opposed_area_camera_rgb_at(
        plane,
        image::Dimensions{6U, 6U},
        3U,
        3U
    );
    for (const float value : camera_rgb) {
        expect(std::isfinite(value) && value >= 0.0F, "reference area RGB must stay finite");
    }
}

void production_area_matches_point_owned_reference_without_expanding_the_write_mask() {
    const auto frame = isolated_clipped_red_frame();
    const auto transform = reference_transform();
    const auto plane =
        image::probe_detail::make_darktable_opposed_reference_plane(frame, transform);
    const image::Dimensions target_dimensions{6U, 6U};
    const auto grid = image::detail::make_bayer_area_sampling_grid(frame, target_dimensions);
    const auto policy = image::detail::editable_raw_cfa_sampling_policy(transform);

    bool matches = true;
    float maximum_delta = 0.0F;
    for (std::uint32_t y = 0U; y < target_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < target_dimensions.width; ++x) {
            const auto reference = image::probe_detail::darktable_opposed_area_camera_rgb_at(
                plane,
                target_dimensions,
                x,
                y
            );
            const auto production =
                image::detail::area_camera_rgb_sample_at(frame, grid, x, y, &transform, policy);
            for (std::size_t channel = 0U; channel < reference.size(); ++channel) {
                const float delta = std::abs(reference[channel] - production.values[channel]);
                maximum_delta = std::max(maximum_delta, delta);
                matches = matches && delta < 1.0e-6F;
            }
            matches = matches && production.highlight_chroma_neutralization == 0.0F;
        }
    }
    expect(
        matches,
        "default production area reconstruction must equal the point-owned Darktable oracle and "
        "must not add a post-area RGB write"
    );
    if (!matches) {
        std::cerr << "production/reference maximum area delta=" << maximum_delta << '\n';
    }
}

void bounded_source_model_matches_the_direct_oracle_estimator_across_white_balance() {
    const auto frame = isolated_clipped_red_frame(66U, 4U);
    auto transform = reference_transform();
    for (const std::array<double, 4U> gains : {
             std::array<double, 4U>{1.0, 2.0, 2.0, 2.0},
             std::array<double, 4U>{1.4, 0.9, 1.1, 2.3},
         }) {
        transform.cfa_white_balance = gains;
        const auto policy = image::detail::editable_raw_cfa_sampling_policy(transform);
        const auto direct = image::detail::estimate_opposed_highlight_chrominance_correction(
            frame,
            &transform,
            policy,
            1U
        );
        const auto model =
            image::detail::build_opposed_highlight_chrominance_model(frame, 1U, 4'096U);
        const auto compiled =
            image::detail::evaluate_opposed_highlight_chrominance_model(model, &transform, policy);
        expect(
            model.any_terminal_photosite
                && direct.supporting_samples == compiled.supporting_samples,
            "bounded opposed model must retain the direct estimator's source support identity"
        );
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            expect(
                model.support_records[channel].size() <= 4'096U,
                "opposed model memory must remain bounded independently of sensor dimensions"
            );
            expect(
                std::abs(direct.offsets[channel] - compiled.offsets[channel]) < 1.0e-6F,
                "cached phase-resolved support must reproduce the direct estimator after WB rebind"
            );
        }
    }
}

} // namespace

int main() {
    opposed_reference_changes_only_factually_clipped_photosites();
    opposed_reference_area_output_is_finite();
    production_area_matches_point_owned_reference_without_expanding_the_write_mask();
    bounded_source_model_matches_the_direct_oracle_estimator_across_white_balance();
    return failures == 0 ? 0 : 1;
}
