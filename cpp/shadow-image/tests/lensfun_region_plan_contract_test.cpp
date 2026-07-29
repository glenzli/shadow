#include "../src/optics/lensfun_modifier_plan.hpp"
#include "../src/optics/scene_linear_region_optics.hpp"
#include "contract_test_assertions.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/optics.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace {

namespace image = shadow::image;
namespace lensfun_plan = shadow::image::detail::lensfun_modifier_plan;
using image::test_support::expect;
using image::test_support::failures;
using image::test_support::ScopedEnvironment;

static_assert(
    !std::is_default_constructible_v<lensfun_plan::PreparedRegion>
        && !std::is_aggregate_v<lensfun_plan::PreparedRegion>,
    "region evidence must only be constructible by its concrete prepared optics owner"
);

[[nodiscard]] image::AssetMetadata nikon_d850_metadata() {
    image::AssetMetadata metadata;
    metadata.make = "Nikon Corporation";
    metadata.model = "Nikon D850";
    metadata.normalized_make = "Nikon";
    metadata.normalized_model = "D850";
    metadata.lens_make = "Nikon";
    metadata.lens_model = "Nikon AF Nikkor 50mm f/1.4D";
    metadata.focal_length_mm = 50.0;
    metadata.focal_length_35mm = 50.0;
    metadata.aperture_f_number = 1.4;
    metadata.focus_distance_meters = 10.0;
    return metadata;
}

[[nodiscard]] image::SceneLinearRgbFrame source_frame(const image::Dimensions dimensions) {
    image::SceneLinearRgbFrame result;
    result.dimensions = dimensions;
    result.row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float);
    result.samples.resize(static_cast<std::size_t>(dimensions.width) * dimensions.height * 3U);
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const std::size_t pixel = (static_cast<std::size_t>(y) * dimensions.width + x) * 3U;
            const float normalized_x = static_cast<float>(x) / static_cast<float>(dimensions.width);
            const float normalized_y =
                static_cast<float>(y) / static_cast<float>(dimensions.height);
            result.samples[pixel] = 0.08F + normalized_x * 0.71F + normalized_y * 0.03F;
            result.samples[pixel + 1U] = 0.11F + normalized_y * 0.63F + normalized_x * 0.05F;
            result.samples[pixel + 2U] = 0.14F + normalized_x * 0.27F + normalized_y * 0.41F;
        }
    }
    return result;
}

[[nodiscard]] image::SceneLinearRgbFrame
textured_hdr_dcp_like_source_frame(const image::Dimensions dimensions) {
    image::SceneLinearRgbFrame result;
    result.dimensions = dimensions;
    result.row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float);
    result.samples.resize(static_cast<std::size_t>(dimensions.width) * dimensions.height * 3U);
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const std::uint32_t hash =
                (x * 73'856'093U) ^ (y * 19'349'663U) ^ ((x + y) * 83'492'791U);
            const std::size_t pixel = (static_cast<std::size_t>(y) * dimensions.width + x) * 3U;
            result.samples[pixel] = (hash & 1U) == 0U ? -0.75F : 63.75F;
            result.samples[pixel + 1U] = (hash & 2U) == 0U ? 0.005F : 31.5F;
            result.samples[pixel + 2U] = (hash & 4U) == 0U ? -1.25F : 47.875F;
        }
    }
    return result;
}

[[nodiscard]] image::SceneLinearRgbFrame
crop(const image::SceneLinearRgbFrame& source, const image::GeometryPixelRect rect) {
    image::SceneLinearRgbFrame result;
    result.dimensions = {rect.width, rect.height};
    result.row_stride_bytes = static_cast<std::size_t>(rect.width) * 3U * sizeof(float);
    result.samples.resize(static_cast<std::size_t>(rect.width) * rect.height * 3U);
    for (std::uint32_t y = 0U; y < rect.height; ++y) {
        for (std::uint32_t x = 0U; x < rect.width; ++x) {
            const std::size_t source_pixel =
                (static_cast<std::size_t>(rect.y + y) * source.dimensions.width + rect.x + x) * 3U;
            const std::size_t output_pixel = (static_cast<std::size_t>(y) * rect.width + x) * 3U;
            std::copy_n(
                source.samples.begin() + static_cast<std::ptrdiff_t>(source_pixel),
                3U,
                result.samples.begin() + static_cast<std::ptrdiff_t>(output_pixel)
            );
        }
    }
    return result;
}

[[nodiscard]] bool near(
    const std::span<const float> left,
    const std::span<const float> right,
    // Stable Lensfun computes vignette blocks with a block-local float recurrence. Starting an
    // otherwise identical tile at a non-zero x can differ from its full-row oracle by a few ULPs.
    // Keep that implementation property bounded well below one 16-bit display code.
    const float tolerance = 1.5e-5F
) {
    return left.size() == right.size()
           && std::equal(
               left.begin(),
               left.end(),
               right.begin(),
               [tolerance](const float first, const float second) {
                   return std::abs(first - second) <= tolerance;
               }
           );
}

[[nodiscard]] float
maximum_difference(const std::span<const float> left, const std::span<const float> right) {
    float result = 0.0F;
    const std::size_t count = std::min(left.size(), right.size());
    for (std::size_t index = 0U; index < count; ++index) {
        result = std::max(result, std::abs(left[index] - right[index]));
    }
    return result;
}

[[nodiscard]] std::optional<image::GeometryPixelRect>
expected_preimage(const std::span<const float> coordinates, const image::Dimensions dimensions) {
    std::uint32_t minimum_x = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t minimum_y = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum_x = 0U;
    std::uint32_t maximum_y = 0U;
    bool found = false;
    for (std::size_t index = 0U; index < coordinates.size(); index += 2U) {
        const float x = coordinates[index];
        const float y = coordinates[index + 1U];
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0F || y < 0.0F
            || x > static_cast<float>(dimensions.width - 1U)
            || y > static_cast<float>(dimensions.height - 1U)) {
            continue;
        }
        const auto x0 = static_cast<std::uint32_t>(std::floor(x));
        const auto y0 = static_cast<std::uint32_t>(std::floor(y));
        const std::uint32_t x1 = std::min(x0 + 1U, dimensions.width - 1U);
        const std::uint32_t y1 = std::min(y0 + 1U, dimensions.height - 1U);
        minimum_x = std::min(minimum_x, x0);
        minimum_y = std::min(minimum_y, y0);
        maximum_x = std::max(maximum_x, x1);
        maximum_y = std::max(maximum_y, y1);
        found = true;
    }
    if (!found) {
        return std::nullopt;
    }
    return image::GeometryPixelRect{
        .x = minimum_x,
        .y = minimum_y,
        .width = maximum_x - minimum_x + 1U,
        .height = maximum_y - minimum_y + 1U,
    };
}

[[nodiscard]] image::SceneLinearRgbFrame render_region(
    const image::SceneLinearRgbFrame& source,
    const std::shared_ptr<const lensfun_plan::LensfunModifierPlan>& plan,
    const lensfun_plan::PreparedRegion& region
) {
    image::SceneLinearRgbFrame source_preimage;
    if (region.source_preimage().has_value()) {
        source_preimage = crop(source, *region.source_preimage());
    }
    return plan->render_scene_linear_region_cpu(source_preimage, region);
}

void requested_but_unmatched_profiles_are_actually_neutral() {
    auto provider = image::make_lensfun_optics_provider();
    image::AssetMetadata missing;
    missing.make = "Shadow Missing Camera";
    missing.model = "Shadow Missing Model";
    missing.lens_model = "Shadow Missing Lens";
    missing.focal_length_mm = 35.0;
    const auto prepared = image::detail::prepare_scene_linear_region_optics(
        provider.get(),
        {128U, 96U},
        missing,
        image::default_optics_settings()
    );
    expect(
        prepared.kind() == image::detail::SceneLinearRegionOpticsKind::neutral
            && prepared.resident_eligible() && prepared.lensfun_plan() == nullptr,
        "requested profile geometry becomes neutral when no profile actually matches"
    );
    expect(
        prepared.receipt().status != image::OpticsProfileStatus::matched,
        "the unmatched neutral plan retains its real profile-resolution status"
    );
}

void matched_plan_survives_provider_and_tiles_match_the_full_oracle() {
    const auto* database = std::getenv("SHADOW_TEST_LENSFUN_DB");
    if (database == nullptr || *database == '\0') {
        return;
    }
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    constexpr image::Dimensions dimensions{128U, 96U};
    const auto metadata = nikon_d850_metadata();
    const auto settings = image::default_optics_settings();
    const auto source = source_frame(dimensions);

    auto provider = image::make_lensfun_optics_provider(std::filesystem::path{database});
    expect(provider->info().available, "the region-plan Lensfun fixture database loads");
    if (!provider->info().available) {
        return;
    }
    const auto full = provider->correct_scene_linear_reference(source, metadata, settings);
    auto prepared = image::detail::prepare_scene_linear_region_optics(
        provider.get(),
        dimensions,
        metadata,
        settings
    );
    expect(
        prepared.kind() == image::detail::SceneLinearRegionOpticsKind::coordinate_remap
            && prepared.has_owned_coordinate_remap() && prepared.receipt().applied_distortion
            && prepared.receipt().applied_tca && prepared.receipt().applied_vignetting
            && prepared.receipt().applied_scaling,
        "a normal matched profile compiles one owned distortion/TCA/vignette/autoscale plan"
    );
    expect(
        full.corrected_scene_linear_rgb.has_value(),
        "the matched full-frame Lensfun oracle materializes corrected pixels"
    );
    if (!prepared.has_owned_coordinate_remap() || !full.corrected_scene_linear_rgb.has_value()) {
        return;
    }
    const auto plan = prepared.lensfun_plan();
    provider.reset();

    const std::vector<image::GeometryPixelRect> tiles{
        {0U, 0U, 31U, 17U},
        {31U, 0U, 97U, 17U},
        {0U, 17U, 53U, 28U},
        {53U, 17U, 75U, 28U},
        {0U, 45U, 19U, 51U},
        {19U, 45U, 109U, 51U},
    };
    image::SceneLinearRgbFrame stitched;
    stitched.dimensions = dimensions;
    stitched.row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float);
    stitched.samples.assign(
        static_cast<std::size_t>(dimensions.width) * dimensions.height * 3U,
        -1.0F
    );

    for (const auto tile : tiles) {
        const auto region = plan->prepare_region(tile);
        expect(
            region.executor_identity() == lensfun_plan::region_executor_identity
                && region.coordinate_remap()
                && region.absolute_source_coordinates().size()
                       == static_cast<std::size_t>(tile.width) * tile.height * 6U,
            "every irregular tile carries the v1 absolute R/G/B inverse-coordinate contract"
        );
        expect(
            region.source_preimage()
                == expected_preimage(region.absolute_source_coordinates(), dimensions),
            "the source preimage is exactly the union of every in-bounds 2x2 channel footprint"
        );
        if (region.source_preimage().has_value() && region.profile_vignetting()) {
            expect(
                region.profile_vignetting_gains().size()
                    == static_cast<std::size_t>(region.source_preimage()->width)
                           * region.source_preimage()->height * 3U,
                "profile vignetting gains cover the exact pre-remap source preimage"
            );
        }
        const auto rendered = render_region(source, plan, region);
        const auto oracle_tile = crop(*full.corrected_scene_linear_rgb, tile);
        const bool tile_matches = near(rendered.samples, oracle_tile.samples);
        if (!tile_matches) {
            std::cerr << "Lensfun tile mismatch at " << tile.x << ',' << tile.y << " size "
                      << tile.width << 'x' << tile.height
                      << " max-diff=" << maximum_difference(rendered.samples, oracle_tile.samples)
                      << '\n';
        }
        expect(
            tile_matches,
            "an irregular region is numerically equal to the independent full-frame Lensfun oracle"
        );
        for (std::uint32_t y = 0U; y < tile.height; ++y) {
            for (std::uint32_t x = 0U; x < tile.width; ++x) {
                const std::size_t source_pixel =
                    (static_cast<std::size_t>(y) * tile.width + x) * 3U;
                const std::size_t target_pixel =
                    (static_cast<std::size_t>(tile.y + y) * dimensions.width + tile.x + x) * 3U;
                std::copy_n(
                    rendered.samples.begin() + static_cast<std::ptrdiff_t>(source_pixel),
                    3U,
                    stitched.samples.begin() + static_cast<std::ptrdiff_t>(target_pixel)
                );
            }
        }
    }
    expect(
        near(stitched.samples, full.corrected_scene_linear_rgb->samples),
        "irregular Lensfun tiles stitch without seams or uncovered pixels"
    );

    const image::GeometryPixelRect concurrent_tile{11U, 9U, 73U, 61U};
    const auto expected = crop(*full.corrected_scene_linear_rgb, concurrent_tile).samples;
    std::vector<std::future<bool>> futures;
    for (std::size_t index = 0U; index < 8U; ++index) {
        futures.push_back(std::async(std::launch::async, [&, plan] {
            const auto region = plan->prepare_region(concurrent_tile);
            return near(render_region(source, plan, region).samples, expected);
        }));
    }
    expect(
        std::all_of(futures.begin(), futures.end(), [](auto& future) { return future.get(); }),
        "one owned plan concurrently prepares and renders regions after provider destruction"
    );

}

void profile_and_manual_vignettes_remain_pointwise_and_ordered() {
    const auto* database = std::getenv("SHADOW_TEST_LENSFUN_DB");
    if (database == nullptr || *database == '\0') {
        return;
    }
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    constexpr image::Dimensions dimensions{128U, 96U};
    const auto metadata = nikon_d850_metadata();
    auto settings = image::default_optics_settings();
    settings.correct_distortion = false;
    settings.correct_tca = false;
    settings.automatic_scale = false;
    settings.manual_vignetting_amount = 24;
    settings.manual_vignetting_midpoint = 37U;
    const auto source = source_frame(dimensions);
    auto provider = image::make_lensfun_optics_provider(std::filesystem::path{database});
    const auto full = provider->correct_scene_linear_reference(source, metadata, settings);
    auto prepared = image::detail::prepare_scene_linear_region_optics(
        provider.get(),
        dimensions,
        metadata,
        settings
    );
    expect(
        prepared.kind() == image::detail::SceneLinearRegionOpticsKind::pointwise
            && prepared.resident_eligible() && prepared.lensfun_plan() != nullptr
            && prepared.receipt().applied_vignetting,
        "profile plus manual vignetting is an owned pointwise resident plan"
    );
    provider.reset();
    if (!full.corrected_scene_linear_rgb.has_value()) {
        expect(false, "the pointwise full-frame oracle materializes corrected pixels");
        return;
    }
    const image::GeometryPixelRect tile{23U, 19U, 67U, 51U};
    auto region = crop(source, tile);
    image::detail::apply_scene_linear_region_optics(region, tile, prepared);
    expect(
        near(region.samples, crop(*full.corrected_scene_linear_rgb, tile).samples),
        "pointwise execution applies profile vignetting before the manual output vignette"
    );
}

void concrete_plan_identity_rejects_other_lens_evidence() {
    const auto* database = std::getenv("SHADOW_TEST_LENSFUN_DB");
    if (database == nullptr || *database == '\0') {
        return;
    }
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    constexpr image::Dimensions dimensions{128U, 96U};
    constexpr image::GeometryPixelRect tile{17U, 13U, 53U, 41U};
    const auto settings = image::default_optics_settings();
    auto provider = image::make_lensfun_optics_provider(std::filesystem::path{database});
    auto candidate_metadata = nikon_d850_metadata();
    const auto candidates = provider->profile_candidates(candidate_metadata);
    expect(
        candidates.size() >= 2U,
        "the pinned camera catalog exposes two distinct lens plans for owner-binding validation"
    );
    if (candidates.size() < 2U) {
        return;
    }

    const auto prepare = [&](const image::OpticsProfileCandidate& candidate) {
        auto metadata = candidate_metadata;
        metadata.lens_make = candidate.lens_maker;
        metadata.lens_model = candidate.lens_model;
        return image::detail::prepare_scene_linear_region_optics(
            provider.get(),
            dimensions,
            metadata,
            settings
        );
    };
    auto first = prepare(candidates.front());
    auto second = prepare(candidates.back());
    expect(
        first.lensfun_plan() != nullptr && second.lensfun_plan() != nullptr
            && first.receipt().lens_profile != second.receipt().lens_profile,
        "same-sized, same-settings preparations retain two concrete lens identities"
    );
    if (first.lensfun_plan() == nullptr || second.lensfun_plan() == nullptr) {
        return;
    }

    const auto source = source_frame(dimensions);
    const auto first_region = first.prepare_region(tile);
    expect(
        first.owns_region(first_region) && !second.owns_region(first_region),
        "only the concrete optics owner recognizes its immutable region evidence"
    );
    image::SceneLinearRgbFrame source_preimage;
    if (first_region.source_preimage().has_value()) {
        source_preimage = crop(source, *first_region.source_preimage());
    }
    try {
        static_cast<void>(
            second.lensfun_plan()->render_scene_linear_region_cpu(source_preimage, first_region)
        );
        expect(false, "a second lens plan must reject same-shaped evidence from the first");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "cross-plan CPU evidence fails as an invalid owner contract"
        );
    }
    expect(
        first.lensfun_plan()->render_scene_linear_region_cpu(source_preimage, first_region).valid(),
        "the issuing lens plan still renders its own region evidence"
    );
}

void textured_hdr_regions_match_full_frame_call_boundaries() {
    const auto* database = std::getenv("SHADOW_TEST_LENSFUN_DB");
    if (database == nullptr || *database == '\0') {
        return;
    }
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    constexpr float exact_fp32_tolerance = 0.0F;
    const auto metadata = nikon_d850_metadata();
    const std::vector<image::Dimensions> dimension_cases{
        {128U, 96U},
        {96U, 128U},
    };
    for (const image::Dimensions dimensions : dimension_cases) {
        const auto source = textured_hdr_dcp_like_source_frame(dimensions);
        std::vector<image::OpticsSettings> setting_cases;
        {
            auto geometry = image::default_optics_settings();
            geometry.correct_vignetting = false;
            setting_cases.push_back(geometry);
        }
        {
            auto profile_vignette = image::default_optics_settings();
            profile_vignette.correct_distortion = false;
            profile_vignette.correct_tca = false;
            profile_vignette.automatic_scale = false;
            setting_cases.push_back(profile_vignette);
        }
        {
            auto combined = image::default_optics_settings();
            combined.manual_vignetting_amount = 37;
            combined.manual_vignetting_midpoint = 43U;
            setting_cases.push_back(combined);
        }

        const std::vector<image::GeometryPixelRect> tiles{
            {1U, 1U, dimensions.width / 2U, dimensions.height / 2U},
            {
                dimensions.width / 3U,
                17U,
                dimensions.width - dimensions.width / 3U - 1U,
                std::min(37U, dimensions.height - 17U),
            },
            {
                7U,
                47U,
                dimensions.width - 13U,
                std::min(43U, dimensions.height - 47U),
            },
            {
                dimensions.width - 29U,
                dimensions.height - 23U,
                28U,
                22U,
            },
        };

        for (std::size_t setting_index = 0U; setting_index < setting_cases.size();
             ++setting_index) {
            const auto& settings = setting_cases[setting_index];
            auto provider = image::make_lensfun_optics_provider(std::filesystem::path{database});
            const auto full = provider->correct_scene_linear_reference(source, metadata, settings);
            auto prepared = image::detail::prepare_scene_linear_region_optics(
                provider.get(),
                dimensions,
                metadata,
                settings
            );
            expect(
                prepared.lensfun_plan() != nullptr && full.corrected_scene_linear_rgb.has_value(),
                "the textured HDR Lensfun parity case resolves an owned plan and full oracle"
            );
            if (prepared.lensfun_plan() == nullptr || !full.corrected_scene_linear_rgb.has_value()) {
                continue;
            }
            const auto plan = prepared.lensfun_plan();
            provider.reset();
            const image::GeometryPixelRect whole_rect{
                0U,
                0U,
                dimensions.width,
                dimensions.height,
            };
            const auto whole_region = plan->prepare_region(whole_rect);
            const auto whole_rendered = render_region(source, plan, whole_region);
            const float whole_difference = maximum_difference(
                whole_rendered.samples,
                full.corrected_scene_linear_rgb->samples
            );
            if (whole_difference > exact_fp32_tolerance) {
                std::cerr << "Whole textured HDR Lensfun mismatch for settings " << setting_index
                          << " in " << dimensions.width << 'x' << dimensions.height
                          << " max-diff=" << whole_difference << '\n';
            }
            expect(
                whole_difference <= exact_fp32_tolerance,
                "whole-frame region evidence matches the full-frame Lensfun oracle"
            );
            for (const auto tile : tiles) {
                const auto region = plan->prepare_region(tile);
                const auto rendered = render_region(source, plan, region);
                const auto expected = crop(*full.corrected_scene_linear_rgb, tile);
                const float difference = maximum_difference(rendered.samples, expected.samples);
                if (difference > exact_fp32_tolerance) {
                    std::cerr << "Textured HDR Lensfun mismatch at " << tile.x << ',' << tile.y
                              << " size " << tile.width << 'x' << tile.height << " in "
                              << dimensions.width << 'x' << dimensions.height << " settings "
                              << setting_index << " max-diff=" << difference << '\n';
                }
                expect(
                    difference <= exact_fp32_tolerance,
                    "textured HDR region evidence matches the full-frame coordinate and vignette "
                    "call boundaries"
                );
            }
        }
    }
}

} // namespace

int main() {
    requested_but_unmatched_profiles_are_actually_neutral();
    matched_plan_survives_provider_and_tiles_match_the_full_oracle();
    profile_and_manual_vignettes_remain_pointwise_and_ordered();
    concrete_plan_identity_rejects_other_lens_evidence();
    textured_hdr_regions_match_full_frame_call_boundaries();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
