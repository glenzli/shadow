#include <shadow/image/cxx_bridge.hpp>

#include <shadow/image/edit.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

inline constexpr std::size_t maximum_adjustment_nodes = 256U;
inline constexpr std::size_t maximum_adjustment_node_id_bytes = 256U;

[[noreturn]] void throw_invalid_adjustment_plan(std::string message) {
    throw image::DecodeError(
        image::DecodeErrorCode::invalid_request,
        0,
        std::move(message)
    );
}

void require_parameter_count(
    const FfiAdjustmentNode& node,
    const std::size_t expected,
    const std::string_view operation
) {
    if (node.parameters.size() != expected) {
        throw_invalid_adjustment_plan(
            "adjustment node " + std::string(operation) + " requires exactly "
            + std::to_string(expected) + " parameters"
        );
    }
}

[[nodiscard]] image::AdjustmentNode adjustment_node(const FfiAdjustmentNode& source) {
    if (
        source.node_id.empty()
        || source.node_id.size() > maximum_adjustment_node_id_bytes
    ) {
        throw_invalid_adjustment_plan(
            "adjustment node id must contain between 1 and 256 UTF-8 bytes"
        );
    }

    image::AdjustmentNode result{
        .node_id = std::string(source.node_id.data(), source.node_id.size()),
        .parameter_schema_version = source.parameter_schema_version,
        .implementation_version = source.implementation_version,
        .enabled = source.enabled,
    };

    switch (source.operation) {
    case FfiAdjustmentOperation::Exposure:
        require_parameter_count(source, 1U, "exposure");
        result.parameters = image::ExposureAdjustment{source.parameters[0]};
        break;
    case FfiAdjustmentOperation::Contrast:
        require_parameter_count(source, 2U, "contrast");
        result.parameters = image::ContrastAdjustment{
            source.parameters[0],
            source.parameters[1],
        };
        break;
    case FfiAdjustmentOperation::ToneCurve: {
        if (source.parameters.size() % 2U != 0U) {
            throw_invalid_adjustment_plan(
                "tone curve parameters must contain flattened x/y pairs"
            );
        }
        const std::size_t point_count = source.parameters.size() / 2U;
        if (point_count < 2U || point_count > image::maximum_tone_curve_points) {
            throw_invalid_adjustment_plan(
                "tone curve must contain between 2 and 256 control points"
            );
        }

        image::ToneCurve curve{
            .parameter_schema_version = source.parameter_schema_version,
            .implementation_version = source.implementation_version,
            .points = {},
        };
        curve.points.reserve(point_count);
        for (std::size_t index = 0U; index < source.parameters.size(); index += 2U) {
            curve.points.push_back(image::ToneCurvePoint{
                source.parameters[index],
                source.parameters[index + 1U],
            });
        }
        result.parameters = std::move(curve);
        break;
    }
    case FfiAdjustmentOperation::ChannelGain:
        require_parameter_count(source, 3U, "channel gain");
        result.parameters = image::ChannelGainAdjustment{{
            source.parameters[0],
            source.parameters[1],
            source.parameters[2],
        }};
        break;
    case FfiAdjustmentOperation::Saturation:
        require_parameter_count(source, 1U, "saturation");
        result.parameters = image::SaturationAdjustment{source.parameters[0]};
        break;
    default:
        throw_invalid_adjustment_plan("adjustment node operation is unsupported");
    }

    return result;
}

[[nodiscard]] std::vector<image::AdjustmentNode> adjustment_nodes(
    const FfiAdjustmentRenderRequest& request
) {
    if (request.nodes.empty() || request.nodes.size() > maximum_adjustment_nodes) {
        throw_invalid_adjustment_plan(
            "adjustment render plan must contain between 1 and 256 nodes"
        );
    }

    std::vector<image::AdjustmentNode> result;
    result.reserve(request.nodes.size());
    for (const auto& source : request.nodes) {
        result.push_back(adjustment_node(source));
    }
    return result;
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

FfiEncodedProxy DecodeHandle::render_adjustment_plan(
    const FfiAdjustmentRenderRequest& request
) const {
    const auto nodes = adjustment_nodes(request);
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

FfiEncodedProxy EditPreviewHandle::render_adjustment_plan(
    const FfiAdjustmentRenderRequest& request
) const {
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto nodes = adjustment_nodes(request);
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
