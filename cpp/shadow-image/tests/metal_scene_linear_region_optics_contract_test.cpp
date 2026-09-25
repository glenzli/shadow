#include "../src/optics/metal_scene_linear_region_optics.hpp"
#include "../src/optics/scene_linear_region_optics.hpp"
#include "../src/raw/metal_raw_development.hpp"
#include "../src/raw/metal_resident_raw_source.hpp"
#include "../src/raw/raw_frame_development_plan.hpp"
#include "../src/raw/raw_frame_source_preparation.hpp"
#include "../src/raw/raw_preview_rebinding.hpp"
#include "../src/raw/resident_raw_source.hpp"
#include "raw_pipeline_routing_test_support.hpp"
#include "scoped_environment.hpp"

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/optics.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace lensfun_plan = image::detail::lensfun_modifier_plan;
using image::test_support::ScopedEnvironment;

[[nodiscard]] image::Dimensions
oriented_dimensions(const image::Dimensions dimensions, const std::int32_t orientation) noexcept {
    return orientation == 5 || orientation == 6
               ? image::Dimensions{dimensions.height, dimensions.width}
               : dimensions;
}

[[nodiscard]] image::RawFrame
resident_fixture(const std::int32_t orientation, const image::Dimensions dimensions = {128U, 96U}) {
    image::RawFrame frame = synthetic_bayer_frame();
    frame.descriptor.provider_id = "generic-open-raw-provider";
    frame.descriptor.provider_version = "provider-v1";
    frame.descriptor.storage_dimensions = dimensions;
    frame.descriptor.active_dimensions = frame.descriptor.storage_dimensions;
    frame.descriptor.orientation = orientation;
    frame.descriptor.bits_per_sample = 12U;
    frame.descriptor.white_levels = {4'095U, 4'095U, 4'095U, 4'095U};
    frame.descriptor.sensor_noise = {
        .schema_version = image::raw_sensor_noise_calibration_schema_version,
        .model = image::RawSensorNoiseModel::poisson_gaussian_per_cfa,
        .source = image::RawSensorNoiseCalibrationSource::provider_calibration_profile,
        .iso_sensitivity = 3'200.0,
        .read_noise_stddev_dn = {18.0, 16.0, 17.0, 20.0},
        .shot_noise_variance_per_dn = {0.06, 0.055, 0.057, 0.065},
    };
    frame.samples.resize(
        static_cast<std::size_t>(frame.descriptor.storage_dimensions.pixel_count())
    );
    for (std::uint32_t y = 0U; y < frame.descriptor.storage_dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < frame.descriptor.storage_dimensions.width; ++x) {
            const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
            const image::RawCfaColor colour = frame.descriptor.bayer_2x2[site];
            const std::uint16_t base = colour == image::RawCfaColor::red     ? 730U
                                       : colour == image::RawCfaColor::green ? 1'270U
                                                                             : 510U;
            const std::uint16_t gradient =
                static_cast<std::uint16_t>((x * 13U + y * 17U + (x * y) % 97U) % 1'300U);
            const int noise = ((x / 2U + y / 2U) & 1U) == 0U ? 29 : -23;
            frame.samples
                [static_cast<std::size_t>(y) * frame.descriptor.storage_dimensions.width + x] =
                static_cast<std::uint16_t>(
                    std::clamp(static_cast<int>(base + gradient) + noise, 0, 4'095)
                );
        }
    }
    return frame;
}

[[nodiscard]] image::RawDevelopmentPlan resident_plan() {
    image::RawDevelopmentPlan plan = image::default_raw_development_plan();
    plan.quality = image::RawDevelopmentQuality::high;
    plan.noise_reduction = image::RawNoiseReductionIntent::noise_robust;
    return plan;
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

[[nodiscard]] image::OpticsSettings optics_settings() {
    image::OpticsSettings settings = image::default_optics_settings();
    settings.manual_vignetting_amount = 37;
    settings.manual_vignetting_midpoint = 43U;
    return settings;
}

class ResidentOpticsRawSession final : public image::DecodeSession {
  public:
    ResidentOpticsRawSession(image::RawFrame frame, image::AssetMetadata metadata) :
        frame_(std::move(frame)), metadata_(std::move(metadata)) {
        metadata_.raw_dimensions = frame_.descriptor.active_dimensions;
        metadata_.image_dimensions =
            oriented_dimensions(frame_.descriptor.active_dimensions, frame_.descriptor.orientation);
        metadata_.orientation = frame_.descriptor.orientation;
        metadata_.iso_speed = 3'200.0;
        capabilities_.metadata = true;
        capabilities_.raw_frame = true;
        capabilities_.reference_rgb = true;
        capabilities_.raw_development = raw_capabilities();
    }

    [[nodiscard]] const image::AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const image::DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const image::PreviewDescriptor> previews() const noexcept override {
        return {};
    }

    [[nodiscard]] image::PreviewPayload decode_preview(std::size_t) override {
        throw image::DecodeError(image::DecodeErrorCode::no_preview, 0, "no preview");
    }

    [[nodiscard]] image::RawFrame decode_raw_frame() override {
        return frame_;
    }

    [[nodiscard]] image::PixelBuffer render_reference_rgb() const override {
        return processed_fallback();
    }

  private:
    image::RawFrame frame_;
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
};

[[nodiscard]] image::CameraProfileCatalog nikon_d850_dcp_catalog() {
    image::CameraProfileCatalog catalog = exact_dcp_catalog();
    auto& definition = catalog.profiles.front();
    definition.normalized_camera_model = "NIKON D850";
    definition.profile.unique_camera_model = "NIKON D850";
    definition.profile.profile_name = "Nikon D850 contract DCP";
    definition.content_identity = "sha256:synthetic-nikon-d850-dcp";
    catalog.identity = "shadow-camera-profile-catalog-v1:nikon-d850-optics-contract";
    return catalog;
}

struct ResidentRawPipeline final {
    std::unique_ptr<image::raw_pipeline_detail::ResidentRawSource> source;
    image::SceneLinearRgbFrame full_scene_linear;
    image::SceneLinearRgbFrame full_corrected;
};

[[nodiscard]] std::optional<ResidentRawPipeline> prepare_resident_raw_pipeline(
    const std::int32_t orientation,
    const std::filesystem::path& database,
    image::AssetMetadata metadata = nikon_d850_metadata()
) {
    image::RawFrame frame = resident_fixture(orientation);
    const image::RawFrame full_frame = frame;
    ResidentOpticsRawSession session(std::move(frame), std::move(metadata));
    auto prepared_source = image::raw_pipeline_detail::prepare_raw_frame_source(
        session,
        resident_plan(),
        std::nullopt,
        nikon_d850_dcp_catalog()
    );
    auto full = full_metal_reference(full_frame, prepared_source.development());
    expect(
        full.development.has_value(),
        "complete Metal RAW reference is available for the optics intersection"
    );
    if (!full.development.has_value()) {
        return std::nullopt;
    }

    auto provider = image::make_lensfun_optics_provider(database);
    expect(provider->info().available, "the pinned Lensfun database is available");
    if (!provider->info().available) {
        return std::nullopt;
    }
    auto prepared = prepared_source.prepare_region_optics(provider.get(), optics_settings());
    expect(
        prepared.kind() == image::detail::SceneLinearRegionOpticsKind::coordinate_remap
            && prepared.has_owned_coordinate_remap() && prepared.receipt().applied_distortion
            && prepared.receipt().applied_tca && prepared.receipt().applied_vignetting
            && prepared.receipt().applied_scaling,
        "C-a supplies distortion, TCA, profile vignette, and autoscale evidence"
    );
    if (!prepared.has_owned_coordinate_remap()) {
        return std::nullopt;
    }
    auto full_corrected = [&] {
        const ScopedEnvironment cpu_oracle("SHADOW_IMAGE_ACCELERATION", "cpu");
        return prepared.lensfun_plan()->correct_scene_linear_reference(
            full.development->scene_linear
        );
    }();
    expect(
        full_corrected.corrected_scene_linear_rgb.has_value(),
        "the owned C-a plan produces the complete CPU optics oracle"
    );
    if (!full_corrected.corrected_scene_linear_rgb.has_value()) {
        return std::nullopt;
    }
    provider.reset();
    auto attempt = image::raw_pipeline_detail::try_prepare_metal_resident_raw_source(
        std::move(prepared_source),
        std::move(prepared)
    );
    expect(
        attempt.published() && attempt.diagnostic.empty(),
        "the source-bound DCP, denoise, and optics plans publish one aggregate resident source"
    );
    if (!attempt.published()) {
        return std::nullopt;
    }
    return ResidentRawPipeline{
        .source = std::move(attempt.source),
        .full_scene_linear = std::move(full.development->scene_linear),
        .full_corrected = std::move(*full_corrected.corrected_scene_linear_rgb),
    };
}

[[nodiscard]] image::SceneLinearRgbFrame
crop(const image::SceneLinearRgbFrame& source, const image::GeometryPixelRect rect) {
    image::SceneLinearRgbFrame result{
        .dimensions = {rect.width, rect.height},
        .row_stride_bytes = static_cast<std::size_t>(rect.width) * 3U * sizeof(float),
        .samples = std::vector<float>(static_cast<std::size_t>(rect.width) * rect.height * 3U),
    };
    for (std::uint32_t y = 0U; y < rect.height; ++y) {
        const std::size_t source_first =
            (static_cast<std::size_t>(rect.y + y) * source.dimensions.width + rect.x) * 3U;
        const std::size_t output_first = static_cast<std::size_t>(y) * rect.width * 3U;
        std::copy_n(
            source.samples.begin() + static_cast<std::ptrdiff_t>(source_first),
            static_cast<std::size_t>(rect.width) * 3U,
            result.samples.begin() + static_cast<std::ptrdiff_t>(output_first)
        );
    }
    return result;
}

[[nodiscard]] image::SceneLinearRgbFrame render_region_cpu(
    const image::SceneLinearRgbFrame& full_scene_linear,
    const std::shared_ptr<const lensfun_plan::LensfunModifierPlan>& plan,
    const lensfun_plan::PreparedRegion& region
) {
    image::SceneLinearRgbFrame preimage;
    if (region.source_preimage().has_value()) {
        preimage = crop(full_scene_linear, *region.source_preimage());
    }
    return plan->render_scene_linear_region_cpu(preimage, region);
}

[[nodiscard]] float
maximum_difference(const std::span<const float> left, const std::span<const float> right) {
    float maximum = 0.0F;
    const std::size_t count = std::min(left.size(), right.size());
    for (std::size_t index = 0U; index < count; ++index) {
        maximum = std::max(maximum, std::abs(left[index] - right[index]));
    }
    return maximum;
}

void expect_near(
    const image::SceneLinearRgbFrame& actual,
    const image::SceneLinearRgbFrame& expected,
    const std::string_view contract
) {
    constexpr float tolerance = 2.0e-4F;
    const float difference = maximum_difference(actual.samples, expected.samples);
    expect(
        actual.dimensions == expected.dimensions && actual.samples.size() == expected.samples.size()
            && difference <= tolerance,
        contract
    );
    if (difference > tolerance) {
        const auto maximum_absolute = [](const std::span<const float> values) {
            float result = 0.0F;
            for (const float value : values) {
                result = std::max(result, std::abs(value));
            }
            return result;
        };
        std::cerr << "maximum difference: " << difference
                  << "; actual magnitude: " << maximum_absolute(actual.samples)
                  << "; expected magnitude: " << maximum_absolute(expected.samples) << '\n';
    }
}

[[nodiscard]] image::detail::MetalSceneLinearRegionLease execute_region(
    const image::raw_pipeline_detail::ResidentRawSource& source,
    const image::GeometryPixelRect output_rect
) {
    auto region = source.region_optics().prepare_region(output_rect);
    return image::detail::develop_metal_scene_linear_region_optics(
        source,
        std::move(region),
        std::numeric_limits<std::uint64_t>::max()
    );
}

void irregular_tiles_match_and_nominal_path_has_zero_readback(
    const std::filesystem::path& database
) {
    auto raw = prepare_resident_raw_pipeline(0, database);
    if (!raw.has_value()) {
        return;
    }
    const auto plan = raw->source->region_optics().lensfun_plan();
    expect(plan != nullptr, "the resident aggregate retains its exact C-a Lensfun plan");
    if (plan == nullptr) {
        return;
    }
    const std::vector<image::GeometryPixelRect> tiles{
        {0U, 0U, 31U, 17U},
        {31U, 0U, 97U, 17U},
        {0U, 17U, 53U, 28U},
        {53U, 17U, 75U, 28U},
        {0U, 45U, 19U, 51U},
        {19U, 45U, 109U, 51U},
    };

    for (const auto tile : tiles) {
        auto region = raw->source->region_optics().prepare_region(tile);
        expect(
            region.source_preimage().has_value() && region.coordinate_remap()
                && region.profile_vignetting(),
            "every representative tile has an exact C-a profile preimage"
        );
        if (!region.source_preimage().has_value()) {
            continue;
        }
        const auto region_expected = render_region_cpu(raw->full_scene_linear, plan, region);
        const auto expected = crop(raw->full_corrected, tile);
        expect_near(
            region_expected,
            expected,
            "C-a region evidence remains numerically identical to its complete optics oracle"
        );
        auto output = image::detail::develop_metal_scene_linear_region_optics(
            *raw->source,
            std::move(region),
            std::numeric_limits<std::uint64_t>::max()
        );
        const auto before_readback = output.telemetry();
        const auto source_before_readback = raw->source->metal_source().telemetry();
        expect(
            output.valid() && output.device_identity() != 0U && output.completion_fence_value() > 0U
                && before_readback.source_completion_fence_value > 0U
                && before_readback.source_slot_pinned_through_completion
                && before_readback.source_reupload_count == 0U
                && before_readback.source_fp32_readback_count == 0U
                && before_readback.optics_dispatch_count == 1U
                && before_readback.coordinate_upload_count == 1U
                && before_readback.profile_gain_upload_count == 1U
                && before_readback.debug_readback_count == 0U,
            "normal RAW-to-optics execution stays on one device without materialization"
        );
        expect(
            source_before_readback.source_upload_count == 1U
                && source_before_readback.region_readback_count == 0U
                && source_before_readback.full_frame_readback_count == 0U
                && source_before_readback.active_region_lease_count == 0U
                && source_before_readback.region_lease_count
                       == source_before_readback.region_lease_release_count,
            "the RAW slot releases only after optics completion and never reaches the host"
        );

        const auto actual = output.debug_readback();
        expect_near(
            actual,
            expected,
            "irregular Metal tiles match the stable C-a Lensfun/DCP/denoise region oracle"
        );
        const auto after_readback = output.telemetry();
        expect(
            after_readback.debug_readback_count == 1U
                && after_readback.debug_readback_bytes == output.retained_bytes()
                && raw->source->metal_source().telemetry().region_readback_count == 0U,
            "only the explicit output oracle performs a counted fp32 readback"
        );
    }
    const auto telemetry = raw->source->metal_source().telemetry();
    expect(
        telemetry.source_upload_count == 1U && telemetry.denoise_dispatch_count == 1U
            && telemetry.region_dispatch_count == tiles.size()
            && telemetry.region_readback_count == 0U && telemetry.full_frame_readback_count == 0U,
        "all irregular tiles reuse one uploaded and denoised resident CFA"
    );
}

void rebindable_preview_full_frame_stays_resident_through_optics(
    const std::filesystem::path& database,
    const bool strict_source_subset = false,
    const bool profile_vignetting = true
) {
    image::RawFrame frame = resident_fixture(
        0,
        strict_source_subset ? image::Dimensions{1536U, 1024U} : image::Dimensions{128U, 96U}
    );
    image::AssetMetadata metadata = nikon_d850_metadata();
    if (strict_source_subset) {
        metadata.make = metadata.normalized_make = "Canon";
        metadata.model = metadata.normalized_model =
            profile_vignetting ? "EOS 5D Mark II" : "EOS R";
        metadata.lens_make = "Canon";
        metadata.lens_model =
            profile_vignetting ? "Canon EF 24-105mm f/4L IS USM" : "Canon RF 24-105mm F4L IS USM";
        metadata.focal_length_mm = metadata.focal_length_35mm = 105.0;
        metadata.aperture_f_number = 5.6;
    }
    ResidentOpticsRawSession session(std::move(frame), metadata);
    auto prepared = image::raw_pipeline_detail::prepare_raw_frame_source(
        session,
        resident_plan(),
        std::nullopt,
        nikon_d850_dcp_catalog()
    );
    auto rebindable =
        image::raw_pipeline_detail::prepare_raw_preview_rebinding(std::move(prepared));
    const auto* full = std::get_if<image::SceneLinearRgbFrame>(&rebindable.developed.source);
    expect(
        full != nullptr,
        "the preview-rebind fixture retains one complete scene-linear CPU oracle"
    );
    if (full == nullptr) {
        return;
    }
    const auto resident = rebindable.source->try_bind_metal_resident(resident_plan());
    expect(
        resident.has_value(),
        "the rebindable preview develops one same-device scene-linear RGB buffer"
    );
    if (!resident.has_value()) {
        return;
    }
    auto provider = image::make_lensfun_optics_provider(database);
    auto settings = optics_settings();
    settings.correct_vignetting = profile_vignetting;
    auto optics = image::detail::prepare_scene_linear_region_optics(
        provider.get(),
        resident->output.dimensions(),
        metadata,
        settings
    );
    const image::GeometryPixelRect full_region{
        0U,
        0U,
        resident->output.dimensions().width,
        resident->output.dimensions().height,
    };
    auto region = optics.prepare_region(full_region);
    expect(
        optics.device_resident_eligible() && region.source_preimage().has_value(),
        "the full preview carries owned optics evidence"
    );
    if (!optics.device_resident_eligible() || !region.source_preimage().has_value()) {
        return;
    }
    expect(
        (*region.source_preimage() != full_region) == strict_source_subset,
        "telephoto correction samples a strict subset of the resident full image"
    );
    expect(
        region.profile_vignetting() == profile_vignetting,
        "the fixture exercises both compact source-aligned gains and geometry-only sampling"
    );
    const auto expected_result = optics.lensfun_plan()->correct_scene_linear_reference(*full);
    expect(
        expected_result.corrected_scene_linear_rgb.has_value(),
        "the full preview retains the CPU optics oracle for numerical comparison"
    );
    if (!expected_result.corrected_scene_linear_rgb.has_value()) {
        return;
    }
    auto corrected = image::detail::apply_metal_scene_linear_preview_optics(
        resident->output,
        optics,
        std::move(region)
    );
    const auto telemetry = corrected.telemetry();
    expect(
        corrected.valid() && telemetry.source_reupload_count == 0U
            && telemetry.source_fp32_readback_count == 0U && telemetry.optics_dispatch_count == 1U
            && telemetry.source_slot_pinned_through_completion == false,
        "a RAW rebind continues through full-preview optics without host materialization"
    );
    expect_near(
        corrected.debug_readback(),
        *expected_result.corrected_scene_linear_rgb,
        "resident rebind optics matches the same full-preview Lensfun oracle"
    );
}

void every_orientation_matches_the_complete_oracle(const std::filesystem::path& database) {
    for (const std::int32_t orientation : {0, 3, 5, 6}) {
        auto raw = prepare_resident_raw_pipeline(orientation, database);
        if (!raw.has_value()) {
            continue;
        }
        const auto plan = raw->source->region_optics().lensfun_plan();
        if (plan == nullptr) {
            expect(false, "every orientation retains the prepared Lensfun plan");
            continue;
        }
        const image::Dimensions dimensions = oriented_dimensions({128U, 96U}, orientation);
        const image::GeometryPixelRect tile{
            .x = dimensions.width / 5U,
            .y = dimensions.height / 6U,
            .width = dimensions.width / 2U + 3U,
            .height = dimensions.height / 3U + 5U,
        };
        const auto region_expected = render_region_cpu(
            raw->full_scene_linear,
            plan,
            raw->source->region_optics().prepare_region(tile)
        );
        const auto expected = crop(raw->full_corrected, tile);
        expect_near(
            region_expected,
            expected,
            "every orientation keeps C-a region evidence identical to its complete oracle"
        );
        auto output = execute_region(*raw->source, tile);
        expect_near(
            output.debug_readback(),
            expected,
            "Metal intersection preserves the complete orientation contract"
        );
        expect(
            raw->source->metal_source().telemetry().region_readback_count == 0U,
            "orientation execution does not materialize the RAW preimage"
        );
    }
}

void output_survives_source_plan_and_provider_destruction(const std::filesystem::path& database) {
    std::optional<image::detail::MetalSceneLinearRegionLease> retained;
    image::SceneLinearRgbFrame expected;
    {
        auto raw = prepare_resident_raw_pipeline(0, database);
        if (!raw.has_value()) {
            return;
        }
        const image::GeometryPixelRect tile{17U, 13U, 61U, 37U};
        expected = crop(raw->full_corrected, tile);
        retained.emplace(execute_region(*raw->source, tile));
    }
    expect(
        retained.has_value() && retained->valid(),
        "the output lease owns its device resource after every preparation owner is destroyed"
    );
    if (retained.has_value()) {
        expect_near(
            retained->debug_readback(),
            expected,
            "the retained output remains numerically complete after owner destruction"
        );
    }
}

void concurrent_regions_preserve_independent_outputs(const std::filesystem::path& database) {
    auto raw = prepare_resident_raw_pipeline(0, database);
    if (!raw.has_value()) {
        return;
    }
    const std::array<image::GeometryPixelRect, 2U> tiles{
        image::GeometryPixelRect{0U, 0U, 55U, 41U},
        image::GeometryPixelRect{63U, 47U, 65U, 49U},
    };
    std::array<std::optional<image::detail::MetalSceneLinearRegionLease>, 2U> outputs;
    std::barrier both_regions_ready(2);
    std::array<std::thread, 2U> workers{
        std::thread([&] {
            auto region = raw->source->region_optics().prepare_region(tiles[0]);
            both_regions_ready.arrive_and_wait();
            outputs[0].emplace(
                image::detail::develop_metal_scene_linear_region_optics(
                    *raw->source,
                    std::move(region),
                    std::numeric_limits<std::uint64_t>::max()
                )
            );
        }),
        std::thread([&] {
            auto region = raw->source->region_optics().prepare_region(tiles[1]);
            both_regions_ready.arrive_and_wait();
            outputs[1].emplace(
                image::detail::develop_metal_scene_linear_region_optics(
                    *raw->source,
                    std::move(region),
                    std::numeric_limits<std::uint64_t>::max()
                )
            );
        }),
    };
    for (auto& worker : workers) {
        worker.join();
    }
    const auto telemetry = raw->source->metal_source().telemetry();
    expect(
        telemetry.region_dispatch_count == outputs.size()
            && telemetry.active_region_lease_count == 0U
            && telemetry.region_lease_count == telemetry.region_lease_release_count
            && telemetry.region_readback_count == 0U,
        "concurrent optics commands balance their owner-bound RAW leases without readback"
    );
    for (std::size_t index = 0U; index < outputs.size(); ++index) {
        expect(outputs[index].has_value(), "each concurrent optics command publishes an output");
        if (outputs[index].has_value()) {
            expect_near(
                outputs[index]->debug_readback(),
                crop(raw->full_corrected, tiles[index]),
                "concurrent Metal region output matches the stable C-a region oracle"
            );
        }
    }
}

void cross_lens_evidence_is_rejected_before_raw_admission(const std::filesystem::path& database) {
    auto raw = prepare_resident_raw_pipeline(0, database);
    if (!raw.has_value()) {
        return;
    }
    auto provider = image::make_lensfun_optics_provider(database);
    auto metadata = nikon_d850_metadata();
    const auto candidates = provider->profile_candidates(metadata);
    std::optional<image::detail::PreparedSceneLinearRegionOptics> other_owner;
    for (const auto& candidate : candidates) {
        metadata.lens_make = candidate.lens_maker;
        metadata.lens_model = candidate.lens_model;
        auto candidate_owner = image::detail::prepare_scene_linear_region_optics(
            provider.get(),
            raw->source->dimensions(),
            metadata,
            optics_settings()
        );
        if (candidate_owner.has_owned_coordinate_remap()
            && candidate_owner.receipt().lens_profile
                   != raw->source->optics_receipt().lens_profile) {
            other_owner.emplace(std::move(candidate_owner));
            break;
        }
    }
    expect(
        other_owner.has_value(),
        "the pinned catalog supplies a second concrete lens owner for C-c rejection"
    );
    if (!other_owner.has_value()) {
        return;
    }

    auto region = other_owner->prepare_region({11U, 7U, 43U, 29U});
    expect(
        !raw->source->region_optics().owns_region(region),
        "same dimensions and settings do not make another lens plan's evidence transferable"
    );
    const auto before = raw->source->metal_source().telemetry();
    bool threw = false;
    try {
        static_cast<void>(image::detail::develop_metal_scene_linear_region_optics(
            *raw->source,
            std::move(region),
            std::numeric_limits<std::uint64_t>::max()
        ));
    } catch (const image::DecodeError& error) {
        threw = error.code() == image::DecodeErrorCode::invalid_request;
    }
    const auto after = raw->source->metal_source().telemetry();
    expect(
        threw && raw->source->device_path_valid() && !after.invalidated
            && after.invalidation_count == before.invalidation_count
            && after.region_dispatch_count == before.region_dispatch_count
            && after.region_lease_count == before.region_lease_count
            && after.region_lease_release_count == before.region_lease_release_count
            && after.region_readback_count == 0U,
        "cross-lens evidence is rejected before any C-b lease, dispatch, or invalidation"
    );
}

void admitted_failure_invalidates_without_cpu_receipt(const std::filesystem::path& database) {
    auto raw = prepare_resident_raw_pipeline(0, database);
    if (!raw.has_value()) {
        return;
    }
    const auto& metal_source = raw->source->metal_source();
    auto region = raw->source->region_optics().prepare_region({23U, 19U, 47U, 31U});
    bool threw = false;
    {
        const ScopedEnvironment failure("SHADOW_TEST_METAL_SCENE_LINEAR_OPTICS_FAIL", "1");
        try {
            static_cast<void>(image::detail::develop_metal_scene_linear_region_optics(
                *raw->source,
                std::move(region),
                std::numeric_limits<std::uint64_t>::max()
            ));
        } catch (const image::DecodeError&) {
            threw = true;
        }
    }
    const auto telemetry = metal_source.telemetry();
    expect(
        threw && !raw->source->device_path_valid() && telemetry.invalidated
            && telemetry.invalidation_count == 1U && telemetry.region_readback_count == 0U
            && telemetry.active_region_lease_count == 0U,
        "an admitted Metal optics failure invalidates once without CPU substitution"
    );
}

void non_metal_stub_contract() {
    expect(
        !image::detail::metal_scene_linear_region_optics_available()
            && !image::detail::metal_scene_linear_region_optics_diagnostic().empty(),
        "the non-Metal executor publishes an explicit unavailable diagnostic"
    );
}

} // namespace

int main() {
    if (!image::raw_pipeline_detail::metal_resident_raw_source_available()
        || !image::detail::metal_scene_linear_region_optics_available()) {
        if (std::getenv("SHADOW_TEST_REQUIRE_METAL") != nullptr) {
            std::cerr << "FAILED: required Metal region optics is unavailable: "
                      << image::detail::metal_scene_linear_region_optics_diagnostic() << '\n';
            return 1;
        }
        non_metal_stub_contract();
        return failures == 0 ? 0 : 1;
    }
    const char* database = std::getenv("SHADOW_TEST_LENSFUN_DB");
    if (database == nullptr || *database == '\0') {
        if (std::getenv("SHADOW_TEST_REQUIRE_METAL") != nullptr) {
            std::cerr << "FAILED: required Lensfun contract database is unavailable\n";
            return 1;
        }
        return 0;
    }
    const std::filesystem::path database_path(database);
    irregular_tiles_match_and_nominal_path_has_zero_readback(database_path);
    rebindable_preview_full_frame_stays_resident_through_optics(database_path);
    rebindable_preview_full_frame_stays_resident_through_optics(database_path, true, true);
    rebindable_preview_full_frame_stays_resident_through_optics(database_path, true, false);
    every_orientation_matches_the_complete_oracle(database_path);
    output_survives_source_plan_and_provider_destruction(database_path);
    concurrent_regions_preserve_independent_outputs(database_path);
    cross_lens_evidence_is_rejected_before_raw_admission(database_path);
    admitted_failure_invalidates_without_cpu_receipt(database_path);
    return failures == 0 ? 0 : 1;
}
