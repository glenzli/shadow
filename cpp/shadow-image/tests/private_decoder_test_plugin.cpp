#include <shadow/image/private_decoder_plugin.hpp>

#include <memory>
#include <span>

namespace shadow::image {

namespace {

[[nodiscard]] RawDevelopmentCapabilities fixture_raw_development_capabilities() noexcept {
    RawDevelopmentCapabilities capabilities;
    capabilities.schema_version = raw_development_capabilities_schema_version;
    capabilities.available = true;
    capabilities.raw_frame = true;
    capabilities.dng_opcode_execution_receipt = true;
    capabilities.supported_intents = raw_development_intent_mask(RawDevelopmentIntent::preview)
        | raw_development_intent_mask(RawDevelopmentIntent::detail)
        | raw_development_intent_mask(RawDevelopmentIntent::export_image);
    capabilities.supported_qualities = raw_development_quality_mask(
        RawDevelopmentQuality::balanced
    );
    capabilities.supported_dng_opcode_policies = dng_opcode_policy_mask(
        DngOpcodePolicy::provider_default
    );
    capabilities.supported_noise_reduction_intents = raw_noise_reduction_intent_mask(
        RawNoiseReductionIntent::provider_default
    );
    capabilities.supported_highlight_recovery_intents = raw_highlight_recovery_intent_mask(
        RawHighlightRecoveryIntent::provider_default
    );
    return capabilities;
}

[[nodiscard]] RawDevelopmentReceipt fixture_receipt(
    const RawDevelopmentPlan& plan,
    const bool half_size = false
) {
    return RawDevelopmentReceipt{
        .schema_version = raw_development_receipt_schema_version,
        .provider_id = "spoofed-provider",
        .provider_version = "spoofed-version",
        .library_version = "private-fixture-sdk",
        .requested_plan_identity = raw_development_plan_identity(plan),
        .effective_plan_identity = raw_development_plan_identity(plan),
        .requested_plan = plan,
        .effective_plan = plan,
        .plan_negotiation_status = RawDevelopmentPlanNegotiationStatus::accepted,
        .half_size = half_size,
    };
}

class TestSession final : public DecodeSession {
public:
    TestSession() {
        metadata_.make = "Shadow";
        metadata_.model = "Private decoder test fixture";
        metadata_.image_dimensions = {2U, 1U};
        metadata_.raw_dimensions = metadata_.image_dimensions;
        capabilities_.metadata = true;
        capabilities_.reference_rgb = true;
        capabilities_.raw_development = fixture_raw_development_capabilities();
    }

    [[nodiscard]] const AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const PreviewDescriptor> previews() const noexcept override {
        return {};
    }

    [[nodiscard]] PreviewPayload decode_preview(std::size_t) override {
        return {};
    }

    [[nodiscard]] RawFrame decode_raw_frame() override {
        RawFrame frame;
        frame.descriptor.storage_dimensions = {2U, 2U};
        frame.descriptor.active_dimensions = {2U, 2U};
        frame.descriptor.active_margins = {};
        frame.descriptor.orientation = 1;
        frame.descriptor.sample_encoding = RawFrameSampleEncoding::uint16_native;
        frame.descriptor.cfa_layout = RawFrameCfaLayout::bayer_2x2;
        frame.descriptor.bayer_2x2 = {
            RawCfaColor::red,
            RawCfaColor::green,
            RawCfaColor::green,
            RawCfaColor::blue,
        };
        frame.descriptor.cfa_pattern = "RGGB";
        frame.descriptor.bits_per_sample = 12U;
        frame.descriptor.black_levels = {64U, 64U, 64U, 64U};
        frame.descriptor.white_levels = {4'095U, 4'095U, 4'095U, 4'095U};
        frame.descriptor.as_shot_neutral = {0.5, 1.0, 1.0, 0.25};
        frame.descriptor.camera_to_xyz_d50 = {
            0.70, 0.20, 0.10,
            0.10, 0.80, 0.10,
            0.05, 0.15, 0.80,
        };
        frame.descriptor.has_camera_to_xyz_d50 = true;
        frame.samples = {1'024U, 1'100U, 1'100U, 900U};
        return frame;
    }

    [[nodiscard]] PixelBuffer render_reference_rgb() const override {
        PixelBuffer result;
        result.dimensions = metadata_.image_dimensions;
        result.bits_per_channel = 16U;
        result.channels = 3U;
        result.row_stride_bytes = 2U * 3U * sizeof(std::uint16_t);
        result.primaries = RgbPrimaries::srgb_rec709_d65;
        result.transfer_function = RgbTransferFunction::linear;
        result.reference = RgbBufferReference::processed_raw;
        result.samples = {0U, 1U, 2U, 3U, 4U, 5U};
        // Deliberately use a non-host identity: the public adapter must bind a recorded receipt
        // to its namespaced provider identity before it reaches the caller.
        result.raw_development_receipt = fixture_receipt(default_raw_development_plan());
        return result;
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge
    ) const override {
        if (max_edge != 1U) {
            return render_reference_rgb();
        }
        PixelBuffer result;
        result.dimensions = {1U, 1U};
        result.bits_per_channel = 16U;
        result.channels = 3U;
        result.row_stride_bytes = 3U * sizeof(std::uint16_t);
        result.primaries = RgbPrimaries::srgb_rec709_d65;
        result.transfer_function = RgbTransferFunction::linear;
        result.reference = RgbBufferReference::processed_raw;
        result.samples = {9U, 8U, 7U};
        result.raw_development_receipt = fixture_receipt(
            preview_raw_development_plan(),
            true
        );
        return result;
    }

private:
    AssetMetadata metadata_;
    DecodeCapabilities capabilities_;
};

class TestProvider final : public DecoderProvider {
public:
    TestProvider() {
        info_.id = "fixture";
        info_.version = "1.0.0";
    }

    [[nodiscard]] const ProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] std::unique_ptr<DecodeSession> open(const std::filesystem::path&) const override {
        return std::make_unique<TestSession>();
    }

private:
    ProviderInfo info_;
};

constexpr PrivateDecoderPluginDescriptor descriptor{
    .abi_version = private_decoder_plugin_abi_version,
    .raw_development_plan_schema_version = raw_development_plan_schema_version,
    .raw_frame_schema_version = raw_frame_schema_version,
    .plugin_id = "test-private-provider",
    .plugin_version = "1.0.0",
};

} // namespace

extern "C" std::uint64_t shadow_private_decoder_plugin_interface_contract_v1() {
    return private_decoder_plugin_interface_contract_token;
}

extern "C" const PrivateDecoderPluginDescriptor*
shadow_private_decoder_plugin_descriptor_v1() {
    return &descriptor;
}

extern "C" DecoderProvider* shadow_create_private_decoder_provider_v1() {
    return new TestProvider();
}

extern "C" void shadow_destroy_private_decoder_provider_v1(DecoderProvider* provider) {
    delete provider;
}

} // namespace shadow::image
