#include "../src/optics/scene_linear_region_optics.hpp"
#include "../src/proxy/full_edit_detail_source_preparation.hpp"
#include "../src/raw/raw_frame_region_development.hpp"
#include "../src/raw/raw_frame_source_preparation.hpp"
#include "../src/raw/resident_raw_source.hpp"
#include "raw_pipeline_routing_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/full_edit_detail.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

namespace {

using shadow::image::test_support::ScopedEnvironment;

static_assert(
    !std::is_aggregate_v<image::raw_pipeline_detail::PreparedRawFrameSource>
        && !std::is_copy_constructible_v<image::raw_pipeline_detail::PreparedRawFrameSource>,
    "prepared RawFrame source/plan pairs must be owner-constructed and move-only"
);

[[nodiscard]] image::RawFrame padded_bayer_frame() {
    image::RawFrame frame = synthetic_bayer_frame();
    frame.descriptor.storage_dimensions = {6U, 6U};
    frame.descriptor.active_dimensions = {4U, 4U};
    frame.descriptor.active_margins = {
        .left = 1U,
        .top = 1U,
        .right = 1U,
        .bottom = 1U,
    };
    frame.samples.resize(36U);
    for (std::uint32_t y = 0U; y < 6U; ++y) {
        for (std::uint32_t x = 0U; x < 6U; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            frame.samples[static_cast<std::size_t>(y) * 6U + x] =
                colour == image::RawCfaColor::red     ? 100U
                : colour == image::RawCfaColor::green ? 200U
                                                      : 50U;
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame spatial_margined_bayer_frame() {
    image::RawFrame frame = synthetic_bayer_frame();
    frame.descriptor.storage_dimensions = {9U, 7U};
    frame.descriptor.active_dimensions = {6U, 4U};
    frame.descriptor.active_margins = {
        .left = 1U,
        .top = 1U,
        .right = 2U,
        .bottom = 2U,
    };
    frame.descriptor.as_shot_neutral = {1.0, 1.0, 1.0, 1.0};
    frame.descriptor.white_levels = {4'095U, 4'095U, 4'095U, 4'095U};
    frame.samples.resize(9U * 7U);
    for (std::uint32_t y = 0U; y < 7U; ++y) {
        for (std::uint32_t x = 0U; x < 9U; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            const std::uint16_t colour_base = colour == image::RawCfaColor::red     ? 300U
                                              : colour == image::RawCfaColor::green ? 900U
                                                                                    : 1'500U;
            frame.samples[static_cast<std::size_t>(y) * 9U + x] =
                static_cast<std::uint16_t>(colour_base + x * 37U + y * 71U);
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame noisy_resident_bayer_frame() {
    image::RawFrame frame = synthetic_bayer_frame();
    auto& descriptor = frame.descriptor;
    descriptor.storage_dimensions = {12U, 12U};
    descriptor.active_dimensions = descriptor.storage_dimensions;
    descriptor.white_levels = {4'095U, 4'095U, 4'095U, 4'095U};
    descriptor.sensor_noise = {
        .schema_version = image::raw_sensor_noise_calibration_schema_version,
        .model = image::RawSensorNoiseModel::poisson_gaussian_per_cfa,
        .source = image::RawSensorNoiseCalibrationSource::provider_calibration_profile,
        .iso_sensitivity = 6'400.0,
        .read_noise_stddev_dn = {42.0, 39.0, 39.0, 44.0},
        .shot_noise_variance_per_dn = {0.08, 0.08, 0.08, 0.09},
    };
    frame.samples.resize(12U * 12U);
    constexpr std::array<std::uint16_t, 4U> flat_signal{
        600U,
        1'200U,
        1'160U,
        400U,
    };
    constexpr std::array<int, 4U> perturbation{-72, 56, -44, 68};
    for (std::uint32_t y = 0U; y < 12U; ++y) {
        for (std::uint32_t x = 0U; x < 12U; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const bool positive = (((x / 2U) + (y / 2U)) & 1U) == 0U;
            const int sample = static_cast<int>(flat_signal[site])
                               + (positive ? perturbation[site] : -perturbation[site]);
            frame.samples[static_cast<std::size_t>(y) * 12U + x] =
                static_cast<std::uint16_t>(sample);
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame active_only_reference(const image::RawFrame& padded) {
    image::RawFrame active = padded;
    const auto margins = padded.descriptor.active_margins;
    active.descriptor.storage_dimensions = padded.descriptor.active_dimensions;
    active.descriptor.active_margins = {};
    for (std::uint32_t y = 0U; y < 2U; ++y) {
        for (std::uint32_t x = 0U; x < 2U; ++x) {
            active.descriptor.bayer_2x2[static_cast<std::size_t>(y * 2U + x)] =
                padded.descriptor.bayer_2x2[static_cast<std::size_t>(
                    ((y + margins.top) & 1U) * 2U + ((x + margins.left) & 1U)
                )];
        }
    }
    active.samples.resize(
        static_cast<std::size_t>(active.descriptor.active_dimensions.pixel_count())
    );
    for (std::uint32_t y = 0U; y < active.descriptor.active_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < active.descriptor.active_dimensions.width; ++x) {
            active.samples
                [static_cast<std::size_t>(y) * active.descriptor.active_dimensions.width + x] =
                padded.samples
                    [static_cast<std::size_t>(y + margins.top)
                         * padded.descriptor.storage_dimensions.width
                     + x + margins.left];
        }
    }
    return active;
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> expected_source_coordinate(
    const std::uint32_t output_x,
    const std::uint32_t output_y,
    const image::Dimensions source,
    const std::int32_t orientation
) {
    switch (orientation) {
    case 3:
        return {
            source.width - 1U - output_x,
            source.height - 1U - output_y,
        };
    case 5:
        return {source.width - 1U - output_y, output_x};
    case 6:
        return {output_y, source.height - 1U - output_x};
    case 0:
    default:
        return {output_x, output_y};
    }
}

[[nodiscard]] std::vector<float>
crop_samples(const image::SceneLinearRgbFrame& source, const image::GeometryPixelRect rect) {
    std::vector<float> result;
    result.reserve(static_cast<std::size_t>(rect.width) * rect.height * 3U);
    for (std::uint32_t y = 0U; y < rect.height; ++y) {
        const std::size_t begin =
            (static_cast<std::size_t>(rect.y + y) * source.dimensions.width + rect.x) * 3U;
        result.insert(
            result.end(),
            source.samples.begin() + static_cast<std::ptrdiff_t>(begin),
            source.samples.begin()
                + static_cast<std::ptrdiff_t>(begin + static_cast<std::size_t>(rect.width) * 3U)
        );
    }
    return result;
}

class UnknownSceneLinearOpticsProvider final : public image::OpticsProvider {
  public:
    UnknownSceneLinearOpticsProvider() {
        info_.id = "unknown-region-optics";
        info_.version = "v1";
        info_.available = true;
    }

    [[nodiscard]] const image::OpticsProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] image::OpticsCorrectionResult correct_reference_rgb(
        const image::PixelBuffer&,
        const image::AssetMetadata&,
        const image::OpticsSettings&
    ) const override {
        return {};
    }

    [[nodiscard]] image::SceneLinearOpticsCorrectionResult correct_scene_linear_reference(
        const image::SceneLinearRgbFrame&,
        const image::AssetMetadata&,
        const image::OpticsSettings&
    ) const override {
        ++scene_linear_calls_;
        return image::SceneLinearOpticsCorrectionResult{
            .receipt = {
                .status = image::OpticsProfileStatus::incompatible_input,
                .provider_id = info_.id,
                .provider_version = info_.version,
            },
        };
    }

    [[nodiscard]] std::size_t scene_linear_calls() const noexcept {
        return scene_linear_calls_;
    }

  private:
    image::OpticsProviderInfo info_;
    mutable std::size_t scene_linear_calls_ = 0U;
};

void full_and_region_share_pixels_coordinates_and_cfa_phase() {
    const image::RawFrameLinearTransform transform{{
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
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        for (const auto quality : {
                 image::RawDevelopmentQuality::balanced,
                 image::RawDevelopmentQuality::high,
             }) {
            image::RawFrame frame = padded_bayer_frame();
            frame.descriptor.orientation = orientation;
            const image::GeometryPixelRect core{1U, 1U, 2U, 2U};
            const auto dependency = image::raw_pipeline_detail::prepare_raw_frame_region(
                frame,
                quality,
                image::detail::PreparedRawBayerDenoise{},
                core
            );
            const auto region = image::raw_pipeline_detail::develop_raw_frame_region_cpu(
                frame,
                transform,
                image::RawHighlightRecoveryIntent::provider_default,
                dependency
            );
            const auto full = image::develop_bayer_linear_srgb_f32_fused_with_backend(
                frame,
                transform,
                std::nullopt,
                image::RawDevelopmentBackendMode::cpu,
                image::RawHighlightRecoveryIntent::provider_default,
                quality
            );
            expect(
                region.samples == crop_samples(full.scene_linear, core),
                "resident region is byte-exact with the canonical full CPU developer "
                "across orientation, active margins, CFA phase, and quality"
            );
            expect(
                dependency.valid(frame.descriptor, full.scene_linear.dimensions)
                    && dependency.requested_core() == core
                    && dependency.optics_output_preimage() == core
                    && dependency.demosaic_halo()
                           == (quality == image::RawDevelopmentQuality::high ? 3U : 1U)
                    && dependency.algorithm()
                           == (quality == image::RawDevelopmentQuality::high
                                   ? image::RawDemosaicAlgorithm::bayer_edge_aware_v1
                                   : image::RawDemosaicAlgorithm::bayer_bilinear_v1),
                "region plan binds exact output, reconstruction, demosaic, and optics geometry"
            );

            auto changed = frame.descriptor;
            changed.orientation = orientation == 0 ? 3 : 0;
            expect(
                !dependency.valid(changed, full.scene_linear.dimensions),
                "a region plan cannot be paired with a different source orientation"
            );

            changed = frame.descriptor;
            changed.bayer_2x2[0] = changed.bayer_2x2[0] == image::RawCfaColor::red
                                       ? image::RawCfaColor::blue
                                       : image::RawCfaColor::red;
            expect(
                !dependency.valid(changed, full.scene_linear.dimensions),
                "a region plan cannot be paired with a different CFA phase"
            );
        }
    }
}

void spatial_margins_and_orientation_follow_an_independent_coordinate_oracle() {
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
    image::RawFrame padded = spatial_margined_bayer_frame();
    image::RawFrame active = active_only_reference(padded);
    const auto padded_baseline = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        padded,
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled,
        image::RawDevelopmentQuality::balanced
    );
    const auto active_baseline = image::develop_bayer_linear_srgb_f32_fused_with_backend(
        active,
        identity,
        std::nullopt,
        image::RawDevelopmentBackendMode::cpu,
        image::RawHighlightRecoveryIntent::disabled,
        image::RawDevelopmentQuality::balanced
    );
    expect(
        crop_samples(padded_baseline.scene_linear, image::GeometryPixelRect{1U, 1U, 4U, 2U})
            == crop_samples(active_baseline.scene_linear, image::GeometryPixelRect{1U, 1U, 4U, 2U}),
        "odd active margins and their shifted CFA phase match an explicit active-only interior "
        "reference"
    );

    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        image::RawFrame oriented = padded;
        oriented.descriptor.orientation = orientation;
        const image::GeometryPixelRect core{1U, 1U, 2U, 2U};
        const auto plan = image::raw_pipeline_detail::prepare_raw_frame_region(
            oriented,
            image::RawDevelopmentQuality::balanced,
            image::detail::PreparedRawBayerDenoise{},
            core
        );
        const auto region = image::raw_pipeline_detail::develop_raw_frame_region_cpu(
            oriented,
            identity,
            image::RawHighlightRecoveryIntent::disabled,
            plan
        );
        std::vector<float> expected;
        expected.reserve(2U * 2U * 3U);
        for (std::uint32_t local_y = 0U; local_y < core.height; ++local_y) {
            for (std::uint32_t local_x = 0U; local_x < core.width; ++local_x) {
                const auto [source_x, source_y] = expected_source_coordinate(
                    core.x + local_x,
                    core.y + local_y,
                    padded.descriptor.active_dimensions,
                    orientation
                );
                const std::size_t source_index =
                    (static_cast<std::size_t>(source_y) * padded.descriptor.active_dimensions.width
                     + source_x)
                    * 3U;
                expected.insert(
                    expected.end(),
                    padded_baseline.scene_linear.samples.begin()
                        + static_cast<std::ptrdiff_t>(source_index),
                    padded_baseline.scene_linear.samples.begin()
                        + static_cast<std::ptrdiff_t>(source_index + 3U)
                );
            }
        }
        expect(
            region.samples == expected,
            "oriented regions follow an independent non-square source-coordinate mapping"
        );
    }
}

void region_plan_names_denoise_preimage_and_clamps_boundaries() {
    const image::RawFrame frame = padded_bayer_frame();
    image::detail::PreparedRawBayerDenoise robust;
    robust.mode = image::RawBayerDenoiseMode::cfa_bilateral_noise_robust_v1;
    const auto edge = image::raw_pipeline_detail::prepare_raw_frame_region(
        frame,
        image::RawDevelopmentQuality::high,
        robust,
        image::GeometryPixelRect{0U, 0U, 1U, 1U}
    );
    expect(
        edge.demosaic_halo() == 3U && edge.denoise_halo() == 4U
            && edge.demosaic_sensor_preimage().x == 0U && edge.demosaic_sensor_preimage().y == 0U
            && edge.denoise_sensor_preimage().x == 0U && edge.denoise_sensor_preimage().y == 0U,
        "edge regions distinguish high-quality demosaic and robust CFA-denoise halos "
        "while clamping both sensor preimages"
    );
    expect(
        edge.reconstruction_core() == image::GeometryPixelRect{0U, 0U, 1U, 1U},
        "active-sensor reconstruction coordinates remain separate from stored-sensor margins"
    );
}

void unsupported_orientation_fails_closed_during_region_preparation() {
    image::RawFrame frame = padded_bayer_frame();
    frame.descriptor.orientation = 1;
    try {
        static_cast<void>(image::raw_pipeline_detail::prepare_raw_frame_region(
            frame,
            image::RawDevelopmentQuality::balanced,
            image::detail::PreparedRawBayerDenoise{},
            image::GeometryPixelRect{0U, 0U, 1U, 1U}
        ));
        expect(false, "region preparation rejects an unsupported orientation");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported_layout,
            "unsupported region orientation returns a typed layout error"
        );
    }
}

void metadata_preflight_uses_the_actual_retained_representation() {
    image::AssetMetadata metadata;
    metadata.raw_dimensions = {10'000U, 10'000U};
    metadata.image_dimensions = metadata.raw_dimensions;
    try {
        image::proxy_detail::validate_full_detail_source_preflight(
            metadata,
            image::proxy_detail::FullDetailSourceStorage::resident_raw_candidate
        );
    } catch (const image::DecodeError&) {
        expect(false, "100 MP metadata fits the resident uint16 CFA memory contract");
    }
    try {
        image::proxy_detail::validate_full_detail_source_preflight(
            metadata,
            image::proxy_detail::FullDetailSourceStorage::materialized_scene_linear
        );
        expect(false, "100 MP metadata exceeds complete fp32 scene-linear materialization");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::resource_limit,
            "materialized 100 MP metadata fails with the typed memory limit"
        );
    }
}

void structural_requirements_gate_metal_only_source_publication() {
    expect(
        image::proxy_detail::full_detail_source_allows_metal_publication(
            image::FullEditDetailSourceRequirements{}
        ),
        "an unrestricted render plan admits the resident Metal RAW source"
    );
    expect(
        !image::proxy_detail::full_detail_source_allows_metal_publication(
            image::FullEditDetailSourceRequirements{
                .requires_cpu_replay = true,
            }
        ),
        "a CPU-replay render plan rejects Metal-only source publication before residency"
    );
}

void cpu_resident_source_rejects_unbound_and_cross_source_optics() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    const image::CameraProfileCatalog catalog{
        .identity = "shadow-camera-profile-catalog-v1:resident-source-binding",
    };
    {
        SyntheticRawSession session(padded_bayer_frame());
        const image::AssetMetadata metadata = session.metadata();
        auto source = image::raw_pipeline_detail::prepare_raw_frame_source(
            session,
            image::default_raw_development_plan(),
            std::nullopt,
            catalog
        );
        auto unbound_optics = image::detail::prepare_scene_linear_region_optics(
            nullptr,
            source.development().reconstruction_dimensions(),
            metadata,
            image::default_optics_settings()
        );
        expect(
            !image::raw_pipeline_detail::cpu_resident_raw_source_supported(source, unbound_optics),
            "ordinary independently prepared optics are not CPU-resident eligible"
        );
        try {
            static_cast<void>(image::raw_pipeline_detail::prepare_resident_raw_source(
                std::move(source),
                std::move(unbound_optics)
            ));
            expect(false, "unbound optics must not publish a CPU-resident source");
        } catch (const image::DecodeError& error) {
            expect(
                error.code() == image::DecodeErrorCode::unsupported,
                "unbound CPU optics fail with the typed residency admission error"
            );
        }
    }

    SyntheticRawSession first_session(padded_bayer_frame());
    SyntheticRawSession second_session(padded_bayer_frame());
    auto first_source = image::raw_pipeline_detail::prepare_raw_frame_source(
        first_session,
        image::default_raw_development_plan(),
        std::nullopt,
        catalog
    );
    auto second_source = image::raw_pipeline_detail::prepare_raw_frame_source(
        second_session,
        image::default_raw_development_plan(),
        std::nullopt,
        catalog
    );
    auto first_optics =
        first_source.prepare_region_optics(nullptr, image::default_optics_settings());
    expect(
        !image::raw_pipeline_detail::cpu_resident_raw_source_supported(second_source, first_optics),
        "same-descriptor optics from another owner are not CPU-resident eligible"
    );
    try {
        static_cast<void>(image::raw_pipeline_detail::prepare_resident_raw_source(
            std::move(second_source),
            std::move(first_optics)
        ));
        expect(false, "cross-source optics must not publish a CPU-resident source");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported,
            "cross-source CPU optics fail before CFA denoise or region work"
        );
    }
}

void resident_aggregate_reuses_one_preparation_and_matches_materialization() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    const ScopedEnvironment pipeline_mode("SHADOW_RAW_PIPELINE", "raw-frame");
    const image::CameraProfileCatalog catalog = exact_dcp_catalog();

    SyntheticRawSession resident_session(padded_bayer_frame(), "Open Camera", "Mk I");
    auto prepared = image::raw_pipeline_detail::prepare_raw_frame_source(
        resident_session,
        image::default_raw_development_plan(),
        std::nullopt,
        catalog
    );
    auto optics = prepared.prepare_region_optics(nullptr, image::default_optics_settings());
    auto resident = image::raw_pipeline_detail::prepare_resident_raw_source(
        std::move(prepared),
        std::move(optics)
    );
    const image::GeometryPixelRect core{1U, 0U, 2U, 3U};
    const auto first_plan = resident.prepare_region(core);
    const auto second_plan = resident.prepare_region(core);
    const auto first = resident.develop_region(core);
    const auto second = resident.develop_region(core);

    SyntheticRawSession materialized_session(padded_bayer_frame(), "Open Camera", "Mk I");
    auto materialized_prepared = image::raw_pipeline_detail::prepare_raw_frame_source(
        materialized_session,
        image::default_raw_development_plan(),
        std::nullopt,
        catalog
    );
    const auto materialized = image::raw_pipeline_detail::materialize_prepared_raw_frame_source(
        std::move(materialized_prepared)
    );
    const auto& full = std::get<image::SceneLinearRgbFrame>(materialized.source);
    expect(
        first.scene_linear.samples == crop_samples(full, core)
            && second.scene_linear.samples == first.scene_linear.samples,
        "repeated resident requests reuse one prepared CFA source and match full materialization"
    );
    expect(
        first_plan.requested_core() == second_plan.requested_core()
            && first_plan.demosaic_sensor_preimage() == second_plan.demosaic_sensor_preimage()
            && first_plan.denoise_sensor_preimage() == second_plan.denoise_sensor_preimage(),
        "repeated region preparation is deterministic and source-bound"
    );
    expect(
        resident_session.raw_frame_count() == 1U && resident_session.processed_count() == 0U
            && resident.retained_bytes() < static_cast<std::uint64_t>(4U * 4U * 3U * sizeof(float)),
        "resident preparation decodes once, never asks for provider RGB, and avoids full fp32 RGB"
    );
    expect(
        resident.raw_development_receipt().development_settings_signature
                == materialized.raw_development_receipt.development_settings_signature
            && resident.raw_pipeline_receipt().pipeline_identity
                   == materialized.pipeline_receipt.pipeline_identity,
        "resident and materialized CPU development publish identical receipts"
    );
}

void robust_cfa_denoise_is_retained_once_and_matches_materialization() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    auto plan = image::default_raw_development_plan();
    plan.noise_reduction = image::RawNoiseReductionIntent::noise_robust;
    const image::CameraProfileCatalog catalog{
        .identity = "shadow-camera-profile-catalog-v1:resident-denoise-test",
    };

    SyntheticRawSession resident_session(noisy_resident_bayer_frame());
    auto prepared = image::raw_pipeline_detail::prepare_raw_frame_source(
        resident_session,
        plan,
        std::nullopt,
        catalog
    );
    auto optics = prepared.prepare_region_optics(nullptr, image::default_optics_settings());
    auto resident = image::raw_pipeline_detail::prepare_resident_raw_source(
        std::move(prepared),
        std::move(optics)
    );
    const image::GeometryPixelRect edge_core{0U, 0U, 5U, 5U};
    const auto first = resident.develop_region(edge_core);
    const auto repeated = resident.develop_region(edge_core);

    SyntheticRawSession materialized_session(noisy_resident_bayer_frame());
    auto complete = image::raw_pipeline_detail::materialize_prepared_raw_frame_source(
        image::raw_pipeline_detail::prepare_raw_frame_source(
            materialized_session,
            plan,
            std::nullopt,
            catalog
        )
    );
    const auto& complete_scene = std::get<image::SceneLinearRgbFrame>(complete.source);
    expect(
        first.scene_linear.samples == crop_samples(complete_scene, edge_core)
            && repeated.scene_linear.samples == first.scene_linear.samples
            && first.dependency_plan.denoise_halo() == 4U,
        "noise-robust resident edge regions reuse the denoised CFA and match full materialization"
    );
    expect(
        resident_session.raw_frame_count() == 1U && materialized_session.raw_frame_count() == 1U
            && resident.raw_development_receipt().development_settings_signature.find(
                   "raw-denoise=cfa-bilateral-noise-robust-v1"
               ) != std::string::npos
            && resident.raw_development_receipt().development_settings_signature
                   == complete.raw_development_receipt.development_settings_signature,
        "resident denoise executes once at preparation and publishes the materialized receipt"
    );
}

void public_full_detail_session_uses_the_resident_source() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    const ScopedEnvironment pipeline_mode("SHADOW_RAW_PIPELINE", "raw-frame");
    SyntheticRawSession decoder(padded_bayer_frame());
    const auto session = image::prepare_full_edit_detail(
        decoder,
        image::default_raw_development_plan(),
        image::FullEditDetailSourceRequirements{
            .requires_cpu_replay = true,
        }
    );
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "resident-neutral",
            .parameters = image::ExposureAdjustment{},
        },
    };
    const auto first = session.render_rgb8(nodes, image::DetailTileRect{1U, 1U, 2U, 2U});
    const auto second = session.render_rgb8(nodes, image::DetailTileRect{1U, 1U, 2U, 2U});
    expect(
        decoder.raw_frame_count() == 1U && decoder.processed_count() == 0U,
        "public full-detail preparation and repeated tiles decode one RawFrame only"
    );
    expect(
        session.retained_bytes() < static_cast<std::uint64_t>(4U * 4U * 3U * sizeof(float))
            && session.cpu_replay_available()
            && session.raw_pipeline_receipt().path == image::RawPipelinePath::shadow_raw_frame,
        "required CPU-replay detail retains capable CFA storage instead of complete fp32 RGB"
    );
    expect(
        first.bytes == second.bytes
            && first.execution.backend == image::DetailTileRenderBackend::cpu
            && !first.execution.fell_back,
        "public resident detail renders deterministic CPU tiles without a false fallback"
    );
}

void unknown_optics_provider_forces_complete_materialization() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    const ScopedEnvironment pipeline_mode("SHADOW_RAW_PIPELINE", "raw-frame");
    SyntheticRawSession decoder(padded_bayer_frame());
    UnknownSceneLinearOpticsProvider optics;
    const auto session =
        image::prepare_full_edit_detail(decoder, &optics, image::default_optics_settings());
    const std::uint64_t complete_fp32_bytes = 4U * 4U * 3U * sizeof(float);
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "forced-cpu-materialized-optics",
            .parameters = image::ExposureAdjustment{},
        },
    };
    const auto tile = session.render_rgb8(nodes, image::DetailTileRect{0U, 0U, 2U, 2U});
    expect(
        optics.scene_linear_calls() == 1U && session.retained_bytes() >= complete_fp32_bytes
            && session.optics_receipt().status == image::OpticsProfileStatus::incompatible_input
            && tile.execution.backend == image::DetailTileRenderBackend::cpu
            && !tile.execution.fell_back,
        "forced CPU with non-resident optics materializes and renders without entering Metal"
    );
    expect(
        decoder.raw_frame_count() == 1U && decoder.processed_count() == 0U,
        "optics fail-closed materialization reuses the already prepared RawFrame"
    );
}

void automatic_full_detail_fallback_does_not_decode_raw_twice() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
    const ScopedEnvironment pipeline_mode("SHADOW_RAW_PIPELINE", "auto");
    image::RawFrame unsupported = padded_bayer_frame();
    unsupported.descriptor.orientation = 1;
    SyntheticRawSession decoder(std::move(unsupported));
    const auto session = image::prepare_full_edit_detail(decoder);
    expect(
        decoder.raw_frame_count() == 1U && decoder.processed_count() == 1U,
        "automatic full-detail fallback attempts an unsupported RawFrame only once"
    );
    expect(
        session.raw_pipeline_receipt().path
                == image::RawPipelinePath::provider_processed_compatibility
            && session.raw_pipeline_receipt().fallback_reason.find("orientation")
                   != std::string::npos,
        "single-pass full-detail fallback preserves the original RawFrame failure reason"
    );
}

} // namespace

int main() {
    full_and_region_share_pixels_coordinates_and_cfa_phase();
    spatial_margins_and_orientation_follow_an_independent_coordinate_oracle();
    region_plan_names_denoise_preimage_and_clamps_boundaries();
    unsupported_orientation_fails_closed_during_region_preparation();
    metadata_preflight_uses_the_actual_retained_representation();
    structural_requirements_gate_metal_only_source_publication();
    cpu_resident_source_rejects_unbound_and_cross_source_optics();
    resident_aggregate_reuses_one_preparation_and_matches_materialization();
    robust_cfa_denoise_is_retained_once_and_matches_materialization();
    public_full_detail_session_uses_the_resident_source();
    unknown_optics_provider_forces_complete_materialization();
    automatic_full_detail_fallback_does_not_decode_raw_twice();
    return failures == 0 ? 0 : 1;
}
