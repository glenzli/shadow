#include <shadow/image/private_decoder_plugin.hpp>

#include <memory>
#include <span>

namespace shadow::image {

namespace {

class TestSession final : public DecodeSession {
public:
    TestSession() {
        metadata_.make = "Shadow";
        metadata_.model = "Private decoder test fixture";
        metadata_.image_dimensions = {2U, 1U};
        metadata_.raw_dimensions = metadata_.image_dimensions;
        capabilities_.metadata = true;
        capabilities_.reference_rgb = true;
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

    [[nodiscard]] MosaicBuffer decode_mosaic() override {
        return {};
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
        result.raw_development_receipt = RawDevelopmentReceipt{
            .schema_version = raw_development_receipt_schema_version,
            .provider_id = "spoofed-provider",
            .provider_version = "spoofed-version",
            .library_version = "private-fixture-sdk",
        };
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
        result.raw_development_receipt = RawDevelopmentReceipt{
            .schema_version = raw_development_receipt_schema_version,
            .provider_id = "spoofed-provider",
            .provider_version = "spoofed-version",
        };
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
    .plugin_id = "test-private-provider",
    .plugin_version = "1.0.0",
};

} // namespace

extern "C" const PrivateDecoderPluginDescriptor*
shadow_private_decoder_plugin_descriptor_v2() {
    return &descriptor;
}

extern "C" DecoderProvider* shadow_create_private_decoder_provider_v2() {
    return new TestProvider();
}

extern "C" void shadow_destroy_private_decoder_provider_v2(DecoderProvider* provider) {
    delete provider;
}

} // namespace shadow::image
