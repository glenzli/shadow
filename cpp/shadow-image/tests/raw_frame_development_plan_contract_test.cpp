#include "../src/raw/raw_frame_development_plan.hpp"
#include "../src/raw/raw_frame_source_development.hpp"
#include "raw_pipeline_routing_test_support.hpp"

#include <cmath>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

static_assert(
    !std::is_default_constructible_v<image::raw_pipeline_detail::PreparedRawFrameDevelopment>
);
static_assert(
    !std::is_copy_constructible_v<image::raw_pipeline_detail::PreparedRawFrameDevelopment>
);
static_assert(
    std::is_move_constructible_v<image::raw_pipeline_detail::PreparedRawFrameDevelopment>
);
static_assert(!std::is_move_assignable_v<image::raw_pipeline_detail::PreparedRawFrameDevelopment>);

namespace {

void prepared_plan_binds_source_policy_and_calibration_once() {
    const auto frame = synthetic_bayer_frame();
    auto preview_plan = image::preview_raw_development_plan();
    const auto preview = image::raw_pipeline_detail::prepare_raw_frame_development(
        frame,
        preview_plan,
        2U,
        std::nullopt,
        6'400.0
    );
    const auto detail = image::raw_pipeline_detail::prepare_raw_frame_development(
        frame,
        image::default_raw_development_plan(),
        std::nullopt,
        std::nullopt,
        6'400.0
    );

    expect(
        preview.descriptor().provider_id == frame.descriptor.provider_id
            && preview.descriptor().provider_version == frame.descriptor.provider_version
            && preview.development_plan() == preview_plan && preview.preview_max_edge() == 2U,
        "prepared RAW development binds provider provenance, effective policy, and preview geometry"
    );
    expect(
        preview.linear_transform().valid() && preview.camera_profile() == nullptr
            && !preview.raw_denoise().applied()
            && preview.reconstruction_dimensions() == image::Dimensions{2U, 2U}
            && preview.diagnostic_dimensions() == image::Dimensions{2U, 2U},
        "prepared RAW development owns transform, preview dimensions, and preview-aware denoise"
    );
    expect(
        std::isfinite(preview.source_scene_luminance_percentile())
            && preview.source_scene_luminance_percentile() >= 0.0
            && preview.source_scene_luminance_percentile()
                   == detail.source_scene_luminance_percentile(),
        "source calibration is prepared once before the preview/detail execution split"
    );
}

void prepared_plan_owns_the_compiled_camera_profile() {
    const auto frame = synthetic_bayer_frame(false);
    auto catalog = exact_dcp_catalog();
    auto transform = image::compile_dcp_color_transform(catalog.profiles.front(), frame.descriptor);
    const auto expected_identity = transform.receipt.profile_content_identity;
    auto prepared = image::raw_pipeline_detail::prepare_raw_frame_development(
        frame,
        image::default_raw_development_plan(),
        std::nullopt,
        std::optional<image::DcpColorTransform>{std::move(transform)},
        0.0
    );

    const auto* owned_profile = prepared.camera_profile();
    expect(
        owned_profile != nullptr
            && owned_profile->receipt.profile_content_identity == expected_identity
            && owned_profile->hue_sat_map.has_value()
            && owned_profile->hue_sat_map->entries.size() == 2U
            && prepared.linear_transform().camera_to_linear_srgb_d65
                   == owned_profile->camera_to_linear_srgb_d65,
        "prepared RAW development owns the complete compiled DCP and its camera transform"
    );
}

void full_materializer_consumes_the_prepared_contract() {
    const auto plan = image::default_raw_development_plan();
    SyntheticRawSession direct_session(synthetic_bayer_frame());
    const image::CameraProfileCatalog catalog{
        .identity = "shadow-camera-profile-catalog-v1:prepared-source-test",
    };
    auto prepared = image::raw_pipeline_detail::prepare_raw_frame_source(
        direct_session,
        plan,
        std::nullopt,
        catalog
    );
    const auto prepared_percentile = prepared.development().source_scene_luminance_percentile();
    const auto direct =
        image::raw_pipeline_detail::materialize_prepared_raw_frame_source(std::move(prepared));

    SyntheticRawSession session(synthetic_bayer_frame());
    const auto routed = image::develop_source_reference(
        session,
        plan,
        std::nullopt,
        image::RawPipelinePolicy{
            .mode = image::RawPipelineMode::require_shadow_raw_frame,
        }
    );
    const auto& direct_pixels = std::get<image::SceneLinearRgbFrame>(direct.source);
    const auto& routed_pixels = std::get<image::SceneLinearRgbFrame>(routed.source);
    expect(
        direct_pixels.dimensions == routed_pixels.dimensions
            && direct_pixels.row_stride_bytes == routed_pixels.row_stride_bytes
            && direct_pixels.samples == routed_pixels.samples,
        "the routed full materializer emits the exact prepared-plan pixels"
    );
    expect(
        direct.raw_development_receipt.development_settings_signature
                == routed.raw_development_receipt.development_settings_signature
            && direct.raw_development_receipt.rendered_dimensions
                   == routed.raw_development_receipt.rendered_dimensions
            && direct.raw_development_receipt.demosaic_quality
                   == routed.raw_development_receipt.demosaic_quality
            && direct.pipeline_receipt.source_scene_luminance_percentile == prepared_percentile
            && routed.pipeline_receipt.source_scene_luminance_percentile == prepared_percentile,
        "prepared execution preserves development, denoise, and calibration provenance"
    );

    const auto raster_receipt =
        image::resolve_source_rendering(routed_pixels, session.metadata(), routed.pipeline_receipt);
    const auto resident_receipt =
        image::resolve_source_rendering(session.metadata(), routed.pipeline_receipt);
    expect(
        resident_receipt == raster_receipt,
        "source-wide RAW calibration resolves identical rendering without a full scene-linear "
        "raster"
    );
}

template <typename Prepare>
void expect_prepare_error(
    Prepare&& prepare,
    const image::DecodeErrorCode expected_code,
    const std::string_view expected_message,
    const std::string_view contract
) {
    try {
        std::forward<Prepare>(prepare)();
        expect(false, contract);
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == expected_code && std::string_view(error.what()) == expected_message,
            contract
        );
    }
}

void preparation_preserves_validation_order() {
    auto invalid = synthetic_bayer_frame();
    invalid.samples.pop_back();
    invalid.descriptor.orientation = 1;
    expect_prepare_error(
        [&] {
            static_cast<void>(image::raw_pipeline_detail::prepare_raw_frame_development(
                invalid,
                image::default_raw_development_plan(),
                std::nullopt,
                std::nullopt,
                0.0
            ));
        },
        image::DecodeErrorCode::unsupported_layout,
        "Shadow's RAW developer currently requires a valid Bayer two-by-two frame",
        "frame validity is rejected before orientation"
    );

    auto pending_without_matrix = synthetic_bayer_frame(false);
    pending_without_matrix.descriptor.declared_pending_corrections.dng_opcode_list_bytes[0] = 1U;
    expect_prepare_error(
        [&] {
            static_cast<void>(image::raw_pipeline_detail::prepare_raw_frame_development(
                pending_without_matrix,
                image::default_raw_development_plan(),
                std::nullopt,
                std::nullopt,
                0.0
            ));
        },
        image::DecodeErrorCode::unsupported,
        "Shadow's RAW developer cannot yet execute this source's declared DNG opcodes",
        "pending source corrections are rejected before a missing camera transform"
    );
}

} // namespace

int main() {
    prepared_plan_binds_source_policy_and_calibration_once();
    prepared_plan_owns_the_compiled_camera_profile();
    full_materializer_consumes_the_prepared_contract();
    preparation_preserves_validation_order();
    return failures == 0 ? 0 : 1;
}
