#include <shadow/image/cxx_bridge.hpp>

#include <shadow/image/edit.hpp>

#include <array>
#include <cmath>
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

[[nodiscard]] FfiByteOrder byte_order(const image::ByteOrder value) noexcept {
    switch (value) {
    case image::ByteOrder::native:
        return FfiByteOrder::Native;
    case image::ByteOrder::little_endian:
        return FfiByteOrder::LittleEndian;
    case image::ByteOrder::big_endian:
        return FfiByteOrder::BigEndian;
    case image::ByteOrder::not_applicable:
        return FfiByteOrder::NotApplicable;
    }
    return FfiByteOrder::NotApplicable;
}

[[nodiscard]] FfiPreviewSnapshot preview_snapshot(const image::PreviewDescriptor& preview) {
    return FfiPreviewSnapshot{
        preview.id,
        preview_format(preview.format),
        dimensions(preview.dimensions),
        preview.bits_per_channel,
        preview.channels,
        preview.encoded_bytes,
        preview.decodable,
    };
}

[[nodiscard]] FfiEncodedProxy encoded_proxy(const image::EncodedProxy& proxy) {
    FfiEncodedProxy result;
    result.dimensions = dimensions(proxy.dimensions);
    result.format = preview_format(proxy.format);
    result.bits_per_channel = proxy.bits_per_channel;
    result.channels = proxy.channels;
    result.bytes.reserve(proxy.bytes.size());
    for (const auto byte : proxy.bytes) {
        result.bytes.push_back(byte);
    }
    return result;
}

void validate_basic_edit_parameter(
    const double value,
    const double minimum,
    const double maximum,
    const std::string_view name,
    const bool minimum_is_inclusive = true
) {
    const bool below_minimum = minimum_is_inclusive ? value < minimum : value <= minimum;
    if (!std::isfinite(value) || below_minimum || value > maximum) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "edited proxy " + std::string(name) + " is outside the supported range"
        );
    }
}

void validate_basic_edits(const FfiBasicEditRequest& request) {
    validate_basic_edit_parameter(request.exposure_stops, -16.0, 16.0, "exposure stops");
    validate_basic_edit_parameter(request.contrast_factor, 0.0, 8.0, "contrast factor");
    validate_basic_edit_parameter(
        request.red_channel_gain,
        0.0,
        16.0,
        "red channel gain",
        false
    );
    validate_basic_edit_parameter(
        request.green_channel_gain,
        0.0,
        16.0,
        "green channel gain",
        false
    );
    validate_basic_edit_parameter(
        request.blue_channel_gain,
        0.0,
        16.0,
        "blue channel gain",
        false
    );
    validate_basic_edit_parameter(request.saturation_factor, 0.0, 8.0, "saturation factor");
}

[[nodiscard]] std::array<image::AdjustmentNode, 4> basic_edit_nodes(
    const FfiBasicEditRequest& request
) {
    // These are resolved post-demosaic scene-linear RGB gains, not camera-domain RAW white
    // balance coefficients. Keeping the node name honest prevents a lossy API promise.
    return {
        image::AdjustmentNode{
            .node_id = "basic-exposure",
            .parameters = image::ExposureAdjustment{request.exposure_stops},
        },
        image::AdjustmentNode{
            .node_id = "basic-contrast",
            .parameters = image::ContrastAdjustment{request.contrast_factor, 0.18},
        },
        image::AdjustmentNode{
            .node_id = "basic-channel-gain",
            .parameters = image::ChannelGainAdjustment{
                {
                    request.red_channel_gain,
                    request.green_channel_gain,
                    request.blue_channel_gain,
                }
            },
        },
        image::AdjustmentNode{
            .node_id = "basic-saturation",
            .parameters = image::SaturationAdjustment{request.saturation_factor},
        },
    };
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
        snapshots.push_back(preview_snapshot(preview));
    }
    return snapshots;
}

FfiPreviewPayload DecodeHandle::decode_best_preview() {
    const auto selected = image::select_best_preview(session_->previews());
    FfiPreviewPayload result;
    if (!selected.has_value()) {
        result.present = false;
        return result;
    }

    auto payload = session_->decode_preview(*selected);
    result.present = true;
    result.descriptor = preview_snapshot(payload.descriptor);
    result.byte_order = byte_order(payload.byte_order);
    result.bytes.reserve(payload.bytes.size());
    for (const auto byte : payload.bytes) {
        result.bytes.push_back(byte);
    }
    return result;
}

FfiEncodedProxy DecodeHandle::render_reference_proxy(
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality
) const {
    const auto proxy = image::render_reference_proxy_jpeg(
        *session_,
        image::ProxyRequest{max_edge, jpeg_quality}
    );
    return encoded_proxy(proxy);
}

FfiEncodedProxy DecodeHandle::render_edited_reference_proxy(
    const FfiBasicEditRequest& request
) const {
    validate_basic_edits(request);
    const auto nodes = basic_edit_nodes(request);
    const auto proxy = image::render_edited_reference_proxy_jpeg(
        *session_,
        nodes,
        image::ProxyRequest{request.max_edge, request.jpeg_quality}
    );
    return encoded_proxy(proxy);
}

std::unique_ptr<EditPreviewHandle> DecodeHandle::prepare_edit_preview(
    const std::uint32_t max_edge
) const {
    return std::make_unique<EditPreviewHandle>(
        image::prepare_warm_edit_preview(*session_, max_edge)
    );
}

EditPreviewHandle::EditPreviewHandle(image::WarmEditPreviewSession session)
    : session_(std::move(session)) {}

EditPreviewHandle::~EditPreviewHandle() = default;

FfiDimensions EditPreviewHandle::dimensions() const noexcept {
    return shadow::bridge::dimensions(session_.dimensions());
}

std::uint32_t EditPreviewHandle::max_edge() const noexcept {
    return session_.max_edge();
}

FfiEncodedProxy EditPreviewHandle::render_basic_edits(
    const FfiBasicEditRequest& request
) const {
    validate_basic_edits(request);
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto nodes = basic_edit_nodes(request);
    return encoded_proxy(session_.render_jpeg(nodes, request.jpeg_quality));
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

rust::String libraw_provider_version() {
    const auto provider = image::make_libraw_decoder_provider();
    return rust::String(provider->info().version);
}

} // namespace shadow::bridge
