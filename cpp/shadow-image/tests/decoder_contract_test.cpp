#include <shadow/image/color_management.hpp>
#include <shadow/image/decoder.hpp>
#include <shadow/image/edit.hpp>
#include <shadow/image/optics.hpp>
#include <shadow/image/private_decoder_plugin.hpp>
#include <shadow/image/raw_development.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
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

[[nodiscard]] bool jpeg_uses_444_chroma_sampling(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4U || bytes[0] != 0xffU || bytes[1] != 0xd8U) {
        return false;
    }
    std::size_t offset = 2U;
    while (offset + 4U <= bytes.size()) {
        if (bytes[offset] != 0xffU) {
            return false;
        }
        while (offset < bytes.size() && bytes[offset] == 0xffU) {
            ++offset;
        }
        if (offset >= bytes.size()) {
            return false;
        }
        const std::uint8_t marker = bytes[offset++];
        if (marker == 0xd9U || marker == 0xdaU) {
            return false;
        }
        if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U)) {
            continue;
        }
        if (offset + 2U > bytes.size()) {
            return false;
        }
        const std::size_t length = (static_cast<std::size_t>(bytes[offset]) << 8U)
            | bytes[offset + 1U];
        if (length < 2U || offset + length > bytes.size()) {
            return false;
        }
        const bool start_of_frame = marker >= 0xc0U && marker <= 0xcfU
            && marker != 0xc4U && marker != 0xc8U && marker != 0xccU;
        if (start_of_frame) {
            if (length < 11U || bytes[offset + 7U] != 3U) {
                return false;
            }
            for (std::size_t component = 0U; component < 3U; ++component) {
                const std::size_t sampling = offset + 9U + component * 3U;
                if (sampling >= offset + length || bytes[sampling] != 0x11U) {
                    return false;
                }
            }
            return true;
        }
        offset += length;
    }
    return false;
}

void pending_corrections_are_explicit() {
    image::PendingCorrections empty;
    expect(!empty.has_pending(), "empty correction state must not be pending");

    image::PendingCorrections stage_three{{0U, 0U, 76U}};
    expect(stage_three.has_pending(), "a DNG opcode list must be reported as pending");
}

void raw_development_receipt_is_explicitly_absent_until_a_provider_records_it() {
    const image::PixelBuffer generic;
    expect(
        !generic.raw_development_receipt.recorded(),
        "generic processed RGB never pretends to carry RAW provenance"
    );
    expect(
        image::raw_development_receipt_schema_version == 2U,
        "RAW development receipt schema is explicitly versioned"
    );
}

void raw_frame_is_owned_unprocessed_and_bayer_guarded() {
    image::RawFrame frame;
    frame.descriptor.schema_version = image::raw_frame_schema_version;
    frame.descriptor.storage_dimensions = {4U, 2U};
    frame.descriptor.active_dimensions = {4U, 2U};
    frame.descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    frame.descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    frame.descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    frame.descriptor.cfa_pattern = "RGGB";
    frame.descriptor.bits_per_sample = 14U;
    frame.descriptor.black_levels = {512U, 510U, 511U, 508U};
    frame.descriptor.white_levels = {16'383U, 16'383U, 16'383U, 16'383U};
    frame.samples = {512U, 600U, 700U, 800U, 900U, 1'000U, 1'100U, 1'200U};
    expect(frame.valid(), "a complete owned RAW frame validates before any sensor processing");
    expect(frame.is_bayer_2x2(), "Bayer stages require an explicit two-by-two CFA layout");

    auto unknown_layout = frame;
    unknown_layout.descriptor.cfa_layout = image::RawFrameCfaLayout::unknown;
    expect(
        unknown_layout.valid() && !unknown_layout.is_bayer_2x2(),
        "an unknown CFA remains inspectable but cannot enter a Bayer-only algorithm"
    );

    auto truncated = frame;
    truncated.samples.pop_back();
    expect(!truncated.valid(), "RAW frame validation rejects a non-owned/truncated sample plane");
}

void bayer_bilinear_demosaic_keeps_the_sensor_domain_explicit() {
    image::RawFrame frame;
    frame.descriptor.schema_version = image::raw_frame_schema_version;
    frame.descriptor.storage_dimensions = {4U, 4U};
    frame.descriptor.active_dimensions = {4U, 4U};
    frame.descriptor.sample_encoding = image::RawFrameSampleEncoding::uint16_native;
    frame.descriptor.cfa_layout = image::RawFrameCfaLayout::bayer_2x2;
    frame.descriptor.bayer_2x2 = {
        image::RawCfaColor::red,
        image::RawCfaColor::green,
        image::RawCfaColor::green,
        image::RawCfaColor::blue,
    };
    frame.descriptor.cfa_pattern = "RGGB";
    frame.descriptor.bits_per_sample = 12U;
    frame.descriptor.black_levels = {100U, 100U, 100U, 100U};
    frame.descriptor.white_levels = {1'100U, 1'100U, 1'100U, 1'100U};
    frame.samples.resize(16U);
    for (std::uint32_t y = 0U; y < 4U; ++y) {
        for (std::uint32_t x = 0U; x < 4U; ++x) {
            const auto color = frame.descriptor.bayer_2x2[(y & 1U) * 2U + (x & 1U)];
            frame.samples[static_cast<std::size_t>(y) * 4U + x] = color == image::RawCfaColor::red
                ? 300U
                : color == image::RawCfaColor::green ? 500U : 900U;
        }
    }
    expect(frame.valid(), "constant Bayer fixture is a valid unprocessed RAW frame");

    const auto output = image::demosaic_bayer_bilinear(frame);
    expect(output.valid(), "bilinear Bayer demosaic produces a valid camera-linear RGB frame");
    expect(
        output.receipt.algorithm == image::RawDemosaicAlgorithm::bayer_bilinear_v1
            && output.receipt.black_subtraction_applied
            && output.receipt.white_level_normalization_applied
            && !output.receipt.white_balance_applied
            && !output.receipt.dng_opcodes_applied,
        "Bayer demosaic receipt never overclaims white balance or DNG opcode application"
    );
    for (std::size_t pixel = 0U; pixel < 16U; ++pixel) {
        const auto index = pixel * 3U;
        expect(
            std::abs(output.samples[index] - 0.2F) < 1.0e-6F
                && std::abs(output.samples[index + 1U] - 0.4F) < 1.0e-6F
                && std::abs(output.samples[index + 2U] - 0.8F) < 1.0e-6F,
            "bilinear Bayer reconstruction preserves per-CFA black/white normalized camera RGB"
        );
    }

    auto non_bayer = frame;
    non_bayer.descriptor.cfa_layout = image::RawFrameCfaLayout::unknown;
    try {
        static_cast<void>(image::demosaic_bayer_bilinear(non_bayer));
        expect(false, "Bayer demosaic must reject an unknown CFA layout");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported_layout,
            "unknown CFA layout is rejected before Bayer processing"
        );
    }
}

void raw_development_plan_is_canonical_and_capability_negotiated() {
    const auto detail = image::default_raw_development_plan();
    const auto preview = image::preview_raw_development_plan();
    expect(
        detail.intent == image::RawDevelopmentIntent::detail
            && detail.quality == image::RawDevelopmentQuality::balanced
            && detail.dng_opcode_policy == image::DngOpcodePolicy::provider_default,
        "default RAW development plan is a neutral full-detail provider request"
    );
    expect(
        preview.intent == image::RawDevelopmentIntent::preview
            && preview.quality == detail.quality
            && preview.dng_opcode_policy == detail.dng_opcode_policy,
        "preview RAW development plan changes intent without changing source policy"
    );
    expect(
        image::raw_development_plan_identity(detail)
            == "shadow-raw-plan-v1;intent=detail;quality=balanced;opcodes=provider-default;nr=provider-default;highlights=provider-default",
        "RAW development plan identity is canonical and cache-visible"
    );

    image::RawDevelopmentCapabilities capabilities;
    capabilities.schema_version = image::raw_development_capabilities_schema_version;
    capabilities.available = true;
    capabilities.supported_intents = image::raw_development_intent_mask(
        image::RawDevelopmentIntent::preview
    ) | image::raw_development_intent_mask(image::RawDevelopmentIntent::detail);
    capabilities.supported_qualities = image::raw_development_quality_mask(
        image::RawDevelopmentQuality::balanced
    );
    capabilities.supported_dng_opcode_policies = image::dng_opcode_policy_mask(
        image::DngOpcodePolicy::provider_default
    );
    capabilities.supported_noise_reduction_intents = image::raw_noise_reduction_intent_mask(
        image::RawNoiseReductionIntent::provider_default
    );
    capabilities.supported_highlight_recovery_intents =
        image::raw_highlight_recovery_intent_mask(
            image::RawHighlightRecoveryIntent::provider_default
        );
    const auto accepted = image::negotiate_raw_development_plan(detail, capabilities);
    expect(
        accepted.accepted() && accepted.exact() && accepted.requested == detail
            && accepted.effective == detail,
        "capability negotiation accepts an exactly supported RAW plan"
    );

    auto unsupported_quality = detail;
    unsupported_quality.quality = image::RawDevelopmentQuality::high;
    const auto quality_rejected = image::negotiate_raw_development_plan(
        unsupported_quality,
        capabilities
    );
    expect(
        !quality_rejected.accepted()
            && image::raw_development_plan_aspect_contains(
                quality_rejected.unresolved,
                image::RawDevelopmentPlanAspect::quality
            ),
        "a provider cannot silently substitute an unsupported RAW quality tier"
    );

    auto unsupported_opcode_policy = detail;
    unsupported_opcode_policy.dng_opcode_policy = image::DngOpcodePolicy::require_applied;
    const auto opcode_rejected = image::negotiate_raw_development_plan(
        unsupported_opcode_policy,
        capabilities
    );
    expect(
        !opcode_rejected.accepted()
            && image::raw_development_plan_aspect_contains(
                opcode_rejected.unresolved,
                image::RawDevelopmentPlanAspect::dng_opcode_policy
            ),
        "a provider cannot silently claim required DNG opcode application"
    );

    auto future_schema = detail;
    future_schema.schema_version += 1U;
    const auto schema_rejected = image::negotiate_raw_development_plan(future_schema, capabilities);
    expect(
        !schema_rejected.accepted()
            && image::raw_development_plan_aspect_contains(
                schema_rejected.unresolved,
                image::RawDevelopmentPlanAspect::schema
            ),
        "unknown RAW development plan schemas fail closed"
    );
    try {
        static_cast<void>(image::raw_development_plan_identity(future_schema));
        expect(false, "unknown RAW development plan schemas cannot produce a cache identity");
    } catch (const std::invalid_argument&) {
        expect(true, "invalid RAW plan identity reports an invalid argument");
    }
}

void icc_color_management_is_content_addressed_and_transfer_aware() {
    const auto linear_srgb = image::make_linear_srgb_icc_profile();
    const auto another_linear_srgb = image::make_linear_srgb_icc_profile();
    const auto display_srgb = image::make_display_srgb_icc_profile();
    const auto display_rec709 = image::make_display_rec709_icc_profile();
    expect(
        linear_srgb.info().id == another_linear_srgb.info().id,
        "equivalent generated ICC profiles have stable content identities"
    );
    expect(
        linear_srgb.info().id != display_srgb.info().id,
        "linear and display sRGB profiles cannot share a cache identity"
    );
    expect(
        display_rec709.info().id != display_srgb.info().id,
        "Rec.709 and sRGB transfers cannot share a cache identity"
    );

    const auto identity = image::make_icc_transform(linear_srgb, linear_srgb);
    std::array<float, 6U> samples{0.18F, 0.5F, 1.2F, 0.0F, 0.25F, 0.75F};
    const auto before = samples;
    identity.apply_interleaved_rgb(samples);
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        expect(
            std::abs(samples[index] - before[index]) < 1.0e-5F,
            "linear sRGB ICC identity transform preserves scene-linear samples"
        );
    }

    const auto display_transform = image::make_icc_transform(
        linear_srgb,
        display_srgb,
        image::IccRenderingIntent::relative_colorimetric
    );
    std::array<float, 3U> middle_gray{0.18F, 0.18F, 0.18F};
    display_transform.apply_interleaved_rgb(middle_gray);
    for (const auto encoded : middle_gray) {
        expect(
            std::abs(encoded - 0.461F) < 0.01F,
            "linear-to-display ICC transform applies the sRGB transfer curve"
        );
    }

    const auto rec709_transform = image::make_icc_transform(
        linear_srgb,
        display_rec709,
        image::IccRenderingIntent::relative_colorimetric
    );
    std::array<float, 3U> rec709_middle_gray{0.18F, 0.18F, 0.18F};
    rec709_transform.apply_interleaved_rgb(rec709_middle_gray);
    for (const auto encoded : rec709_middle_gray) {
        expect(
            std::abs(encoded - 0.409F) < 0.01F,
            "linear-to-display ICC transform applies the Rec.709 transfer curve"
        );
    }

    try {
        std::array<float, 2U> malformed{0.0F, 0.0F};
        identity.apply_interleaved_rgb(malformed);
        expect(false, "ICC transform rejects non-RGB sample counts");
    } catch (const std::invalid_argument&) {
        expect(true, "ICC transform reports malformed RGB sample counts");
    }
}

void private_decoder_plugin_abi_is_explicit_and_fail_closed() {
    const image::PrivateDecoderPluginDescriptor valid{
        .abi_version = image::private_decoder_plugin_abi_version,
        .raw_development_plan_schema_version = image::raw_development_plan_schema_version,
        .raw_frame_schema_version = image::raw_frame_schema_version,
        .plugin_id = "nikon-local",
        .plugin_version = "0.1.0",
    };
    try {
        image::validate_private_decoder_plugin_descriptor(valid);
        expect(true, "private decoder plugin ABI accepts a valid descriptor");
    } catch (const image::DecodeError&) {
        expect(false, "valid private decoder plugin descriptor must not throw");
    }

    auto future_abi = valid;
    future_abi.abi_version += 1U;
    try {
        image::validate_private_decoder_plugin_descriptor(future_abi);
        expect(false, "future private decoder ABI must be rejected");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported,
            "private decoder ABI mismatch reports unsupported"
        );
    }

    auto legacy_abi = valid;
    legacy_abi.abi_version -= 1U;
    try {
        image::validate_private_decoder_plugin_descriptor(legacy_abi);
        expect(false, "legacy private decoder ABI must be rejected before provider construction");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported,
            "legacy private decoder ABI fails closed as unsupported"
        );
    }

    auto legacy_plan_schema = valid;
    legacy_plan_schema.raw_development_plan_schema_version -= 1U;
    try {
        image::validate_private_decoder_plugin_descriptor(legacy_plan_schema);
        expect(false, "private decoder plan schema must be rejected before provider construction");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported,
            "private decoder plan schema mismatch reports unsupported"
        );
    }

    auto legacy_raw_frame_schema = valid;
    legacy_raw_frame_schema.raw_frame_schema_version -= 1U;
    try {
        image::validate_private_decoder_plugin_descriptor(legacy_raw_frame_schema);
        expect(false, "private decoder RAW frame schema must be rejected before construction");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported,
            "private decoder RAW frame schema mismatch fails closed as unsupported"
        );
    }

    auto invalid_id = valid;
    invalid_id.plugin_id = "vendor sdk";
    try {
        image::validate_private_decoder_plugin_descriptor(invalid_id);
        expect(false, "private decoder identifiers cannot contain spaces");
    } catch (const image::DecodeError&) {
        expect(true, "invalid private decoder id is rejected");
    }
}

void private_decoder_plugin_loads_an_explicit_local_module() {
#if defined(SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH)
    const auto provider = image::load_private_decoder_plugin(
        SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH
    );
    expect(
        provider->info().id == "private.test-private-provider.fixture",
        "private plugin identity remains namespaced by its explicit local module"
    );
    expect(
        provider->info().version.starts_with("1.0.0;abi=4;plan=1;frame=1;wrapped=")
            && provider->info().version.size() <= 128U,
        "private plugin version, ABI and wrapped-provider cache identity stay bounded"
    );
    const auto session = provider->open("does-not-need-to-exist.raw");
    expect(
        session->metadata().model == "Private decoder test fixture",
        "private provider session stays callable through the host adapter"
    );
    expect(
        session->raw_development_capabilities().available
            && session->raw_development_capabilities().raw_frame
            && session->raw_development_capabilities().supports(
                image::default_raw_development_plan()
            ),
        "private provider exposes plan capability negotiation through the host adapter"
    );
    const auto raw_frame = session->decode_raw_frame();
    expect(
        raw_frame.valid()
            && raw_frame.is_bayer_2x2()
            && raw_frame.samples == std::vector<std::uint16_t>({1'024U, 1'100U, 1'100U, 900U})
            && raw_frame.descriptor.has_camera_to_xyz_d50
            && raw_frame.descriptor.camera_to_xyz_d50
                == std::array<double, 9U>{
                    0.70, 0.20, 0.10,
                    0.10, 0.80, 0.10,
                    0.05, 0.15, 0.80,
                },
        "private plugin RawFrame crosses the local ABI with CFA calibration intact"
    );
    const auto pixels = session->render_reference_rgb();
    expect(
        pixels.samples == std::vector<std::uint16_t>({0U, 1U, 2U, 3U, 4U, 5U}),
        "private plugin reference RGB crosses the provider-neutral contract"
    );
    expect(
        pixels.raw_development_receipt.uses_current_schema()
            && pixels.raw_development_receipt.provider_id
                == "private.test-private-provider.fixture"
            && pixels.raw_development_receipt.provider_version == provider->info().version
            && pixels.raw_development_receipt.library_version == "private-fixture-sdk",
        "private plugin receipts are bound to the host provider identity without hiding SDK detail"
    );
    const auto explicit_detail_plan = image::default_raw_development_plan();
    const auto explicit_detail = session->render_reference_rgb(explicit_detail_plan);
    expect(
        explicit_detail.raw_development_receipt.requested_plan == explicit_detail_plan
            && explicit_detail.raw_development_receipt.effective_plan == explicit_detail_plan
            && explicit_detail.raw_development_receipt.requested_plan_identity
                == image::raw_development_plan_identity(explicit_detail_plan),
        "private provider plan render binds an auditable requested and effective plan receipt"
    );
    const auto preview_pixels = session->render_reference_rgb_for_preview(1U);
    expect(
        preview_pixels.dimensions == image::Dimensions{1U, 1U}
            && preview_pixels.samples == std::vector<std::uint16_t>({9U, 8U, 7U})
            && preview_pixels.raw_development_receipt.provider_id
                == "private.test-private-provider.fixture",
        "private plugin fast preview is forwarded instead of falling back to full RGB"
    );
    const auto explicit_preview_plan = image::preview_raw_development_plan();
    const auto explicit_preview = session->render_reference_rgb_for_preview(
        1U,
        explicit_preview_plan
    );
    expect(
        explicit_preview.raw_development_receipt.requested_plan == explicit_preview_plan
            && explicit_preview.raw_development_receipt.half_size,
        "private provider plan-aware preview retains preview intent in its receipt"
    );
    auto unsupported_quality = explicit_detail_plan;
    unsupported_quality.quality = image::RawDevelopmentQuality::high;
    try {
        static_cast<void>(session->render_reference_rgb(unsupported_quality));
        expect(false, "private provider must reject a RAW plan it did not negotiate");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::unsupported,
            "private provider rejects unsupported RAW plan quality before rendering"
        );
    }
#else
    expect(false, "private decoder plugin test target path must be configured");
#endif
}

void private_decoder_router_prefers_an_explicit_local_module() {
#if defined(SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH) \
    && defined(SHADOW_TEST_LIBRAW_DUMMY_PRIVATE_DECODER_PLUGIN_PATH)
    const auto router = image::make_photo_decoder_provider(
        SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH
    );
    expect(
        router->info().version.find(";private=") != std::string::npos,
        "photo router includes an explicit private provider in its cache identity"
    );
    expect(
        router->info().version.find(";private_module=") != std::string::npos,
        "photo router includes the local private module fingerprint in its cache identity"
    );
    const auto fixture_session = router->open("fixture-private-provider.raw");
    expect(
        fixture_session->metadata().model == "Private decoder test fixture",
        "photo router tries the explicit private provider before LibRaw for RAW sources"
    );

    const auto dummy = image::load_private_decoder_plugin(
        SHADOW_TEST_LIBRAW_DUMMY_PRIVATE_DECODER_PLUGIN_PATH
    );
    expect(
        dummy->info().id == "private.libraw-dummy.libraw",
        "LibRaw dummy provider loads through the same private module contract"
    );
#else
    expect(false, "private decoder router fixture paths must be configured");
#endif
}

[[nodiscard]] image::PixelBuffer optics_reference_buffer(
    const std::uint32_t width = 96U,
    const std::uint32_t height = 64U
) {
    image::PixelBuffer buffer;
    buffer.dimensions = {width, height};
    buffer.bits_per_channel = 16U;
    buffer.channels = 3U;
    buffer.row_stride_bytes = static_cast<std::size_t>(width) * 3U * sizeof(std::uint16_t);
    buffer.primaries = image::RgbPrimaries::srgb_rec709_d65;
    buffer.transfer_function = image::RgbTransferFunction::linear;
    buffer.reference = image::RgbBufferReference::processed_raw;
    buffer.samples.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const auto index = (static_cast<std::size_t>(y) * width + x) * 3U;
            buffer.samples[index] = static_cast<std::uint16_t>(
                static_cast<std::uint64_t>(x) * 65'535U / (width - 1U)
            );
            buffer.samples[index + 1U] = static_cast<std::uint16_t>(
                static_cast<std::uint64_t>(y) * 65'535U / (height - 1U)
            );
            buffer.samples[index + 2U] = static_cast<std::uint16_t>(
                (static_cast<std::uint64_t>(x + y) * 65'535U) / (width + height - 2U)
            );
        }
    }
    return buffer;
}

void optics_settings_are_explicit_and_provider_safe() {
    const auto defaults = image::default_optics_settings();
    expect(defaults.schema_version == image::optics_settings_schema_version,
           "optics defaults declare the current schema");
    expect(defaults.enabled && defaults.correct_distortion && defaults.correct_tca
               && defaults.correct_vignetting && defaults.automatic_scale,
           "optics defaults preserve all automatic profile corrections");
    const auto default_signature = image::optics_settings_signature(defaults);
    auto no_tca = defaults;
    no_tca.correct_tca = false;
    expect(
        image::optics_settings_signature(no_tca) != default_signature,
        "every optics choice participates in cache identity"
    );

    const auto provider = image::make_lensfun_optics_provider();
    auto disabled = defaults;
    disabled.enabled = false;
    const auto disabled_result = provider->correct_reference_rgb(
        optics_reference_buffer(),
        image::AssetMetadata{},
        disabled
    );
    expect(
        disabled_result.receipt.status == image::OpticsProfileStatus::disabled,
        "disabled optics do not require profile metadata"
    );
    expect(
        !disabled_result.corrected_reference_rgb.has_value(),
        "disabled optics do not duplicate the reference raster"
    );

    auto unsupported_schema = defaults;
    unsupported_schema.schema_version += 1U;
    try {
        static_cast<void>(image::optics_settings_signature(unsupported_schema));
        expect(false, "unknown optics settings schemas must fail closed");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "unknown optics schema reports invalid request"
        );
    }
}

void lensfun_adapter_applies_a_real_profile_when_a_test_database_is_available() {
    const auto* database = std::getenv("SHADOW_TEST_LENSFUN_DB");
    if (database == nullptr || *database == '\0') {
        return;
    }
    const auto provider = image::make_lensfun_optics_provider(database);
    expect(provider->info().available, "test Lensfun database loads");
    if (!provider->info().available) {
        return;
    }
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

    const auto profile_candidates = provider->profile_candidates(metadata);
    expect(
        !profile_candidates.empty(),
        "Lensfun enumerates profiles compatible with a matched camera"
    );
    if (profile_candidates.empty()) {
        return;
    }

    auto pentax_metadata = metadata;
    pentax_metadata.make = "Pentax";
    pentax_metadata.model = "K10D";
    pentax_metadata.normalized_make = "Pentax";
    pentax_metadata.normalized_model = "K10D";
    expect(
        !provider->profile_candidates(pentax_metadata).empty(),
        "Lensfun profile enumeration accepts the Pentax K10D EXIF identity"
    );

    const auto input = optics_reference_buffer();
    const auto result = provider->correct_reference_rgb(
        input,
        metadata,
        image::default_optics_settings()
    );
    expect(
        result.receipt.status == image::OpticsProfileStatus::matched,
        "Lensfun matches the camera/lens profile from RAW metadata"
    );
    expect(result.receipt.applied_distortion, "Lensfun applies calibrated distortion correction");
    expect(result.receipt.applied_tca, "Lensfun applies calibrated TCA correction");
    expect(result.receipt.applied_vignetting, "Lensfun applies calibrated vignetting correction");
    expect(result.receipt.applied_scaling, "Lensfun auto-scale is applied with geometry correction");
    expect(
        result.corrected_reference_rgb.has_value(),
        "an active Lensfun profile materializes a corrected raster"
    );
    if (result.corrected_reference_rgb.has_value()) {
        expect(
            result.corrected_reference_rgb->dimensions == input.dimensions,
            "optics preserves the image canvas dimensions"
        );
        expect(
            result.corrected_reference_rgb->samples != input.samples,
            "profile correction changes the synthetic gradient"
        );
    }

    auto missing_distance_metadata = metadata;
    missing_distance_metadata.focus_distance_meters = 0.0;
    const auto far_distance_result = provider->correct_reference_rgb(
        input,
        missing_distance_metadata,
        image::default_optics_settings()
    );
    expect(
        far_distance_result.receipt.applied_vignetting,
        "Lensfun retains ordinary vignetting correction when focus distance is absent"
    );
    expect(
        far_distance_result.receipt.vignetting_used_distance_fallback,
        "Lensfun receipt discloses the far-distance vignetting approximation"
    );

    auto manual_metadata = metadata;
    manual_metadata.lens_make.clear();
    manual_metadata.lens_model.clear();
    auto manual_settings = image::default_optics_settings();
    manual_settings.camera_profile_maker = profile_candidates.front().camera_maker;
    manual_settings.camera_profile_model = profile_candidates.front().camera_model;
    manual_settings.lens_profile_maker = profile_candidates.front().lens_maker;
    manual_settings.lens_profile_model = profile_candidates.front().lens_model;
    const auto manual_result = provider->correct_reference_rgb(
        input,
        manual_metadata,
        manual_settings
    );
    expect(
        manual_result.receipt.status == image::OpticsProfileStatus::matched,
        "an explicit camera/lens profile works without EXIF lens identity"
    );
}

void largest_decodable_preview_wins() {
    const std::array previews{
        image::PreviewDescriptor{
            .id = 2,
            .format = image::PreviewFormat::bitmap,
            .dimensions = {160, 120},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 57'600,
            .decodable = true,
        },
        image::PreviewDescriptor{
            .id = 7,
            .format = image::PreviewFormat::jpeg,
            .dimensions = {3'872, 2'592},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 1'285'213,
            .decodable = true,
        },
        image::PreviewDescriptor{
            .id = 9,
            .format = image::PreviewFormat::unknown,
            .dimensions = {8'000, 6'000},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 2'000'000,
            .decodable = false,
        },
    };

    const auto selected = image::select_best_preview(previews);
    expect(selected.has_value(), "a decodable preview should be selected");
    expect(selected == 7U, "selection returns the provider preview id, not the vector index");
}

void no_decodable_preview_is_a_valid_state() {
    const std::array previews{
        image::PreviewDescriptor{
            .id = 0,
            .format = image::PreviewFormat::unknown,
            .dimensions = {},
            .bits_per_channel = 0,
            .channels = 0,
            .encoded_bytes = 0,
            .decodable = false,
        },
    };
    expect(
        !image::select_best_preview(previews).has_value(),
        "files without an embedded preview must remain importable"
    );
}

class FakeRgbSession final : public image::DecodeSession {
public:
    FakeRgbSession() {
        metadata_.raw_dimensions = {8U, 4U};
        metadata_.image_dimensions = {8U, 4U};
        capabilities_.metadata = true;
        capabilities_.reference_rgb = true;
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
        throw image::DecodeError(image::DecodeErrorCode::unsupported, 0, "no RAW frame");
    }

    [[nodiscard]] image::PixelBuffer render_reference_rgb() const override {
        ++reference_render_count_;
        image::PixelBuffer buffer;
        buffer.dimensions = {8, 4};
        buffer.bits_per_channel = 16;
        buffer.channels = 3;
        buffer.row_stride_bytes = 8U * 3U * sizeof(std::uint16_t);
        buffer.primaries = image::RgbPrimaries::srgb_rec709_d65;
        buffer.transfer_function = image::RgbTransferFunction::linear;
        buffer.reference = image::RgbBufferReference::processed_raw;
        buffer.samples.resize(8U * 4U * 3U);
        for (std::size_t index = 0; index < buffer.samples.size(); ++index) {
            buffer.samples[index] = static_cast<std::uint16_t>((index * 997U) % 65'536U);
        }
        return buffer;
    }

    [[nodiscard]] std::size_t reference_render_count() const noexcept {
        return reference_render_count_;
    }

private:
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
    mutable std::size_t reference_render_count_ = 0;
};

class FakeOpticsProvider final : public image::OpticsProvider {
public:
    [[nodiscard]] const image::OpticsProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] image::OpticsCorrectionResult correct_reference_rgb(
        const image::PixelBuffer& input,
        const image::AssetMetadata&,
        const image::OpticsSettings& settings
    ) const override {
        ++correction_count_;
        expect(settings.enabled, "pipeline sends enabled optics settings to its provider");
        auto corrected = input;
        std::fill(corrected.samples.begin(), corrected.samples.end(), 0U);
        // A third-party optics implementation may allocate or copy only raster pixels. The
        // source-development receipt is owned by preparation and must survive this behaviour.
        corrected.raw_development_receipt = {};
        return {
            .receipt = {
                .status = image::OpticsProfileStatus::matched,
                .provider_id = "fake-optics",
                .provider_version = "test-v1",
                .camera_profile = "Test camera",
                .lens_profile = "Test lens",
                .distortion_available = true,
                .applied_distortion = true,
            },
            .corrected_reference_rgb = std::move(corrected),
        };
    }

    [[nodiscard]] std::size_t correction_count() const noexcept {
        return correction_count_;
    }

private:
    image::OpticsProviderInfo info_{
        .id = "fake-optics",
        .version = "test-v1",
        .available = true,
    };
    mutable std::size_t correction_count_ = 0U;
};

void optics_runs_before_preview_and_full_detail_preparation() {
    const FakeRgbSession session;
    const FakeOpticsProvider optics;
    const auto warm = image::prepare_warm_edit_preview(session, 8U, &optics);
    expect(
        warm.optics_receipt().status == image::OpticsProfileStatus::matched,
        "warm preview retains the applied optics receipt"
    );
    expect(
        warm.optics_receipt().applied_distortion,
        "warm preview receives the provider's optical source"
    );
    expect(optics.correction_count() == 1U, "warm preview applies optics once during preparation");

    const auto detail = image::prepare_full_edit_detail(session, &optics);
    expect(
        detail.optics_receipt().status == image::OpticsProfileStatus::matched,
        "full detail retains the applied optics receipt"
    );
    expect(
        detail.optics_receipt().camera_profile == "Test camera",
        "full detail carries profile identity instead of a hidden transform"
    );
    expect(optics.correction_count() == 2U, "full detail prepares an independent immutable source");

    const std::array no_nodes{image::AdjustmentNode{
        .node_id = "neutral-exposure",
        .parameters = image::ExposureAdjustment{},
    }};
    const auto proxy = warm.render_jpeg(no_nodes);
    expect(!proxy.bytes.empty(), "optically prepared warm preview still encodes normally");
}

class BoundaryRgbSession final : public image::DecodeSession {
public:
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
        throw image::DecodeError(image::DecodeErrorCode::unsupported, 0, "no RAW frame");
    }

    [[nodiscard]] image::PixelBuffer render_reference_rgb() const override {
        ++reference_render_count_;
        return image::PixelBuffer{
            .dimensions = {5, 1},
            .bits_per_channel = 16,
            .channels = 3,
            .row_stride_bytes = 5U * 3U * sizeof(std::uint16_t),
            .primaries = image::RgbPrimaries::srgb_rec709_d65,
            .transfer_function = image::RgbTransferFunction::linear,
            .reference = image::RgbBufferReference::processed_raw,
            .samples = {
                0U, 0U, 0U,
                65'535U, 65'535U, 65'535U,
                65'535U, 0U, 0U,
                0U, 65'535U, 0U,
                0U, 0U, 65'535U,
            },
        };
    }

    [[nodiscard]] std::size_t reference_render_count() const noexcept {
        return reference_render_count_;
    }

private:
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
    mutable std::size_t reference_render_count_ = 0;
};

class RetainedRgbSession final : public image::DecodeSession {
public:
    explicit RetainedRgbSession(
        image::PixelBuffer buffer,
        image::AssetMetadata metadata = {}
    )
        : metadata_(std::move(metadata)), buffer_(std::move(buffer)) {
        if (metadata_.raw_dimensions.width == 0U || metadata_.raw_dimensions.height == 0U) {
            metadata_.raw_dimensions = buffer_.dimensions;
        }
        if (metadata_.image_dimensions.width == 0U || metadata_.image_dimensions.height == 0U) {
            metadata_.image_dimensions = buffer_.dimensions;
        }
        capabilities_.metadata = true;
        capabilities_.reference_rgb = true;
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
        throw image::DecodeError(image::DecodeErrorCode::unsupported, 0, "no RAW frame");
    }

    [[nodiscard]] image::PixelBuffer render_reference_rgb() const override {
        return buffer_;
    }

private:
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
    image::PixelBuffer buffer_;
};

void raw_development_receipt_survives_prepared_edit_sessions() {
    auto source = optics_reference_buffer(8U, 4U);
    const auto source_plan = image::default_raw_development_plan();
    source.raw_development_receipt = image::RawDevelopmentReceipt{
        .schema_version = image::raw_development_receipt_schema_version,
        .provider_id = "fixture-provider",
        .provider_version = "fixture-provider-v1",
        .library_version = "fixture-library-v1",
        .development_settings_signature = "fixture-request-v1",
        .requested_plan_identity = image::raw_development_plan_identity(source_plan),
        .effective_plan_identity = image::raw_development_plan_identity(source_plan),
        .requested_plan = source_plan,
        .effective_plan = source_plan,
        .plan_negotiation_status = image::RawDevelopmentPlanNegotiationStatus::accepted,
        .processed_linear_reference_contract_version = 7U,
        .declared_image_dimensions = {8U, 4U},
        .rendered_dimensions = {8U, 4U},
        .orientation = 5,
        .half_size = true,
        .use_camera_white_balance = true,
        .use_camera_matrix = true,
        .output_bits_per_channel = 16U,
        .output_color = 1,
        .gamma_inverse_power = 1.0,
        .gamma_linear_toe_slope = 1.0,
        .process_warnings = 0x1024U,
    };
    const RetainedRgbSession session(std::move(source));

    const auto warm = image::prepare_warm_edit_preview(session, 8U);
    expect(
        warm.raw_development_receipt().recorded()
            && warm.raw_development_receipt().provider_id == "fixture-provider"
            && warm.raw_development_receipt().half_size
            && warm.raw_development_receipt().process_warnings == 0x1024U,
        "warm preparation retains RAW development provenance after pixel conversion"
    );

    const auto detail = image::prepare_full_edit_detail(session);
    expect(
        detail.raw_development_receipt().recorded()
            && detail.raw_development_receipt().provider_version == "fixture-provider-v1"
            && detail.raw_development_receipt().orientation == 5
            && detail.raw_development_receipt().rendered_dimensions
                == image::Dimensions{8U, 4U}
            && detail.raw_development_receipt().effective_plan == source_plan,
        "full-detail preparation retains RAW development provenance with its source raster"
    );

    const FakeOpticsProvider discarding_optics;
    const auto detail_after_optics = image::prepare_full_edit_detail(session, &discarding_optics);
    expect(
        detail_after_optics.raw_development_receipt().uses_current_schema()
            && detail_after_optics.raw_development_receipt().provider_id == "fixture-provider"
            && detail_after_optics.raw_development_receipt().process_warnings == 0x1024U,
        "full-detail preparation retains source provenance when an optics provider replaces pixels"
    );
}

template <std::size_t Size>
[[nodiscard]] std::uint64_t sum_counts(const std::array<std::uint64_t, Size>& values) {
    std::uint64_t sum = 0U;
    for (const auto value : values) {
        sum += value;
    }
    return sum;
}

void reference_proxy_is_bounded_standard_jpeg() {
    expect(
        image::proxy_dimensions({4'032, 3'024}, 2'048) == image::Dimensions{2'048, 1'536},
        "proxy dimensions preserve aspect ratio and max edge"
    );

    const FakeRgbSession session;
    const auto proxy = image::render_reference_proxy_jpeg(
        session,
        image::ProxyRequest{.max_edge = 4, .jpeg_quality = 88}
    );
    expect(proxy.dimensions == image::Dimensions{4, 2}, "proxy renderer downsizes RGB");
    expect(proxy.format == image::PreviewFormat::jpeg, "proxy output is JPEG");
    expect(proxy.bytes.size() > 4U, "proxy JPEG is not empty");
    expect(
        proxy.bytes[0] == 0xffU && proxy.bytes[1] == 0xd8U,
        "proxy output starts with JPEG SOI"
    );
    expect(
        proxy.bytes[proxy.bytes.size() - 2U] == 0xffU && proxy.bytes.back() == 0xd9U,
        "proxy output ends with JPEG EOI"
    );
    expect(
        jpeg_uses_444_chroma_sampling(proxy.bytes),
        "interactive/reference JPEG proxies preserve 4:4:4 chroma sampling"
    );
}

void dng_baseline_exposure_is_a_consistent_source_rendering_step() {
    const image::PixelBuffer source{
        .dimensions = {2U, 1U},
        .bits_per_channel = 16U,
        .channels = 3U,
        .row_stride_bytes = 2U * 3U * sizeof(std::uint16_t),
        .primaries = image::RgbPrimaries::srgb_rec709_d65,
        .transfer_function = image::RgbTransferFunction::linear,
        .reference = image::RgbBufferReference::processed_raw,
        .samples = {
            8'192U, 8'192U, 8'192U,
            16'384U, 12'288U, 8'192U,
        },
    };
    const image::ProxyRequest request{.max_edge = 2U, .jpeg_quality = 100U};
    const std::array<image::AdjustmentNode, 0U> no_nodes{};

    const RetainedRgbSession no_baseline(source);
    const auto neutral_proxy = image::render_reference_proxy_jpeg(no_baseline, request);

    image::AssetMetadata dng_metadata;
    dng_metadata.dng_version = "1.6.0.0";
    dng_metadata.baseline_exposure = 1.0;
    const RetainedRgbSession dng_source(source, dng_metadata);
    const auto dng_proxy = image::render_reference_proxy_jpeg(dng_source, request);
    expect(
        dng_proxy.bytes != neutral_proxy.bytes,
        "a valid DNG BaselineExposure changes the source rendering before display encoding"
    );
    const auto dng_warm_proxy = image::render_edited_reference_proxy_jpeg(
        dng_source,
        no_nodes,
        request
    );
    expect(
        dng_warm_proxy.bytes == dng_proxy.bytes,
        "warm edit preview and the unedited DNG proxy share the baseline source rendering"
    );

    const auto neutral_detail = image::prepare_full_edit_detail(no_baseline).render_rgb8(
        no_nodes,
        image::DetailTileRect{.x = 0U, .y = 0U, .width = 2U, .height = 1U}
    );
    const auto dng_detail = image::prepare_full_edit_detail(dng_source).render_rgb8(
        no_nodes,
        image::DetailTileRect{.x = 0U, .y = 0U, .width = 2U, .height = 1U}
    );
    expect(
        dng_detail.bytes != neutral_detail.bytes,
        "full-detail tiles apply the same DNG source baseline before the edit graph"
    );

    auto invalid_dng_metadata = dng_metadata;
    invalid_dng_metadata.baseline_exposure = -999.0;
    const RetainedRgbSession missing_tag_sentinel(source, invalid_dng_metadata);
    expect(
        image::render_reference_proxy_jpeg(missing_tag_sentinel, request).bytes
            == neutral_proxy.bytes,
        "LibRaw's absent-DNG-BaselineExposure sentinel is ignored"
    );

    auto non_dng_metadata = dng_metadata;
    non_dng_metadata.dng_version.clear();
    const RetainedRgbSession non_dng_source(source, non_dng_metadata);
    expect(
        image::render_reference_proxy_jpeg(non_dng_source, request).bytes == neutral_proxy.bytes,
        "non-DNG RAW files never inherit a guessed DNG baseline exposure"
    );
}

void edited_proxy_applies_one_explicit_display_srgb_boundary() {
    const FakeRgbSession session;
    const image::ProxyRequest request{.max_edge = 8, .jpeg_quality = 90};
    const auto reference = image::render_reference_proxy_jpeg(session, request);
    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "exposure",
            .parameters = image::ExposureAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "contrast",
            .parameters = image::ContrastAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "tone-curve",
            .parameters = image::ToneCurve{},
        },
        image::AdjustmentNode{
            .node_id = "rgb-white-balance",
            .parameters = image::RgbWhiteBalanceAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{},
        },
    };
    const auto neutral = image::render_edited_reference_proxy_jpeg(
        session,
        neutral_nodes,
        request
    );
    expect(
        neutral.bytes == reference.bytes,
        "neutral edits share the one display-sRGB output transform with the reference path"
    );

    auto adjusted_nodes = neutral_nodes;
    adjusted_nodes[0].parameters = image::ExposureAdjustment{1.0};
    adjusted_nodes[3].parameters = image::RgbWhiteBalanceAdjustment{
        .temperature = 0.2,
        .tint = -0.05,
    };
    const image::ProxyRequest small_request{.max_edge = 4, .jpeg_quality = 90};
    const auto neutral_small = image::render_edited_reference_proxy_jpeg(
        session,
        neutral_nodes,
        small_request
    );
    const auto adjusted = image::render_edited_reference_proxy_jpeg(
        session,
        adjusted_nodes,
        small_request
    );
    expect(adjusted.dimensions == image::Dimensions{4, 2}, "edited preview remains bounded");
    expect(adjusted.bytes != neutral_small.bytes, "ordered edit nodes affect the encoded result");
    expect(
        adjusted.bytes.size() > 4U && adjusted.bytes[0] == 0xffU
            && adjusted.bytes[1] == 0xd8U
            && adjusted.bytes[adjusted.bytes.size() - 2U] == 0xffU
            && adjusted.bytes.back() == 0xd9U,
        "edited preview is a standard JPEG"
    );
}

void warm_edit_preview_decodes_once_and_renders_repeatedly() {
    const FakeRgbSession session;
    const auto warm = image::prepare_warm_edit_preview(session, 4);
    expect(session.reference_render_count() == 1U, "warm preparation renders the RAW once");
    expect(warm.dimensions() == image::Dimensions{4, 2}, "warm working proxy is max-edge bounded");
    expect(warm.max_edge() == 4U, "warm working proxy remembers its resource bound");

    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "exposure",
            .parameters = image::ExposureAdjustment{},
        },
    };
    auto adjusted_nodes = neutral_nodes;
    adjusted_nodes[0].parameters = image::ExposureAdjustment{1.0};

    const auto neutral = warm.render_jpeg(neutral_nodes, 90);
    const auto adjusted = warm.render_jpeg(adjusted_nodes, 90);
    expect(
        session.reference_render_count() == 1U,
        "repeated warm renders never ask the decoder for pixels again"
    );
    expect(neutral.dimensions == image::Dimensions{4, 2}, "warm output dimensions stay fixed");
    expect(adjusted.dimensions == neutral.dimensions, "all warm renders share working dimensions");
    expect(adjusted.bytes != neutral.bytes, "warm renders apply each requested edit independently");

    const auto one_shot_adjusted = image::render_edited_reference_proxy_jpeg(
        session,
        adjusted_nodes,
        image::ProxyRequest{.max_edge = 4, .jpeg_quality = 90}
    );
    expect(
        adjusted.bytes == one_shot_adjusted.bytes,
        "linear affine edits commute with the warm proxy's linear downsampling"
    );
    expect(session.reference_render_count() == 2U, "only the one-shot comparison decodes again");
}

void rotated_raw_preview_preserves_native_effect_radius() {
    // LibRaw returns its processed raster in output orientation. This fixture mirrors a camera
    // whose metadata still advertises an 8x4 sensor frame while the rendered RGB has been
    // transposed to 4x8. A matching already-oriented metadata fixture must produce exactly the
    // same native-pixel denoise footprint and therefore the same warm-preview bytes.
    const auto source = optics_reference_buffer(4U, 8U);
    image::AssetMetadata rotated_metadata;
    rotated_metadata.raw_dimensions = {8U, 4U};
    rotated_metadata.image_dimensions = {8U, 4U};
    rotated_metadata.orientation = 5;
    const RetainedRgbSession rotated(source, rotated_metadata);

    image::AssetMetadata canonical_metadata;
    canonical_metadata.raw_dimensions = {4U, 8U};
    canonical_metadata.image_dimensions = {4U, 8U};
    const RetainedRgbSession canonical(source, canonical_metadata);

    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "orientation-aware-native-denoise",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{
                .denoise_luminance = 0.7,
                .denoise_color = 0.3,
            },
        },
    };
    const auto rotated_proxy = image::prepare_warm_edit_preview(rotated, 8U).render_jpeg(
        nodes,
        100U
    );
    const auto canonical_proxy = image::prepare_warm_edit_preview(canonical, 8U).render_jpeg(
        nodes,
        100U
    );
    expect(
        rotated_proxy.bytes == canonical_proxy.bytes,
        "rotated RAW metadata uses the oriented full raster for native-radius effects"
    );
}

void warm_edit_preview_analysis_is_pre_jpeg_and_strictly_pre_clamp() {
    const BoundaryRgbSession session;
    const auto warm = image::prepare_warm_edit_preview(session, 5);
    expect(session.reference_render_count() == 1U, "analysis preparation decodes exactly once");

    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "neutral-exposure",
            .parameters = image::ExposureAdjustment{},
        },
    };
    const auto low_quality = warm.render_jpeg_with_analysis(neutral_nodes, 1);
    const auto high_quality = warm.render_jpeg_with_analysis(neutral_nodes, 100);
    const auto& neutral = low_quality.analysis;

    expect(
        neutral == high_quality.analysis,
        "JPEG quality cannot affect analysis computed from pre-encode RGB8"
    );
    expect(
        low_quality.proxy.bytes != high_quality.proxy.bytes,
        "the quality-independence check still exercises distinct JPEG encodings"
    );
    expect(
        neutral.sample_dimensions == image::Dimensions{5, 1} && neutral.pixel_count == 5U,
        "analysis describes the complete warm proxy"
    );
    expect(
        neutral.luma[0] >= 1U,
        "fixed-point Rec.709 encoded luma retains the black analysis endpoint"
    );
    expect(
        sum_counts(neutral.red) == neutral.pixel_count
            && sum_counts(neutral.green) == neutral.pixel_count
            && sum_counts(neutral.blue) == neutral.pixel_count
            && sum_counts(neutral.luma) == neutral.pixel_count,
        "every histogram contains exactly one sample per proxy pixel"
    );
    expect(
        neutral.below_zero_samples == std::array<std::uint64_t, 3>{0U, 0U, 0U}
            && neutral.above_one_samples == std::array<std::uint64_t, 3>{0U, 0U, 0U}
            && neutral.shadow_clipped_pixels == 0U
            && neutral.highlight_clipped_pixels == 0U,
        "exact scene-linear zero and one are legal and are not reported as clipped"
    );

    const std::array highlight_nodes{
        image::AdjustmentNode{
            .node_id = "highlight-exposure",
            .parameters = image::ExposureAdjustment{1.0},
        },
    };
    const auto highlight = warm.render_jpeg_with_analysis(highlight_nodes, 80).analysis;
    expect(
        highlight.above_one_samples == std::array<std::uint64_t, 3>{2U, 2U, 2U}
            && highlight.highlight_clipped_pixels == 4U,
        "super-white channels and their any-channel pixel union are counted independently"
    );

    image::ToneCurve lowered_curve;
    lowered_curve.points = {{0.0, -0.1}, {1.0, 0.9}};
    const std::array shadow_nodes{
        image::AdjustmentNode{
            .node_id = "lowered-curve",
            .parameters = std::move(lowered_curve),
        },
    };
    const auto shadow = warm.render_jpeg_with_analysis(shadow_nodes, 80).analysis;
    expect(
        shadow.below_zero_samples == std::array<std::uint64_t, 3>{3U, 3U, 3U}
            && shadow.shadow_clipped_pixels == 4U,
        "negative channels and their any-channel pixel union are counted independently"
    );
    expect(
        session.reference_render_count() == 1U,
        "repeated analyzed renders never ask the decoder for pixels again"
    );

    auto first_concurrent = std::async(std::launch::async, [&warm, &neutral_nodes]() {
        return warm.render_jpeg_with_analysis(neutral_nodes, 80);
    });
    auto second_concurrent = std::async(std::launch::async, [&warm, &neutral_nodes]() {
        return warm.render_jpeg_with_analysis(neutral_nodes, 80);
    });
    const auto first_result = first_concurrent.get();
    const auto second_result = second_concurrent.get();
    expect(
        first_result.analysis == second_result.analysis
            && first_result.proxy.bytes == second_result.proxy.bytes,
        "concurrent const analyzed renders are deterministic and isolated"
    );
}

void warm_edit_preview_bounds_fail_before_decode() {
    const FakeRgbSession session;
    try {
        static_cast<void>(image::prepare_warm_edit_preview(session, 0));
        expect(false, "zero warm edge must fail");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "zero warm edge reports an invalid request"
        );
    }
    try {
        static_cast<void>(image::prepare_warm_edit_preview(
            session,
            image::maximum_warm_edit_preview_edge + 1U
        ));
        expect(false, "oversized warm edge must fail");
    } catch (const image::DecodeError& error) {
        expect(
            error.code() == image::DecodeErrorCode::invalid_request,
            "oversized warm edge reports an invalid request"
        );
    }
    expect(
        session.reference_render_count() == 0U,
        "invalid warm bounds are rejected before decoder work"
    );
}

void edited_proxy_rejects_invalid_nodes_before_decode() {
    const FakeRgbSession session;
    const image::ProxyRequest request{.max_edge = 4, .jpeg_quality = 90};

    const auto rejects_before_decode = [&](const image::AdjustmentNode& invalid_node,
                                           const image::EditErrorCode expected_code,
                                           const std::string_view message) {
        const std::array nodes{invalid_node};
        try {
            static_cast<void>(image::render_edited_reference_proxy_jpeg(
                session,
                nodes,
                request
            ));
            expect(false, message);
        } catch (const image::EditError& error) {
            expect(error.code() == expected_code, message);
            expect(error.node_index() == 0U, "preflight errors retain node provenance");
        }
        expect(
            session.reference_render_count() == 0U,
            "adjustment preflight rejects invalid nodes before rendering reference RGB"
        );
    };

    rejects_before_decode(
        image::AdjustmentNode{
            .node_id = "non-finite-disabled-exposure",
            .enabled = false,
            .parameters = image::ExposureAdjustment{
                std::numeric_limits<double>::quiet_NaN(),
            },
        },
        image::EditErrorCode::invalid_parameter,
        "preflight validates numeric parameters even on disabled nodes"
    );

    rejects_before_decode(
        image::AdjustmentNode{
            .node_id = "future-version",
            .implementation_version = image::adjustment_implementation_version + 1U,
            .parameters = image::ExposureAdjustment{},
        },
        image::EditErrorCode::unsupported_version,
        "preflight rejects unsupported adjustment implementations"
    );

    image::ToneCurve overflowing_slope;
    overflowing_slope.points = {
        {0.0, 0.0},
        {
            std::numeric_limits<double>::min(),
            std::numeric_limits<double>::max(),
        },
        {1.0, 1.0},
    };
    rejects_before_decode(
        image::AdjustmentNode{
            .node_id = "overflowing-tone-curve-slope",
            .parameters = std::move(overflowing_slope),
        },
        image::EditErrorCode::invalid_parameter,
        "preflight rejects non-finite Tone Curve segment slopes"
    );
}

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
        version.find("linear=1") != std::string_view::npos,
        "provider identity versions the processed-linear reference RGB contract"
    );
    expect(
        version.find("receipt=2") != std::string_view::npos,
        "provider identity versions RAW-development provenance semantics"
    );
    expect(
        version.find("plan=1") != std::string_view::npos,
        "provider identity versions the RAW development plan contract"
    );
    expect(
        version.find("frame=1") != std::string_view::npos,
        "provider identity versions the owned RAW frame contract"
    );
    expect(
        version.find("display=5") != std::string_view::npos,
        "provider identity versions the display output transform for cache invalidation"
    );
    expect(
        version.find("settings=s1-w1-m1-a0-e0-") != std::string_view::npos,
        "provider identity includes a compact complete LibRaw development profile"
    );
}

void jpeg_raster_provider_uses_the_common_non_destructive_graph() {
    const auto provider = image::make_photo_decoder_provider();
    const auto session = provider->open(SHADOW_TEST_JPEG_PATH);
    expect(
        provider->info().id == "shadow-photo-router",
        "normal application photo entry point is provider-neutral"
    );
    expect(
        session->capabilities().metadata && session->capabilities().reference_rgb
            && !session->capabilities().raw_frame,
        "JPEG exposes metadata and editable RGB but never pretends to have a sensor RAW frame"
    );
    expect(
        session->previews().empty(),
        "JPEG source relies on the colour-managed generated proxy rather than an unrotated source byte preview"
    );
    const image::PixelBuffer decoded = session->render_reference_rgb_for_preview(1'024U);
    expect(
        decoded.reference == image::RgbBufferReference::decoded_raster
            && decoded.transfer_function == image::RgbTransferFunction::linear
            && decoded.primaries == image::RgbPrimaries::srgb_rec709_d65
            && decoded.bits_per_channel == 16U && decoded.channels == 3U,
        "JPEG is colour-managed into the common linear RGB contract without being labeled RAW"
    );
    expect(
        !decoded.raw_development_receipt.recorded(),
        "JPEG never fabricates a RAW development receipt"
    );

    const image::ProxyRequest request{.max_edge = 1'024U, .jpeg_quality = 90U};
    const auto reference = image::render_reference_proxy_jpeg(*session, request);
    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "neutral-raster-exposure",
            .parameters = image::ExposureAdjustment{},
        },
    };
    const auto edited = image::render_edited_reference_proxy_jpeg(
        *session,
        neutral_nodes,
        request
    );
    expect(
        edited.bytes == reference.bytes,
        "JPEG follows the exact same neutral edit graph and display boundary as its reference proxy"
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
        expect(false, "LibRaw provider rejects an output depth outside the reference contract");
    } catch (const std::invalid_argument&) {
    }
}

void real_libraw_boundary_and_neutral_preview_when_configured() {
    const char* fixture = std::getenv("SHADOW_TEST_DNG");
    if (fixture == nullptr || *fixture == '\0') {
        return;
    }

    const auto provider = image::make_libraw_decoder_provider();
    const auto decoder = provider->open(fixture);
    expect(
        decoder->capabilities().raw_frame
            && decoder->raw_development_capabilities().raw_frame,
        "real LibRaw RAW source advertises the owned RawFrame contract"
    );
    if (decoder->capabilities().raw_frame) {
        const auto frame = decoder->decode_raw_frame();
        expect(frame.valid(), "real LibRaw RAW frame preserves a complete owned sample plane");
        expect(
            frame.descriptor.storage_dimensions == decoder->metadata().raw_dimensions
                && frame.descriptor.active_dimensions == decoder->metadata().image_dimensions
                && frame.descriptor.active_margins == decoder->metadata().margins,
            "real LibRaw RAW frame retains exact sensor storage and active-area geometry"
        );
        expect(
            frame.descriptor.declared_pending_corrections
                == decoder->capabilities().pending_corrections,
            "real LibRaw RAW frame records DNG corrections without claiming they were applied"
        );
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
        "real LibRaw boundary cannot be mistaken for untouched sensor-linear data"
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
        "receipt exposes the fixed linear output transfer and output format request"
    );
    expect(
        receipt.declared_dng_opcode_lists == decoder->capabilities().pending_corrections,
        "receipt preserves declared DNG opcode lists alongside LibRaw processing warnings"
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
    const image::ProxyRequest request{.max_edge = source_edge, .jpeg_quality = 90};
    const auto reference = image::render_reference_proxy_jpeg(retained, request);
    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "neutral-real-exposure",
            .parameters = image::ExposureAdjustment{},
        },
    };
    const auto neutral = image::render_edited_reference_proxy_jpeg(
        retained,
        neutral_nodes,
        request
    );
    expect(
        neutral.bytes == reference.bytes,
        "real processed-linear pixels follow one identical neutral display transform"
    );
}

} // namespace

int main() {
    pending_corrections_are_explicit();
    raw_development_receipt_is_explicitly_absent_until_a_provider_records_it();
    raw_frame_is_owned_unprocessed_and_bayer_guarded();
    bayer_bilinear_demosaic_keeps_the_sensor_domain_explicit();
    raw_development_plan_is_canonical_and_capability_negotiated();
    icc_color_management_is_content_addressed_and_transfer_aware();
    private_decoder_plugin_abi_is_explicit_and_fail_closed();
    private_decoder_plugin_loads_an_explicit_local_module();
    private_decoder_router_prefers_an_explicit_local_module();
    optics_settings_are_explicit_and_provider_safe();
    lensfun_adapter_applies_a_real_profile_when_a_test_database_is_available();
    optics_runs_before_preview_and_full_detail_preparation();
    largest_decodable_preview_wins();
    no_decodable_preview_is_a_valid_state();
    raw_development_receipt_survives_prepared_edit_sessions();
    reference_proxy_is_bounded_standard_jpeg();
    dng_baseline_exposure_is_a_consistent_source_rendering_step();
    edited_proxy_applies_one_explicit_display_srgb_boundary();
    warm_edit_preview_decodes_once_and_renders_repeatedly();
    rotated_raw_preview_preserves_native_effect_radius();
    warm_edit_preview_analysis_is_pre_jpeg_and_strictly_pre_clamp();
    warm_edit_preview_bounds_fail_before_decode();
    edited_proxy_rejects_invalid_nodes_before_decode();
    provider_identity_versions_shadow_pixel_contracts();
    jpeg_raster_provider_uses_the_common_non_destructive_graph();
    libraw_development_settings_are_explicit_and_cache_visible();
    real_libraw_boundary_and_neutral_preview_when_configured();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
