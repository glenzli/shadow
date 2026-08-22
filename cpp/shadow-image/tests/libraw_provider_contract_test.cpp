#include "../src/decoder/libraw_raw_geometry.hpp"
#include "contract_test_assertions.hpp"
#include "processed_rgb_session_fixture.hpp"

#include <shadow/image/decoder.hpp>
#include <shadow/image/edited_proxy_rendering.hpp>
#include <shadow/image/libraw_development_settings.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_development.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;
using shadow::image::test_support::RetainedRgbSession;

void provider_identity_versions_shadow_pixel_contracts() {
    static_assert(image::processed_linear_reference_rgb_contract_version == 1U);
    static_assert(image::processed_linear_reference_maximum_adjustment_threshold == 0.0F);
    const auto provider = image::make_libraw_decoder_provider();
    const std::string_view version = provider->info().version;
    expect(
        version.size() <= 128U,
        "provider identity remains valid for catalog content identities"
    );
    expect(
        version.find(";l=1") != std::string_view::npos,
        "provider identity versions the processed-linear reference RGB contract"
    );
    expect(
        version.find(";r=2026082101") != std::string_view::npos,
        "provider identity versions RAW-development provenance semantics"
    );
    expect(
        version.find(";p=2026082101") != std::string_view::npos,
        "provider identity versions the RAW development plan contract"
    );
    expect(
        version.find(";f=2026081901-n1-g2") != std::string_view::npos,
        "provider identity versions the owned RAW frame and active-area contracts"
    );
    expect(
        version.find("-n1-g2;v=") != std::string_view::npos,
        "provider identity versions embedded DNG sensor-noise calibration"
    );
    expect(
        version.find(";v=1") != std::string_view::npos,
        "provider identity versions display-oriented embedded-preview geometry"
    );
    expect(
        version.find(";d=2") != std::string_view::npos,
        "provider identity versions the display output transform for cache "
        "invalidation"
    );
    expect(
        version.find(";s=s1-w1-m1-a0-e0-") != std::string_view::npos,
        "provider identity includes a compact complete LibRaw development "
        "profile"
    );
}

void libraw_standard_raw_inset_overrides_legacy_margins() {
    const image::detail::LibRawRawFrameGeometryInput input{
        8'280U,
        5'520U,
        8'280U,
        5'520U,
        0U,
        0U,
        {12U, 8U, 8'256U, 5'504U},
    };

    const auto geometry = image::detail::raw_frame_geometry(input);
    expect(
        geometry.active_dimensions == image::Dimensions{8'256U, 5'504U},
        "standard raw inset defines RawFrame active dimensions before legacy image dimensions"
    );
    expect(
        geometry.active_margins == image::Margins{12U, 8U, 12U, 8U},
        "standard raw inset retains the exact stored-sensor margins"
    );
}

void invalid_libraw_standard_raw_inset_falls_back_to_legacy_margins() {
    const image::detail::LibRawRawFrameGeometryInput input{
        8'280U,
        5'520U,
        8'256U,
        5'504U,
        12U,
        8U,
        {8'000U, 5'000U, 1'000U, 1'000U},
    };

    const auto geometry = image::detail::raw_frame_geometry(input);
    expect(
        geometry.active_dimensions == image::Dimensions{8'256U, 5'504U}
            && geometry.active_margins == image::Margins{12U, 8U, 12U, 8U},
        "invalid standard raw inset cannot replace valid legacy active geometry"
    );
}

void libraw_development_settings_are_explicit_and_cache_visible() {
    const image::LibRawDevelopmentSettings defaults = image::default_libraw_development_settings();
    expect(
        defaults.schema_version == image::libraw_development_settings_schema_version,
        "default LibRaw settings name their schema"
    );
    expect(defaults.use_camera_white_balance, "default LibRaw settings use camera white balance");
    expect(defaults.use_camera_matrix, "default LibRaw settings use the camera matrix");
    expect(!defaults.use_auto_brightness, "default LibRaw settings disable auto brightness");
    expect(
        !defaults.use_exposure_correction,
        "default LibRaw settings disable implicit exposure correction"
    );
    expect(
        defaults.maximum_adjustment_threshold
            == image::processed_linear_reference_maximum_adjustment_threshold,
        "default LibRaw settings preserve the stable maximum scale"
    );

    auto tweaked = defaults;
    tweaked.brightness = 1.25F;
    const auto default_provider = image::make_libraw_decoder_provider(defaults);
    const auto tweaked_provider = image::make_libraw_decoder_provider(tweaked);
    expect(
        default_provider->info().version != tweaked_provider->info().version,
        "a LibRaw processing tweak invalidates generated-cache identity"
    );

    auto invalid = defaults;
    invalid.output_bits_per_channel = 8U;
    try {
        static_cast<void>(image::make_libraw_decoder_provider(invalid));
        expect(
            false,
            "LibRaw provider rejects an output depth outside the "
            "reference contract"
        );
    } catch (const std::invalid_argument&) {}
}

void known_canon_black_level_fixture_keeps_common_and_component_terms(
    const std::string_view fixture,
    const image::RawFrame& frame
) {
    const auto separator = fixture.find_last_of("/\\");
    const std::string_view name =
        separator == std::string_view::npos ? fixture : fixture.substr(separator + 1U);
    std::optional<std::array<std::uint32_t, 4U>> expected;
    if (name == "sample_canon_400d1.cr2") {
        // LibRaw reports common=255, cblack={1,0,0,1} and RGBG component
        // indices {0,1,3,2} for this RGGB sensor. The row-major site levels
        // must therefore be {256,255,256,255}, not {1,255,1,255}.
        expected = std::array<std::uint32_t, 4U>{256U, 255U, 256U, 255U};
    } else if (name == "Canon-eos-r-raw-00002.cr3") {
        // This EOS R sample carries common=511 and a 1-DN blue correction.
        expected = std::array<std::uint32_t, 4U>{511U, 511U, 511U, 512U};
    } else if (name == "Canon-eos-r-raw-00018.cr3") {
        expected = std::array<std::uint32_t, 4U>{2'048U, 2'048U, 2'048U, 2'048U};
    }
    if (!expected.has_value()) {
        return;
    }
    expect(frame.descriptor.cfa_pattern == "RGGB" &&
             frame.descriptor.bayer_2x2 ==
                 std::array{
                     image::RawCfaColor::red,
                     image::RawCfaColor::green,
                     image::RawCfaColor::green,
                     image::RawCfaColor::blue,
                 },
         "known Canon fixture retains LibRaw's row-major RGGB CFA mapping");
    expect(
        frame.descriptor.black_levels == *expected,
        "known Canon fixture adds common black level and per-component "
        "correction at each CFA site"
    );
}

void real_libraw_boundary_and_neutral_preview_when_configured() {
    const char* fixture = std::getenv("SHADOW_TEST_DNG");
    if (fixture == nullptr || *fixture == '\0') {
        return;
    }

    const auto provider = image::make_libraw_decoder_provider();
    const auto decoder = provider->open(fixture);
    expect(
        decoder->capabilities().raw_frame && decoder->raw_development_capabilities().raw_frame,
        "real LibRaw RAW source advertises the owned RawFrame contract"
    );
    if (decoder->capabilities().raw_frame) {
        const auto frame = decoder->decode_raw_frame();
        expect(frame.valid(), "real LibRaw RAW frame preserves a complete owned sample plane");
        known_canon_black_level_fixture_keeps_common_and_component_terms(fixture, frame);
        expect(
            std::ranges::all_of(
                frame.descriptor.white_levels,
                [&decoder](const std::uint32_t white_level) {
                    return white_level == decoder->metadata().white_level;
                }
            ),
            "RawFrame uses LibRaw's calibrated coding white at every CFA site instead of "
            "truncating a channel at linear_max"
        );
        expect(
            frame.descriptor.provider_id == provider->info().id
                && frame.descriptor.provider_version == provider->info().version,
            "real LibRaw RAW frame carries the provider identity needed by "
            "sensor-cache keys"
        );
        expect(
            frame.descriptor.storage_dimensions == decoder->metadata().raw_dimensions
                && frame.descriptor.active_dimensions == decoder->metadata().image_dimensions
                && frame.descriptor.active_margins == decoder->metadata().margins,
            "real LibRaw RAW frame retains exact sensor storage and active-area "
            "geometry"
        );
        expect(
            frame.descriptor.declared_pending_corrections
                == decoder->capabilities().pending_corrections,
            "real LibRaw RAW frame records DNG corrections without claiming "
            "they were applied"
        );
        expect(
            std::ranges::all_of(
                frame.descriptor.as_shot_neutral,
                [](const double value) { return std::isfinite(value) && value > 0.0; }
            ),
            "real LibRaw RAW frame resolves a positive row-major neutral for "
            "every CFA site"
        );
        if (frame.is_bayer_2x2()) {
            expect(
                frame.descriptor.has_camera_to_linear_srgb_d65,
                "real LibRaw Bayer frame exposes its camera-to-linear-sRGB D65 "
                "transform"
            );
        }
    }
    image::PixelBuffer decoded = decoder->render_reference_rgb();
    expect(decoded.bits_per_channel == 16U, "real LibRaw boundary returns 16-bit samples");
    expect(
        decoded.primaries == image::RgbPrimaries::srgb_rec709_d65,
        "real LibRaw boundary declares its sRGB/Rec.709-D65 primaries"
    );
    expect(
        decoded.transfer_function == image::RgbTransferFunction::linear,
        "real LibRaw boundary declares the configured linear transfer"
    );
    expect(
        decoded.reference == image::RgbBufferReference::processed_raw,
        "real LibRaw boundary cannot be mistaken for untouched sensor-linear "
        "data"
    );
    const auto& receipt = decoded.raw_development_receipt;
    expect(receipt.recorded(), "real LibRaw render records RAW-development provenance");
    expect(
        receipt.schema_version == image::raw_development_receipt_schema_version,
        "real LibRaw receipt names the supported schema"
    );
    expect(receipt.provider_id == "libraw", "receipt identifies the LibRaw provider");
    expect(
        receipt.provider_version == provider->info().version && !receipt.library_version.empty(),
        "receipt carries both provider identity and linked LibRaw release"
    );
    expect(
        receipt.development_settings_signature
            == image::libraw_development_settings_signature(
                image::default_libraw_development_settings()
            ),
        "receipt carries the exact development settings signature"
    );
    const auto default_plan = image::default_raw_development_plan();
    expect(
        decoder->raw_development_capabilities().available
            && decoder->raw_development_capabilities().supports(default_plan)
            && decoder->negotiate_raw_development_plan(default_plan).exact(),
        "LibRaw advertises the exact provider-neutral RAW plan it can satisfy"
    );
    auto export_plan = default_plan;
    export_plan.intent = image::RawDevelopmentIntent::export_image;
    export_plan.quality = image::RawDevelopmentQuality::high;
    const auto export_negotiation = decoder->negotiate_raw_development_plan(export_plan);
    expect(
        export_negotiation.accepted() && !export_negotiation.exact()
            && export_negotiation.requested == export_plan
            && export_negotiation.effective.intent == image::RawDevelopmentIntent::export_image
            && export_negotiation.effective.quality == image::RawDevelopmentQuality::balanced,
        "LibRaw explicitly records its high-to-balanced export quality "
        "adjustment"
    );
    expect(
        receipt.requested_plan == default_plan && receipt.effective_plan == default_plan
            && receipt.requested_plan_identity == image::raw_development_plan_identity(default_plan)
            && receipt.effective_plan_identity == image::raw_development_plan_identity(default_plan)
            && receipt.plan_negotiation_status
                   == image::RawDevelopmentPlanNegotiationStatus::accepted,
        "LibRaw receipt records the exact effective RAW development plan"
    );
    expect(
        receipt.processed_linear_reference_contract_version
            == image::processed_linear_reference_rgb_contract_version,
        "receipt carries the processed-linear pixel contract"
    );
    expect(
        receipt.rendered_dimensions == decoded.dimensions
            && receipt.orientation == decoder->metadata().orientation,
        "receipt records the actual rendered raster and LibRaw orientation"
    );
    expect(!receipt.half_size, "full reference rendering is never recorded as half-size");
    expect(
        receipt.use_camera_white_balance && receipt.use_camera_matrix
            && !receipt.use_auto_brightness && !receipt.use_exposure_correction,
        "receipt exposes Shadow's fixed LibRaw source-development switches"
    );
    expect(
        receipt.brightness == 1.0F && receipt.maximum_adjustment_threshold == 0.0F
            && receipt.output_bits_per_channel == 16U && receipt.output_color == 1
            && receipt.gamma_inverse_power == 1.0 && receipt.gamma_linear_toe_slope == 1.0,
        "receipt exposes the fixed linear output transfer and output format "
        "request"
    );
    expect(
        receipt.declared_dng_opcode_lists == decoder->capabilities().pending_corrections,
        "receipt preserves declared DNG opcode lists alongside LibRaw "
        "processing warnings"
    );
    for (std::size_t index = 0U; index < receipt.dng_opcode_execution.size(); ++index) {
        const auto expected = receipt.declared_dng_opcode_lists.dng_opcode_list_bytes[index] == 0U
                                  ? image::DngOpcodeExecutionStatus::not_declared
                                  : image::DngOpcodeExecutionStatus::provider_default;
        expect(
            receipt.dng_opcode_execution[index] == expected,
            "LibRaw receipt never overclaims DNG opcode application"
        );
    }

    RetainedRgbSession retained(std::move(decoded), decoder->metadata());
    const std::uint32_t source_edge = std::max(
        retained.metadata().image_dimensions.width,
        retained.metadata().image_dimensions.height
    );
    expect(source_edge <= 16'384U, "real neutral fixture fits the bounded proxy contract");
    if (source_edge > 16'384U) {
        return;
    }
    const image::ProxyRequest request{
        .max_edge = std::min(source_edge, image::maximum_warm_edit_preview_edge),
        .jpeg_quality = 90,
    };
    const auto reference = image::render_reference_proxy_jpeg(retained, request);
    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "neutral-real-exposure",
            .parameters = image::ExposureAdjustment{},
        },
    };
    const auto neutral =
        image::render_edited_reference_proxy_jpeg(retained, neutral_nodes, request);
    expect(
        neutral.bytes == reference.bytes,
        "real processed-linear pixels follow one identical neutral display "
        "transform"
    );
}

} // namespace

int main() {
    provider_identity_versions_shadow_pixel_contracts();
    libraw_standard_raw_inset_overrides_legacy_margins();
    invalid_libraw_standard_raw_inset_falls_back_to_legacy_margins();
    libraw_development_settings_are_explicit_and_cache_visible();
    real_libraw_boundary_and_neutral_preview_when_configured();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
