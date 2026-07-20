#include <shadow/image/cxx_bridge.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::bridge {

namespace {

[[nodiscard]] FfiDimensions dimensions(const image::Dimensions value) noexcept {
    return FfiDimensions{value.width, value.height};
}

[[nodiscard]] FfiPreviewFormat preview_format(const image::PreviewFormat value) noexcept {
    switch (value) {
    case image::PreviewFormat::jpeg:
        return FfiPreviewFormat::Jpeg;
    case image::PreviewFormat::bitmap:
        return FfiPreviewFormat::Bitmap;
    case image::PreviewFormat::jpeg_xl:
        return FfiPreviewFormat::JpegXl;
    case image::PreviewFormat::h265:
        return FfiPreviewFormat::H265;
    case image::PreviewFormat::unknown:
        return FfiPreviewFormat::Unknown;
    }
    return FfiPreviewFormat::Unknown;
}

} // namespace

DecodeHandle::DecodeHandle(
    std::unique_ptr<image::DecoderProvider> provider,
    std::unique_ptr<image::DecodeSession> session
)
    : provider_(std::move(provider)), session_(std::move(session)) {}

DecodeHandle::~DecodeHandle() = default;

FfiProviderSnapshot DecodeHandle::provider() const {
    const auto& info = provider_->info();
    FfiProviderSnapshot snapshot;
    snapshot.id = rust::String(info.id);
    snapshot.version = rust::String(info.version);
    snapshot.dng_sdk = info.dng_sdk;
    snapshot.rawspeed = info.rawspeed;
    snapshot.jpeg = info.jpeg;
    return snapshot;
}

FfiMetadataSnapshot DecodeHandle::metadata() const {
    const auto& metadata = session_->metadata();
    FfiMetadataSnapshot snapshot;
    snapshot.make = rust::String(metadata.make);
    snapshot.model = rust::String(metadata.model);
    snapshot.normalized_make = rust::String(metadata.normalized_make);
    snapshot.normalized_model = rust::String(metadata.normalized_model);
    snapshot.dng_version = rust::String(metadata.dng_version);
    snapshot.raw_count = metadata.raw_count;
    snapshot.raw_dimensions = dimensions(metadata.raw_dimensions);
    snapshot.image_dimensions = dimensions(metadata.image_dimensions);
    snapshot.margins = FfiMargins{
        metadata.margins.left,
        metadata.margins.top,
        metadata.margins.right,
        metadata.margins.bottom,
    };
    snapshot.orientation = metadata.orientation;
    snapshot.cfa_pattern = rust::String(metadata.cfa_pattern);
    snapshot.sensor_colors = metadata.sensor_colors;
    snapshot.sensor_bits = metadata.sensor_bits;
    snapshot.black_level = metadata.black_level;
    snapshot.white_level = metadata.white_level;
    snapshot.as_shot_neutral_r = metadata.as_shot_neutral[0];
    snapshot.as_shot_neutral_g1 = metadata.as_shot_neutral[1];
    snapshot.as_shot_neutral_b = metadata.as_shot_neutral[2];
    snapshot.as_shot_neutral_g2 = metadata.as_shot_neutral[3];
    snapshot.baseline_exposure = metadata.baseline_exposure;
    return snapshot;
}

FfiCapabilitySnapshot DecodeHandle::capabilities() const {
    const auto& capabilities = session_->capabilities();
    const auto& opcode_bytes = capabilities.pending_corrections.dng_opcode_list_bytes;
    return FfiCapabilitySnapshot{
        capabilities.metadata,
        capabilities.embedded_previews,
        capabilities.mosaic,
        capabilities.reference_rgb,
        opcode_bytes[0],
        opcode_bytes[1],
        opcode_bytes[2],
    };
}

rust::Vec<FfiPreviewSnapshot> DecodeHandle::previews() const {
    rust::Vec<FfiPreviewSnapshot> snapshots;
    snapshots.reserve(session_->previews().size());
    for (const auto& preview : session_->previews()) {
        snapshots.push_back(FfiPreviewSnapshot{
            preview.id,
            preview_format(preview.format),
            dimensions(preview.dimensions),
            preview.bits_per_channel,
            preview.channels,
            preview.encoded_bytes,
            preview.decodable,
        });
    }
    return snapshots;
}

std::unique_ptr<DecodeHandle> open_libraw_utf8(const rust::Str path) {
    const std::string_view utf8_bytes(path.data(), path.size());
    std::u8string utf8_path;
    utf8_path.reserve(utf8_bytes.size());
    for (const char byte : utf8_bytes) {
        utf8_path.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    auto provider = image::make_libraw_decoder_provider();
    auto session = provider->open(std::filesystem::path(utf8_path));
    return std::make_unique<DecodeHandle>(std::move(provider), std::move(session));
}

} // namespace shadow::bridge
