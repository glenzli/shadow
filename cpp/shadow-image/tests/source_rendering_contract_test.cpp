#include <shadow/image/edit.hpp>
#include <shadow/image/source_rendering.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void expect_close(
    const double actual,
    const double expected,
    const double tolerance,
    const std::string_view message
) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "FAILED: " << message << " (actual=" << actual
                  << ", expected=" << expected << ")\n";
        ++failures;
    }
}

[[nodiscard]] image::PixelBuffer raw_rgb(const std::uint16_t value) {
    return image::PixelBuffer{
        .dimensions = {2U, 1U},
        .bits_per_channel = 16U,
        .channels = 3U,
        .row_stride_bytes = 6U * sizeof(std::uint16_t),
        .primaries = image::RgbPrimaries::srgb_rec709_d65,
        .transfer_function = image::RgbTransferFunction::linear,
        .reference = image::RgbBufferReference::processed_raw,
        .samples = {value, value, value, value, value, value},
    };
}

[[nodiscard]] image::FloatRgbImage working_rgb(const float value) {
    return image::FloatRgbImage{
        .dimensions = {2U, 1U},
        .row_stride_bytes = 6U * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = image::ImageReference::scene_referred,
        .samples = {value, value, value, value, value, value},
    };
}

[[nodiscard]] image::FloatRgbImage working_rgb(const float red, const float green, const float blue) {
    return image::FloatRgbImage{
        .dimensions = {1U, 1U},
        .row_stride_bytes = 3U * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = image::ImageReference::scene_referred,
        .samples = {red, green, blue},
    };
}

[[nodiscard]] image::SceneLinearRgbFrame scene_linear_rgb(const float value) {
    return image::SceneLinearRgbFrame{
        .dimensions = {2U, 1U},
        .row_stride_bytes = 6U * sizeof(float),
        .samples = {value, value, value, value, value, value},
    };
}

[[nodiscard]] image::SourceProfileCatalog empty_catalog() {
    return image::SourceProfileCatalog{.identity = "test-empty-catalog"};
}

[[nodiscard]] image::RawPipelineReceipt applied_dcp_pipeline() {
    image::RawPipelineReceipt receipt;
    receipt.path = image::RawPipelinePath::shadow_raw_frame;
    receipt.pipeline_identity = "test-shadow-raw+dcp";
    receipt.source_provider_id = "test-provider";
    receipt.source_provider_version = "1";
    receipt.raw_frame_schema_version = image::raw_frame_schema_version;
    receipt.raw_developer_version = image::shadow_raw_frame_developer_version;
    receipt.source_scene_luminance_percentile = 1.5;
    receipt.requested_plan = image::default_raw_development_plan();
    receipt.effective_plan = receipt.requested_plan;
    receipt.camera_profile_status = image::RawCameraProfileStatus::applied;
    receipt.camera_profile_catalog_identity = "test-camera-profile-catalog";
    receipt.camera_profile_identity = "sha256:test-dcp";
    receipt.camera_profile_name = "Test DCP";
    receipt.camera_profile_developer_version = 1U;
    return receipt;
}

void test_standard_normalizes_non_dng_raw() {
    const auto source = raw_rgb(16'384U);
    const image::SourceRenderingReceipt receipt = image::resolve_source_rendering(
        source,
        {},
        empty_catalog()
    );
    expect(
        receipt.kind == image::SourceRenderingKind::shadow_standard,
        "processed RAW resolves through Shadow Standard"
    );
    expect(
        receipt.profile_id == "shadow-standard",
        "Shadow Standard identifies its public profile"
    );
    expect_close(
        receipt.camera_baseline_exposure_stops,
        0.0,
        1.0e-12,
        "non-DNG raw does not invent a DNG baseline"
    );
    expect_close(
        receipt.standard_exposure_normalization_stops,
        1.0,
        1.0e-12,
        "Shadow Standard's lift remains bounded at one stop"
    );
    auto working = working_rgb(0.25F);
    image::apply_source_rendering(working, receipt);
    expect_close(
        working.samples.front(),
        0.5,
        1.0e-6,
        "the source receipt applies before the editable graph"
    );
    expect(
        receipt.profile_identity == "shadow-standard-v1"
            && image::source_rendering_identity(receipt)
                == image::source_rendering_identity(receipt),
        "profile and complete source-render identities are stable and cache-safe"
    );
}

void test_dng_baseline_wins_over_generic_normalization() {
    const auto source = raw_rgb(8'192U);
    image::AssetMetadata metadata;
    metadata.dng_version = "1.6.0.0";
    metadata.baseline_exposure = -0.5;
    const image::SourceRenderingReceipt receipt = image::resolve_source_rendering(
        source,
        metadata,
        empty_catalog()
    );
    expect_close(
        receipt.camera_baseline_exposure_stops,
        -0.5,
        1.0e-12,
        "valid DNG BaselineExposure remains source calibration"
    );
    expect_close(
        receipt.standard_exposure_normalization_stops,
        0.0,
        1.0e-12,
        "generic normalization does not override DNG calibration"
    );
    auto working = working_rgb(0.5F);
    image::apply_source_rendering(working, receipt);
    expect_close(working.samples.front(), 0.5 / std::sqrt(2.0), 1.0e-6, "DNG baseline applies");
}

void test_rendered_raster_remains_untouched() {
    auto source = raw_rgb(16'384U);
    source.reference = image::RgbBufferReference::decoded_raster;
    const image::SourceRenderingReceipt receipt = image::resolve_source_rendering(
        source,
        {},
        empty_catalog()
    );
    expect(
        receipt.kind == image::SourceRenderingKind::embedded_rendering,
        "JPEG and HEIF keep their embedded rendering"
    );
    auto working = working_rgb(0.25F);
    working.reference = image::ImageReference::display_referred;
    image::apply_source_rendering(working, receipt);
    expect_close(working.samples.front(), 0.25, 1.0e-6, "rendered raster is unchanged");
}

void test_public_profile_document_matches_exact_camera_without_vendor_data() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path()
        / "shadow-source-profile-contract-v1";
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    std::filesystem::create_directories(directory, cleanup_error);
    expect(!cleanup_error, "source profile test directory is available");
    {
        std::ofstream profile(directory / "open-camera.shadow-source-profile");
        profile << "shadow-source-profile-v1\n"
                << "id = open-camera-standard\n"
                << "name = Open Camera Standard\n"
                << "make = Open Camera\n"
                << "model = Mk I\n"
                << "exposure_stops = 0.5\n"
                << "tone_curve = 0:0, 0.5:0.6, 1:1\n";
    }

    const image::SourceProfileCatalog catalog = image::load_source_profile_catalog(directory);
    expect(catalog.profiles.size() == 1U, "public source profile document loads from one directory");
    image::AssetMetadata metadata;
    metadata.make = "Open   Camera";
    metadata.model = "MK i";
    const auto matched = image::match_source_profile(catalog, metadata);
    expect(
        matched.has_value() && matched->id == "open-camera-standard",
        "profile matching is exact but whitespace/case normalized"
    );
    const image::SourceRenderingReceipt receipt = image::resolve_source_rendering(
        raw_rgb(16'384U),
        metadata,
        catalog
    );
    expect(
        receipt.kind == image::SourceRenderingKind::public_profile
            && receipt.profile_id == "open-camera-standard",
        "matched public profile replaces only the generic source fallback"
    );
    expect_close(
        receipt.profile_exposure_stops,
        0.5,
        1.0e-12,
        "profile exposure is retained as source calibration"
    );
    expect(
        receipt.standard_exposure_normalization_stops == 0.0,
        "exact profile does not stack a hidden generic normalization"
    );
    expect(
        receipt.luminance_tone_curve.size() == 3U,
        "public profile's optional source luminance curve is retained"
    );
    std::filesystem::remove_all(directory, cleanup_error);
}

void test_bundled_darktable_camera_looks_match_make_and_preserve_chroma() {
    const image::SourceProfileCatalog catalog = image::load_builtin_source_profile_catalog();
    image::AssetMetadata metadata;
    metadata.normalized_make = "Nikon";
    metadata.normalized_model = "Z 9";
    const image::SourceRenderingReceipt receipt = image::resolve_source_rendering(
        raw_rgb(16'384U),
        metadata,
        catalog
    );
    expect(
        receipt.kind == image::SourceRenderingKind::public_profile
            && receipt.profile_id == "darktable-nikon-like",
        "bundled darktable Nikon look is a maker fallback"
    );
    expect(
        receipt.luminance_tone_curve.size() == 6U,
        "bundled darktable maker look carries its published curve samples"
    );
    expect_close(
        receipt.standard_exposure_normalization_stops,
        1.0,
        1.0e-12,
        "generic maker curve does not suppress Shadow Standard's source-exposure fallback"
    );

    auto working = working_rgb(0.20F, 0.40F, 0.10F);
    const double prior_red_to_green = static_cast<double>(working.samples[0]) / working.samples[1];
    const double prior_green_to_blue = static_cast<double>(working.samples[1]) / working.samples[2];
    image::apply_source_rendering(working, receipt);
    expect(
        working.samples[1] > 0.40F,
        "Nikon-like base curve raises a mid-tone luminance sample"
    );
    expect_close(
        static_cast<double>(working.samples[0]) / working.samples[1],
        prior_red_to_green,
        1.0e-6,
        "source curve scales RGB together instead of shifting hue"
    );
    expect_close(
        static_cast<double>(working.samples[1]) / working.samples[2],
        prior_green_to_blue,
        1.0e-6,
        "source curve preserves the RGB chroma ratio"
    );
}

void test_applied_dcp_suppresses_generic_camera_look_but_keeps_dng_baseline() {
    const image::SourceProfileCatalog catalog = image::load_builtin_source_profile_catalog();
    image::AssetMetadata metadata;
    metadata.normalized_make = "Nikon";
    metadata.normalized_model = "Z 9";
    metadata.dng_version = "1.6.0.0";
    metadata.baseline_exposure = 0.25;
    const auto pipeline = applied_dcp_pipeline();
    expect(pipeline.valid(), "synthetic DCP pipeline receipt is valid");
    const image::SourceRenderingReceipt receipt = image::resolve_source_rendering(
        raw_rgb(16'384U),
        metadata,
        pipeline,
        catalog
    );
    expect(
        receipt.kind == image::SourceRenderingKind::shadow_standard
            && receipt.profile_id == "shadow-standard-dcp",
        "applied DCP selects neutral Shadow Standard instead of a maker curve"
    );
    expect(
        receipt.luminance_tone_curve.empty() && receipt.profile_exposure_stops == 0.0,
        "camera-specific public profile is not stacked after DCP color development"
    );
    expect_close(
        receipt.camera_baseline_exposure_stops,
        0.25,
        1.0e-12,
        "source DNG BaselineExposure remains distinct from DCP BaselineExposureOffset"
    );
    expect(
        receipt.profile_identity.find(pipeline.camera_profile_identity) != std::string::npos,
        "source-render identity retains the applied DCP content identity"
    );

    auto rejected = pipeline;
    rejected.camera_profile_status = image::RawCameraProfileStatus::matched_not_applied;
    rejected.camera_profile_diagnostic = "unsupported profile table";
    const auto generic = image::resolve_source_rendering(
        raw_rgb(16'384U),
        metadata,
        rejected,
        catalog
    );
    expect(
        generic.kind == image::SourceRenderingKind::public_profile
            && generic.profile_id == "darktable-nikon-like",
        "fail-closed DCP leaves the existing generic source-rendering path available"
    );
}

void test_scene_linear_raw_keeps_super_white_samples_before_output_mapping() {
    const auto pipeline = applied_dcp_pipeline();
    const auto source = scene_linear_rgb(1.5F);
    const image::SourceRenderingReceipt receipt = image::resolve_source_rendering(
        source,
        {},
        pipeline
    );
    expect_close(
        receipt.standard_exposure_normalization_stops,
        0.0,
        1.0e-12,
        "scene-linear normalization observes super-white RAW samples instead of a clipped proxy"
    );
    auto working = working_rgb(1.5F);
    image::apply_source_rendering(working, receipt);
    expect_close(
        working.samples.front(),
        1.5,
        1.0e-6,
        "source rendering does not clip headroom before the edit/output graph"
    );
}

void test_source_profile_curve_handoffs_smoothly_to_scene_linear_highlights() {
    image::SourceRenderingReceipt receipt;
    receipt.profile_id = "test-hdr-handoff";
    receipt.profile_identity = "test-hdr-handoff-v1";
    receipt.kind = image::SourceRenderingKind::public_profile;
    receipt.luminance_tone_curve = {
        {0.0, 0.0}, {0.5, 0.72}, {1.0, 1.0},
    };

    auto just_below_white = working_rgb(0.999F);
    auto just_above_white = working_rgb(1.001F);
    auto handoff_end = working_rgb(1.25F);
    auto retained_highlight = working_rgb(1.75F, 0.875F, 0.4375F);
    image::apply_source_rendering(just_below_white, receipt);
    image::apply_source_rendering(just_above_white, receipt);
    image::apply_source_rendering(handoff_end, receipt);
    image::apply_source_rendering(retained_highlight, receipt);

    expect(
        std::isfinite(just_above_white.samples.front())
            && just_above_white.samples.front() > 1.0F,
        "a profile curve keeps immediately super-white scene-linear detail finite and recoverable"
    );
    expect(
        just_above_white.samples.front() >= just_below_white.samples.front()
            && just_above_white.samples.front() - just_below_white.samples.front() < 0.01F,
        "profile curve hands off continuously around display white instead of introducing a highlight seam"
    );
    expect_close(
        handoff_end.samples.front(),
        1.25,
        1.0e-6,
        "profile curve returns to identity after its bounded HDR handoff"
    );
    expect_close(
        static_cast<double>(retained_highlight.samples[0]) / retained_highlight.samples[1],
        2.0,
        1.0e-6,
        "profile HDR handoff preserves red-to-green scene-linear chroma"
    );
    expect_close(
        static_cast<double>(retained_highlight.samples[1]) / retained_highlight.samples[2],
        2.0,
        1.0e-6,
        "profile HDR handoff preserves green-to-blue scene-linear chroma"
    );
}

} // namespace

int main() {
    test_standard_normalizes_non_dng_raw();
    test_dng_baseline_wins_over_generic_normalization();
    test_rendered_raster_remains_untouched();
    test_public_profile_document_matches_exact_camera_without_vendor_data();
    test_bundled_darktable_camera_looks_match_make_and_preserve_chroma();
    test_applied_dcp_suppresses_generic_camera_look_but_keeps_dng_baseline();
    test_scene_linear_raw_keeps_super_white_samples_before_output_mapping();
    test_source_profile_curve_handoffs_smoothly_to_scene_linear_highlights();
    return failures == 0 ? 0 : 1;
}
