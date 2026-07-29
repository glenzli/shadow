#include "../src/optics/scene_linear_region_optics.hpp"
#include "../src/raw/metal_raw_development.hpp"
#include "../src/raw/metal_resident_raw_source.hpp"
#include "../src/raw/raw_frame_development_plan.hpp"
#include "../src/raw/raw_frame_region_development.hpp"
#include "../src/raw/raw_frame_source_preparation.hpp"
#include "../src/raw/resident_raw_source.hpp"
#include "raw_pipeline_routing_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using shadow::image::test_support::ScopedEnvironment;

using MetalResidentFactory =
    decltype(&image::raw_pipeline_detail::try_prepare_metal_resident_raw_source);
static_assert(
    !std::is_invocable_v<
        MetalResidentFactory,
        image::RawFrame,
        const image::raw_pipeline_detail::PreparedRawFrameDevelopment&>,
    "a RawFrame and independently prepared development plan must not be pairable"
);

[[nodiscard]] image::RawFrame resident_fixture(std::string provider_id = "generic-raw-provider") {
    image::RawFrame frame = synthetic_bayer_frame();
    frame.descriptor.provider_id = std::move(provider_id);
    frame.descriptor.provider_version = "provider-v1";
    frame.descriptor.storage_dimensions = {16U, 14U};
    frame.descriptor.active_dimensions = {12U, 10U};
    frame.descriptor.active_margins = {
        .left = 1U,
        .top = 1U,
        .right = 3U,
        .bottom = 3U,
    };
    frame.descriptor.white_levels = {4'095U, 4'095U, 4'095U, 4'095U};
    frame.descriptor.sensor_noise = {
        .schema_version = image::raw_sensor_noise_calibration_schema_version,
        .model = image::RawSensorNoiseModel::poisson_gaussian_per_cfa,
        .source = image::RawSensorNoiseCalibrationSource::provider_calibration_profile,
        .iso_sensitivity = 6'400.0,
        .read_noise_stddev_dn = {41.0, 38.0, 39.0, 43.0},
        .shot_noise_variance_per_dn = {0.08, 0.08, 0.08, 0.09},
    };
    frame.samples.resize(
        static_cast<std::size_t>(frame.descriptor.storage_dimensions.pixel_count())
    );
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            const std::uint16_t base = colour == image::RawCfaColor::red     ? 620U
                                       : colour == image::RawCfaColor::green ? 1'220U
                                                                             : 430U;
            const int noise = ((x / 2U + y / 2U) & 1U) == 0U ? 51 : -47;
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                static_cast<std::uint16_t>(
                    static_cast<int>(base) + noise + static_cast<int>(x * 7U + y * 11U)
                );
        }
    }
    return frame;
}

[[nodiscard]] image::RawFrame wide_resident_fixture() {
    image::RawFrame frame = synthetic_bayer_frame();
    frame.descriptor.provider_id = "wide-generic-raw-provider";
    frame.descriptor.provider_version = "provider-v1";
    frame.descriptor.storage_dimensions = {4'096U, 4U};
    frame.descriptor.active_dimensions = frame.descriptor.storage_dimensions;
    frame.descriptor.active_margins = {};
    frame.descriptor.white_levels = {4'095U, 4'095U, 4'095U, 4'095U};
    frame.samples.resize(
        static_cast<std::size_t>(frame.descriptor.storage_dimensions.pixel_count())
    );
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const auto colour = frame.descriptor.bayer_2x2[site];
            const std::uint16_t base = colour == image::RawCfaColor::red     ? 700U
                                       : colour == image::RawCfaColor::green ? 1'300U
                                                                             : 500U;
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                static_cast<std::uint16_t>(base + x % 97U);
        }
    }
    return frame;
}

[[nodiscard]] image::RawDevelopmentPlan resident_plan(const bool denoise) {
    image::RawDevelopmentPlan plan = image::default_raw_development_plan();
    plan.quality = image::RawDevelopmentQuality::high;
    plan.noise_reduction = denoise ? image::RawNoiseReductionIntent::noise_robust
                                   : image::RawNoiseReductionIntent::disabled;
    return plan;
}

struct PreparedResidentOwner final {
    image::raw_pipeline_detail::PreparedRawFrameSource source;
    image::detail::PreparedSceneLinearRegionOptics optics;
};

[[nodiscard]] image::CameraProfileCatalog neutral_catalog() {
    return image::CameraProfileCatalog{
        .identity = "shadow-camera-profile-catalog-v1:metal-resident-neutral",
    };
}

[[nodiscard]] PreparedResidentOwner prepare_resident_owner(
    image::RawFrame frame,
    const image::RawDevelopmentPlan& plan,
    const image::CameraProfileCatalog& catalog,
    std::string normalized_make = {},
    std::string normalized_model = {}
) {
    SyntheticRawSession session(
        std::move(frame),
        std::move(normalized_make),
        std::move(normalized_model)
    );
    auto source =
        image::raw_pipeline_detail::prepare_raw_frame_source(session, plan, std::nullopt, catalog);
    auto optics = source.prepare_region_optics(nullptr, image::default_optics_settings());
    return PreparedResidentOwner{
        .source = std::move(source),
        .optics = std::move(optics),
    };
}

[[nodiscard]] image::raw_pipeline_detail::ResidentRawSourceAttempt
publish_resident_owner(PreparedResidentOwner owner) {
    return image::raw_pipeline_detail::try_prepare_metal_resident_raw_source(
        std::move(owner.source),
        std::move(owner.optics)
    );
}

void source_binding_rejects_unbound_and_cross_source_optics() {
    {
        SyntheticRawSession session(resident_fixture());
        const image::AssetMetadata metadata = session.metadata();
        auto source = image::raw_pipeline_detail::prepare_raw_frame_source(
            session,
            resident_plan(false),
            std::nullopt,
            neutral_catalog()
        );
        auto unbound_optics = image::detail::prepare_scene_linear_region_optics(
            nullptr,
            source.development().reconstruction_dimensions(),
            metadata,
            image::default_optics_settings()
        );
        try {
            static_cast<void>(image::raw_pipeline_detail::try_prepare_metal_resident_raw_source(
                std::move(source),
                std::move(unbound_optics)
            ));
            expect(false, "ordinary independently prepared optics must not publish a RAW source");
        } catch (const image::DecodeError& error) {
            expect(
                error.code() == image::DecodeErrorCode::invalid_request
                    && std::string_view(error.what()).find("independently prepared")
                           != std::string_view::npos,
                "unbound optics fail before Metal availability or device publication"
            );
        }
    }

    auto first =
        prepare_resident_owner(resident_fixture(), resident_plan(false), neutral_catalog());
    auto second =
        prepare_resident_owner(resident_fixture(), resident_plan(false), neutral_catalog());
    try {
        static_cast<void>(image::raw_pipeline_detail::try_prepare_metal_resident_raw_source(
            std::move(second.source),
            std::move(first.optics)
        ));
        expect(false, "same-descriptor optics from a different RAW owner must be rejected");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request
                && std::string_view(error.what()).find("independently prepared")
                       != std::string_view::npos,
            "cross-source optics fail before sample upload or device publication"
        );
    }
}

[[nodiscard]] std::vector<float>
crop_samples(const image::SceneLinearRgbFrame& source, const image::GeometryPixelRect rect) {
    std::vector<float> result;
    result.reserve(static_cast<std::size_t>(rect.width) * rect.height * 3U);
    const std::size_t source_stride = source.row_stride_bytes / sizeof(float);
    for (std::uint32_t y = 0U; y < rect.height; ++y) {
        const std::size_t first = static_cast<std::size_t>(rect.y + y) * source_stride
                                  + static_cast<std::size_t>(rect.x) * 3U;
        result.insert(
            result.end(),
            source.samples.begin() + static_cast<std::ptrdiff_t>(first),
            source.samples.begin()
                + static_cast<std::ptrdiff_t>(first + static_cast<std::size_t>(rect.width) * 3U)
        );
    }
    return result;
}

[[nodiscard]] image::detail::MetalRawDevelopmentAttempt full_metal_reference(
    const image::RawFrame& frame,
    const image::raw_pipeline_detail::PreparedRawFrameDevelopment& development
) {
    return image::detail::try_develop_bayer_linear_srgb_f32_metal(
        frame,
        development.linear_transform(),
        std::nullopt,
        development.development_plan().highlight_recovery,
        development.development_plan().quality,
        image::detail::MetalRawDevelopmentContinuations{
            .dcp_color_transform = development.camera_profile(),
            .raw_denoise = &development.raw_denoise(),
            .project_sensor_clipping = false,
        }
    );
}

void repeated_neutral_and_dcp_regions_match_full_metal() {
    const std::array<image::GeometryPixelRect, 2U> regions{
        image::GeometryPixelRect{.x = 1U, .y = 2U, .width = 5U, .height = 4U},
        image::GeometryPixelRect{.x = 6U, .y = 1U, .width = 4U, .height = 6U},
    };
    for (const bool with_dcp : {false, true}) {
        image::RawFrame frame = resident_fixture();
        const std::uint64_t expected_upload_bytes =
            static_cast<std::uint64_t>(frame.samples.size() * sizeof(std::uint16_t));
        const image::RawFrame full_frame = frame;
        const image::RawDevelopmentPlan plan = resident_plan(true);
        const image::CameraProfileCatalog catalog =
            with_dcp ? exact_dcp_catalog() : neutral_catalog();
        auto owner = prepare_resident_owner(
            std::move(frame),
            plan,
            catalog,
            with_dcp ? "Open Camera" : "",
            with_dcp ? "Mk I" : ""
        );
        auto full = full_metal_reference(full_frame, owner.source.development());
        expect(
            full.development.has_value(),
            "complete Metal RAW reference is available for the resident contract"
        );
        if (!full.development.has_value()) {
            continue;
        }

        auto attempt = publish_resident_owner(std::move(owner));
        expect(
            attempt.published() && !attempt.fallback_source.has_value()
                && attempt.diagnostic.empty(),
            "resident publication consumes the host fallback only after Metal completion"
        );
        if (!attempt.published()) {
            continue;
        }
        const auto& metal_source = attempt.source->metal_source();
        const auto first = metal_source.develop_region(regions[0]);
        const auto repeated = metal_source.develop_region(regions[0]);
        const auto second = metal_source.develop_region(regions[1]);
        expect(
            first.scene_linear.samples == crop_samples(full.development->scene_linear, regions[0])
                && repeated.scene_linear.samples == first.scene_linear.samples
                && second.scene_linear.samples
                       == crop_samples(full.development->scene_linear, regions[1]),
            with_dcp ? "DCP resident regions exactly match complete Metal crops"
                     : "neutral resident regions exactly match complete Metal crops"
        );

        const auto telemetry = metal_source.telemetry();
        const std::uint64_t expected_readback =
            static_cast<std::uint64_t>(regions[0].width * regions[0].height * 3U * sizeof(float))
                * 2U
            + static_cast<std::uint64_t>(regions[1].width * regions[1].height * 3U * sizeof(float));
        expect(
            telemetry.source_upload_count == 1U
                && telemetry.source_upload_bytes == expected_upload_bytes
                && telemetry.denoise_dispatch_count == 1U && telemetry.region_dispatch_count == 3U
                && telemetry.region_readback_count == 3U
                && telemetry.region_readback_bytes == expected_readback
                && telemetry.full_frame_readback_count == 0U
                && telemetry.full_frame_readback_bytes == 0U
                && telemetry.source_buffer_release_count == 1U && telemetry.source_buffer_released
                && telemetry.published && !telemetry.invalidated,
            "one upload and denoise serve repeated bounded regions without a full-frame readback"
        );
        expect(
            metal_source.dcp_applied() == with_dcp
                && metal_source.raw_denoise_receipt().backend
                       == image::RawBayerDenoiseBackend::metal
                && metal_source.demosaic_receipt().algorithm
                       == image::RawDemosaicAlgorithm::bayer_edge_aware_v1,
            "resident execution exposes the actual DCP, denoise, and demosaic provenance"
        );
    }
}

void provider_identity_does_not_change_resident_admission() {
    for (const std::string_view provider : {
             "libraw-canon-cr3",
             "libraw-sony-arw",
             "libraw-nikon-nef",
             "private-nikon-he-provider",
         }) {
        image::RawFrame frame = resident_fixture(std::string(provider));
        auto attempt = publish_resident_owner(
            prepare_resident_owner(std::move(frame), resident_plan(false), neutral_catalog())
        );
        expect(
            attempt.published(),
            "every valid provider-neutral Bayer RawFrame enters the same resident executor"
        );
        if (attempt.published()) {
            const auto& metal_source = attempt.source->metal_source();
            const auto region = metal_source.develop_region({0U, 0U, 3U, 3U});
            expect(
                region.scene_linear.valid()
                    && metal_source.telemetry().denoise_dispatch_count == 0U,
                "provider identity does not invent a denoise dispatch or alter region validity"
            );
        }
    }
}

void every_supported_orientation_uses_level_zero_region_coordinates() {
    const image::GeometryPixelRect region{1U, 1U, 4U, 3U};
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        image::RawFrame frame = resident_fixture();
        frame.descriptor.orientation = orientation;
        const image::RawFrame full_frame = frame;
        auto owner =
            prepare_resident_owner(std::move(frame), resident_plan(false), neutral_catalog());
        auto full = full_metal_reference(full_frame, owner.source.development());
        auto attempt = publish_resident_owner(std::move(owner));
        expect(
            full.development.has_value() && attempt.published(),
            "orientation contract has complete and resident Metal sources"
        );
        if (full.development.has_value() && attempt.published()) {
            const auto developed = attempt.source->metal_source().develop_region(region);
            expect(
                developed.scene_linear.samples
                    == crop_samples(full.development->scene_linear, region),
                "resident region origin remains in oriented level-zero coordinates"
            );
        }
    }
}

void preparation_failure_returns_the_untouched_host_fallback() {
    image::RawFrame frame = resident_fixture();
    image::RawFrame reference_frame = frame;
    auto owner = prepare_resident_owner(std::move(frame), resident_plan(true), neutral_catalog());
    auto attempt = [](PreparedResidentOwner prepared) {
        const ScopedEnvironment forced_failure("SHADOW_TEST_METAL_RESIDENT_FAIL_PREPARE", "1");
        return publish_resident_owner(std::move(prepared));
    }(std::move(owner));
    auto reference_owner =
        prepare_resident_owner(std::move(reference_frame), resident_plan(true), neutral_catalog());
    expect(
        !attempt.published() && attempt.fallback_source.has_value()
            && attempt.diagnostic.find("test-forced") != std::string::npos,
        "pre-publication failure returns the complete owner-bound source transaction"
    );
    if (!attempt.fallback_source.has_value()) {
        return;
    }
    const auto recovered = image::raw_pipeline_detail::materialize_prepared_raw_frame_source(
        std::move(*attempt.fallback_source)
    );
    const auto reference = image::raw_pipeline_detail::materialize_prepared_raw_frame_source(
        std::move(reference_owner.source)
    );
    expect(
        std::get<image::SceneLinearRgbFrame>(recovered.source).samples
                == std::get<image::SceneLinearRgbFrame>(reference.source).samples
            && recovered.pipeline_receipt.pipeline_identity
                   == reference.pipeline_receipt.pipeline_identity,
        "the returned transaction preserves the exact samples and their bound development plan"
    );
}

void forced_metal_preparation_failure_does_not_return_a_cpu_fallback() {
    const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "metal");
    auto owner =
        prepare_resident_owner(resident_fixture(), resident_plan(false), neutral_catalog());
    const ScopedEnvironment forced_failure("SHADOW_TEST_METAL_RESIDENT_FAIL_PREPARE", "1");
    try {
        static_cast<void>(publish_resident_owner(std::move(owner)));
        expect(false, "forced Metal preparation must not return a CPU fallback");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::internal
                && std::string_view(error.what()).find("test-forced") != std::string_view::npos,
            "forced Metal preparation fails closed with the original backend diagnostic"
        );
    }
}

void published_region_failure_invalidates_without_cpu_receipt_substitution() {
    auto attempt = publish_resident_owner(
        prepare_resident_owner(resident_fixture(), resident_plan(false), neutral_catalog())
    );
    expect(attempt.published(), "failure contract begins with a published Metal source");
    if (!attempt.published()) {
        return;
    }
    const auto& metal_source = attempt.source->metal_source();
    {
        const ScopedEnvironment forced_failure("SHADOW_TEST_METAL_RESIDENT_FAIL_REGION", "1");
        try {
            static_cast<void>(metal_source.develop_region({0U, 0U, 3U, 3U}));
            expect(false, "a forced post-publication failure must throw");
        } catch (const image::DecodeError& error) {
            expect(
                error.code() == image::DecodeErrorCode::internal,
                "post-publication Metal failure keeps a typed backend error"
            );
        }
    }
    try {
        static_cast<void>(metal_source.develop_region({0U, 0U, 3U, 3U}));
        expect(false, "an invalidated resident session cannot silently execute on CPU");
    } catch (const image::DecodeError&) {}
    const auto telemetry = metal_source.telemetry();
    expect(
        telemetry.invalidated && telemetry.invalidation_count == 1U
            && telemetry.region_dispatch_count == 0U && telemetry.full_frame_readback_count == 0U,
        "post-publication failure invalidates once without forging CPU work or a full readback"
    );
}

void aggregate_invalidation_reaches_the_published_device_source_once() {
    auto attempt = publish_resident_owner(
        prepare_resident_owner(resident_fixture(), resident_plan(false), neutral_catalog())
    );
    expect(attempt.published(), "aggregate invalidation begins with a published Metal source");
    if (!attempt.published()) {
        return;
    }
    const auto& metal_source = attempt.source->metal_source();
    attempt.source->invalidate_device_path();
    attempt.source->invalidate_device_path();
    bool aggregate_rejected = false;
    bool device_rejected = false;
    try {
        static_cast<void>(attempt.source->metal_source());
    } catch (const image::DecodeError& error) {
        aggregate_rejected = error.code() == image::DecodeErrorCode::internal;
    }
    try {
        static_cast<void>(metal_source.develop_device_region({0U, 0U, 2U, 2U}));
    } catch (const image::DecodeError& error) {
        device_rejected = error.code() == image::DecodeErrorCode::internal;
    }
    const auto telemetry = metal_source.telemetry();
    expect(
        aggregate_rejected && device_rejected && !attempt.source->device_path_valid()
            && !metal_source.valid() && telemetry.invalidated && telemetry.invalidation_count == 1U
            && telemetry.region_dispatch_count == 0U,
        "aggregate invalidation closes cached and retained device access exactly once"
    );
}

void invalid_region_is_rejected_before_the_device_lifecycle() {
    auto attempt = publish_resident_owner(
        prepare_resident_owner(resident_fixture(), resident_plan(false), neutral_catalog())
    );
    expect(attempt.published(), "invalid-request contract begins with a published source");
    if (!attempt.published()) {
        return;
    }
    const auto& metal_source = attempt.source->metal_source();
    try {
        static_cast<void>(metal_source.develop_region({11U, 9U, 2U, 2U}));
        expect(false, "an out-of-bounds region must be rejected");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "region validation keeps a typed request error"
        );
    }
    const auto valid = metal_source.develop_region({0U, 0U, 2U, 2U});
    expect(
        valid.scene_linear.valid() && metal_source.valid()
            && metal_source.telemetry().invalidation_count == 0U,
        "request validation does not invalidate a healthy published device session"
    );
}

void concurrent_regions_use_private_slots_and_completion_fences() {
    image::RawFrame frame = resident_fixture();
    const image::RawFrame full_frame = frame;
    auto owner = prepare_resident_owner(std::move(frame), resident_plan(false), neutral_catalog());
    auto full = full_metal_reference(full_frame, owner.source.development());
    auto attempt = publish_resident_owner(std::move(owner));
    expect(
        full.development.has_value() && attempt.published(),
        "concurrency contract has both complete and resident Metal sources"
    );
    if (!full.development.has_value() || !attempt.published()) {
        return;
    }
    const auto& metal_source = attempt.source->metal_source();

    const std::array<image::GeometryPixelRect, 2U> rects{
        image::GeometryPixelRect{0U, 0U, 6U, 5U},
        image::GeometryPixelRect{5U, 4U, 7U, 6U},
    };
    std::array<std::vector<float>, 2U> outputs;
    std::array<std::thread, 2U> workers{
        std::thread([&] {
            outputs[0] = metal_source.develop_region(rects[0]).scene_linear.samples;
        }),
        std::thread([&] {
            outputs[1] = metal_source.develop_region(rects[1]).scene_linear.samples;
        }),
    };
    for (auto& worker : workers) {
        worker.join();
    }
    const auto telemetry = metal_source.telemetry();
    expect(
        outputs[0] == crop_samples(full.development->scene_linear, rects[0])
            && outputs[1] == crop_samples(full.development->scene_linear, rects[1])
            && telemetry.region_dispatch_count == 2U && telemetry.completed_fence_value >= 2U
            && !telemetry.invalidated,
        "concurrent bounded regions use independent slots and complete behind session fences"
    );
}

void same_descriptor_different_samples_remain_owner_isolated() {
    image::RawFrame first_frame = resident_fixture();
    image::RawFrame second_frame = first_frame;
    for (auto& sample : second_frame.samples) {
        sample = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(static_cast<std::uint32_t>(sample) + 700U, 4'095U)
        );
    }
    auto first_attempt = publish_resident_owner(
        prepare_resident_owner(std::move(first_frame), resident_plan(false), neutral_catalog())
    );
    auto second_attempt = publish_resident_owner(
        prepare_resident_owner(std::move(second_frame), resident_plan(false), neutral_catalog())
    );
    expect(
        first_attempt.published() && second_attempt.published(),
        "same-descriptor owner isolation begins with two independently published sources"
    );
    if (!first_attempt.published() || !second_attempt.published()) {
        return;
    }
    const image::GeometryPixelRect region{0U, 0U, 4U, 4U};
    const auto first =
        first_attempt.source->metal_source().develop_region(region).scene_linear.samples;
    const auto second =
        second_attempt.source->metal_source().develop_region(region).scene_linear.samples;
    expect(
        first != second,
        "same descriptors cannot cause one source's sample-dependent calibration to bind another"
    );
}

void byte_admission_replaces_the_fixed_region_side_limit() {
    auto attempt = publish_resident_owner(
        prepare_resident_owner(wide_resident_fixture(), resident_plan(false), neutral_catalog())
    );
    expect(attempt.published(), "wide-region admission begins with a published source");
    if (!attempt.published()) {
        return;
    }
    const auto& source = attempt.source->metal_source();
    const std::uint64_t before = source.retained_bytes();
    const image::GeometryPixelRect wide_region{0U, 0U, 3'000U, 2U};
    const std::uint64_t region_bytes =
        static_cast<std::uint64_t>(wide_region.width) * wide_region.height * 3U * sizeof(float);
    auto lease = source.develop_device_region(wide_region, before + region_bytes);
    const auto telemetry = source.telemetry();
    expect(
        lease.dimensions() == image::Dimensions{wide_region.width, wide_region.height}
            && telemetry.region_dispatch_count == 1U
            && telemetry.retained_device_bytes == before + region_bytes
            && telemetry.retained_device_bytes == source.retained_bytes(),
        "a width above 2048 succeeds when its actual bytes fit the device and transaction budget"
    );
}

void region_buffer_limit_rejects_before_dispatch() {
    auto attempt = publish_resident_owner(
        prepare_resident_owner(resident_fixture(), resident_plan(false), neutral_catalog())
    );
    expect(attempt.published(), "buffer-limit admission begins with a published source");
    if (!attempt.published()) {
        return;
    }
    const auto& source = attempt.source->metal_source();
    const std::uint64_t retained_before = source.retained_bytes();
    const ScopedEnvironment tiny_buffer_limit(
        "SHADOW_TEST_METAL_RESIDENT_REGION_BUFFER_LIMIT_BYTES",
        "128"
    );
    try {
        static_cast<void>(source.develop_device_region({0U, 0U, 4U, 4U}));
        expect(false, "a region larger than the device buffer limit must be rejected");
    } catch (const image::DecodeError& error) {
        const auto telemetry = source.telemetry();
        expect(
            error.code() == image::DecodeErrorCode::resource_limit
                && telemetry.region_dispatch_count == 0U
                && telemetry.retained_device_bytes == retained_before && source.valid(),
            "device-buffer admission fails before allocation, dispatch, or invalidation"
        );
    }
}

void concurrent_slot_growth_cannot_cross_one_shared_allowance() {
    auto attempt = publish_resident_owner(
        prepare_resident_owner(wide_resident_fixture(), resident_plan(false), neutral_catalog())
    );
    expect(attempt.published(), "concurrent budget admission begins with a published source");
    if (!attempt.published()) {
        return;
    }
    const auto& source = attempt.source->metal_source();
    const std::uint64_t retained_before = source.retained_bytes();
    const image::GeometryPixelRect first_region{0U, 0U, 1'024U, 4U};
    const image::GeometryPixelRect second_region{2'048U, 0U, 1'024U, 4U};
    const std::uint64_t one_region_bytes =
        static_cast<std::uint64_t>(first_region.width) * first_region.height * 3U * sizeof(float);
    const std::uint64_t allowance = retained_before + one_region_bytes;
    std::barrier start(3);
    std::array<std::optional<image::raw_pipeline_detail::MetalResidentRawRegionLease>, 2U> leases;
    std::atomic<std::uint32_t> resource_limit_count{0U};
    std::array<std::thread, 2U> workers{
        std::thread([&] {
            start.arrive_and_wait();
            try {
                leases[0].emplace(source.develop_device_region(first_region, allowance));
            } catch (const image::DecodeError& error) {
                if (error.code() == image::DecodeErrorCode::resource_limit) {
                    resource_limit_count.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        }),
        std::thread([&] {
            start.arrive_and_wait();
            try {
                leases[1].emplace(source.develop_device_region(second_region, allowance));
            } catch (const image::DecodeError& error) {
                if (error.code() == image::DecodeErrorCode::resource_limit) {
                    resource_limit_count.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        }),
    };
    start.arrive_and_wait();
    for (auto& worker : workers) {
        worker.join();
    }
    const std::uint32_t successful_leases = static_cast<std::uint32_t>(leases[0].has_value())
                                            + static_cast<std::uint32_t>(leases[1].has_value());
    const auto telemetry = source.telemetry();
    expect(
        resource_limit_count.load(std::memory_order_relaxed) == 1U && successful_leases <= 1U
            && telemetry.region_dispatch_count <= 1U && telemetry.retained_device_bytes <= allowance
            && telemetry.retained_device_bytes == source.retained_bytes(),
        "two concurrent slot growths serialize against one actual retained-byte allowance"
    );
}

void sequential_slot_replacement_releases_the_old_allocation_before_growth() {
    auto attempt = publish_resident_owner(
        prepare_resident_owner(wide_resident_fixture(), resident_plan(false), neutral_catalog())
    );
    expect(attempt.published(), "sequential replacement begins with a published source");
    if (!attempt.published()) {
        return;
    }
    const auto& source = attempt.source->metal_source();
    const std::uint64_t base_bytes = source.retained_bytes();
    const image::GeometryPixelRect small_region{0U, 0U, 256U, 2U};
    const image::GeometryPixelRect large_region{0U, 0U, 1'024U, 4U};
    const std::uint64_t small_bytes =
        static_cast<std::uint64_t>(small_region.width) * small_region.height * 3U * sizeof(float);
    const std::uint64_t large_bytes =
        static_cast<std::uint64_t>(large_region.width) * large_region.height * 3U * sizeof(float);
    {
        auto lease = source.develop_device_region(small_region, base_bytes + small_bytes);
        expect(
            lease.retained_bytes() == small_bytes,
            "the first request establishes the idle slot that will be replaced"
        );
    }
    const std::uint64_t retained_with_small_slot = source.retained_bytes();
    {
        const ScopedEnvironment forced_failure(
            "SHADOW_TEST_METAL_RESIDENT_FAIL_SLOT_REPLACEMENT",
            "1"
        );
        try {
            static_cast<void>(source.develop_device_region(large_region, base_bytes + large_bytes));
            expect(false, "the replacement failure must interrupt before allocating the new slot");
        } catch (const image::DecodeError& error) {
            expect(
                error.code() == image::DecodeErrorCode::resource_limit,
                "the injected replacement failure retains a typed resource error"
            );
        }
    }
    const auto telemetry = source.telemetry();
    expect(
        retained_with_small_slot == base_bytes + small_bytes
            && telemetry.retained_device_bytes == base_bytes
            && telemetry.region_dispatch_count == 1U && telemetry.invalidated
            && telemetry.invalidation_count == 1U,
        "slot replacement releases and accounts for the old allocation before new allocation"
    );
}

void slot_selection_reuses_the_tightest_fit_then_replaces_the_largest_idle_slot() {
    auto attempt = publish_resident_owner(
        prepare_resident_owner(wide_resident_fixture(), resident_plan(false), neutral_catalog())
    );
    expect(attempt.published(), "slot-selection history begins with a published source");
    if (!attempt.published()) {
        return;
    }
    const auto& source = attempt.source->metal_source();
    const std::uint64_t base_bytes = source.retained_bytes();
    const image::GeometryPixelRect small_region{0U, 0U, 256U, 2U};
    const image::GeometryPixelRect medium_region{0U, 0U, 512U, 2U};
    const image::GeometryPixelRect large_region{0U, 0U, 1'024U, 4U};
    const image::GeometryPixelRect extra_large_region{0U, 0U, 2'048U, 4U};
    const auto region_bytes = [](const image::GeometryPixelRect region) {
        return static_cast<std::uint64_t>(region.width) * region.height * 3U * sizeof(float);
    };
    const std::uint64_t small_bytes = region_bytes(small_region);
    const std::uint64_t medium_bytes = region_bytes(medium_region);
    const std::uint64_t large_bytes = region_bytes(large_region);
    const std::uint64_t extra_large_bytes = region_bytes(extra_large_region);

    std::optional<image::raw_pipeline_detail::MetalResidentRawRegionLease> small_lease;
    small_lease.emplace(
        source.develop_device_region(small_region, base_bytes + small_bytes + large_bytes)
    );
    std::optional<image::raw_pipeline_detail::MetalResidentRawRegionLease> large_lease;
    large_lease.emplace(
        source.develop_device_region(large_region, base_bytes + small_bytes + large_bytes)
    );
    small_lease.reset();
    large_lease.reset();

    const std::uint64_t two_slot_bytes = source.retained_bytes();
    {
        auto medium_lease = source.develop_device_region(medium_region, two_slot_bytes);
        expect(
            medium_lease.retained_bytes() == medium_bytes
                && source.retained_bytes() == two_slot_bytes,
            "a tight allowance reuses the smallest already-adequate idle slot"
        );
    }
    const std::uint64_t replacement_allowance = base_bytes + small_bytes + extra_large_bytes;
    {
        auto extra_large_lease =
            source.develop_device_region(extra_large_region, replacement_allowance);
        expect(
            extra_large_lease.retained_bytes() == extra_large_bytes,
            "growth replaces the largest inadequate idle slot"
        );
    }
    const auto telemetry = source.telemetry();
    expect(
        two_slot_bytes == base_bytes + small_bytes + large_bytes && source.valid()
            && telemetry.invalidation_count == 0U && telemetry.region_dispatch_count == 4U
            && telemetry.retained_device_bytes == replacement_allowance
            && telemetry.retained_device_bytes == source.retained_bytes(),
        "slot history remains reusable and bounded under the exact final allowance"
    );
}

} // namespace

int main() {
    source_binding_rejects_unbound_and_cross_source_optics();
    {
        const ScopedEnvironment acceleration("SHADOW_IMAGE_ACCELERATION", "cpu");
        auto owner =
            prepare_resident_owner(resident_fixture(), resident_plan(false), neutral_catalog());
        try {
            static_cast<void>(publish_resident_owner(std::move(owner)));
            expect(false, "a CPU-prepared plan must not enter the Metal resident transaction");
        } catch (const image::DecodeError& error) {
            expect(
                error.code() == image::DecodeErrorCode::invalid_request,
                "CPU-plan rejection retains a typed request error"
            );
        }
    }
    const ScopedEnvironment automatic_acceleration("SHADOW_IMAGE_ACCELERATION", "auto");
    if (!image::raw_pipeline_detail::metal_resident_raw_source_available()) {
        auto attempt = publish_resident_owner(
            prepare_resident_owner(resident_fixture(), resident_plan(false), neutral_catalog())
        );
        std::cout << "Metal resident RAW source unavailable: " << attempt.diagnostic << '\n';
        if (std::getenv("SHADOW_TEST_REQUIRE_METAL") != nullptr) {
            expect(false, "Metal was required for resident RAW validation");
        }
        return failures == 0 ? 0 : 1;
    }
    repeated_neutral_and_dcp_regions_match_full_metal();
    provider_identity_does_not_change_resident_admission();
    every_supported_orientation_uses_level_zero_region_coordinates();
    preparation_failure_returns_the_untouched_host_fallback();
    forced_metal_preparation_failure_does_not_return_a_cpu_fallback();
    published_region_failure_invalidates_without_cpu_receipt_substitution();
    aggregate_invalidation_reaches_the_published_device_source_once();
    invalid_region_is_rejected_before_the_device_lifecycle();
    concurrent_regions_use_private_slots_and_completion_fences();
    same_descriptor_different_samples_remain_owner_isolated();
    byte_admission_replaces_the_fixed_region_side_limit();
    region_buffer_limit_rejects_before_dispatch();
    concurrent_slot_growth_cannot_cross_one_shared_allowance();
    sequential_slot_replacement_releases_the_old_allocation_before_growth();
    slot_selection_reuses_the_tightest_fit_then_replaces_the_largest_idle_slot();
    return failures == 0 ? 0 : 1;
}
