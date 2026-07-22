#include <shadow/image/cxx_bridge.hpp>

#include <shadow/image/edit.hpp>
#include <shadow/image/display_luma.hpp>

#include <filesystem>
#include <limits>
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

template <std::size_t Size>
[[nodiscard]] rust::Vec<std::uint64_t> sample_counts(
    const std::array<std::uint64_t, Size>& source
) {
    rust::Vec<std::uint64_t> result;
    result.reserve(source.size());
    for (const auto count : source) {
        result.push_back(count);
    }
    return result;
}

[[nodiscard]] FfiEditPreviewAnalysis edit_preview_analysis(
    const image::EditPreviewAnalysis& analysis
) {
    FfiEditPreviewAnalysis result;
    result.version = rust::String(
        image::edit_preview_analysis_version.data(),
        image::edit_preview_analysis_version.size()
    );
    result.sample_dimensions = dimensions(analysis.sample_dimensions);
    result.red = sample_counts(analysis.red);
    result.green = sample_counts(analysis.green);
    result.blue = sample_counts(analysis.blue);
    result.luma = sample_counts(analysis.luma);
    result.below_zero_samples = sample_counts(analysis.below_zero_samples);
    result.above_one_samples = sample_counts(analysis.above_one_samples);
    result.pixel_count = analysis.pixel_count;
    result.shadow_clipped_pixels = analysis.shadow_clipped_pixels;
    result.highlight_clipped_pixels = analysis.highlight_clipped_pixels;
    return result;
}

[[nodiscard]] FfiAnalyzedEditPreview analyzed_edit_preview(
    const image::AnalyzedEditPreview& preview
) {
    FfiAnalyzedEditPreview result;
    result.proxy = encoded_proxy(preview.proxy);
    result.analysis = edit_preview_analysis(preview.analysis);
    return result;
}

[[nodiscard]] FfiDetailTileRect detail_tile_rect(const image::DetailTileRect value) noexcept {
    return FfiDetailTileRect{value.x, value.y, value.width, value.height};
}

[[nodiscard]] image::DetailTileRect detail_tile_rect(const FfiDetailTileRect& value) noexcept {
    return image::DetailTileRect{value.x, value.y, value.width, value.height};
}

[[nodiscard]] FfiRenderedDetailTile rendered_detail_tile(
    const image::RenderedDetailTile& tile
) {
    FfiRenderedDetailTile result;
    result.rect = detail_tile_rect(tile.rect);
    result.full_dimensions = dimensions(tile.full_dimensions);
    result.row_stride_bytes = tile.row_stride_bytes;
    result.bytes.reserve(tile.bytes.size());
    for (const auto byte : tile.bytes) {
        result.bytes.push_back(byte);
    }
    return result;
}

[[nodiscard]] rust::String optics_status(const image::OpticsProfileStatus status) {
    switch (status) {
    case image::OpticsProfileStatus::disabled:
        return rust::String("disabled");
    case image::OpticsProfileStatus::provider_unavailable:
        return rust::String("provider_unavailable");
    case image::OpticsProfileStatus::insufficient_metadata:
        return rust::String("insufficient_metadata");
    case image::OpticsProfileStatus::camera_not_found:
        return rust::String("camera_not_found");
    case image::OpticsProfileStatus::lens_not_found:
        return rust::String("lens_not_found");
    case image::OpticsProfileStatus::incompatible_input:
        return rust::String("incompatible_input");
    case image::OpticsProfileStatus::matched:
        return rust::String("matched");
    }
    return rust::String("provider_unavailable");
}

[[nodiscard]] FfiOpticsReceipt optics_receipt(const image::OpticsProfileReceipt& receipt) {
    FfiOpticsReceipt result;
    result.status = optics_status(receipt.status);
    result.provider_id = rust::String(receipt.provider_id);
    result.provider_version = rust::String(receipt.provider_version);
    result.camera_profile = rust::String(receipt.camera_profile);
    result.lens_profile = rust::String(receipt.lens_profile);
    result.distortion_available = receipt.distortion_available;
    result.tca_available = receipt.tca_available;
    result.vignetting_available = receipt.vignetting_available;
    result.applied_distortion = receipt.applied_distortion;
    result.applied_tca = receipt.applied_tca;
    result.applied_vignetting = receipt.applied_vignetting;
    result.vignetting_used_distance_fallback = receipt.vignetting_used_distance_fallback;
    result.applied_scaling = receipt.applied_scaling;
    return result;
}

[[nodiscard]] FfiRawDevelopmentReceipt raw_development_receipt(
    const image::RawDevelopmentReceipt& receipt
) {
    FfiRawDevelopmentReceipt result;
    result.schema_version = receipt.schema_version;
    result.provider_id = rust::String(receipt.provider_id);
    result.provider_version = rust::String(receipt.provider_version);
    result.library_version = rust::String(receipt.library_version);
    result.development_settings_signature = rust::String(receipt.development_settings_signature);
    result.processed_linear_reference_contract_version =
        receipt.processed_linear_reference_contract_version;
    result.declared_image_dimensions = dimensions(receipt.declared_image_dimensions);
    result.rendered_dimensions = dimensions(receipt.rendered_dimensions);
    result.orientation = receipt.orientation;
    result.half_size = receipt.half_size;
    result.use_camera_white_balance = receipt.use_camera_white_balance;
    result.use_camera_matrix = receipt.use_camera_matrix;
    result.use_auto_brightness = receipt.use_auto_brightness;
    result.use_exposure_correction = receipt.use_exposure_correction;
    result.brightness = receipt.brightness;
    result.maximum_adjustment_threshold = receipt.maximum_adjustment_threshold;
    result.output_bits_per_channel = receipt.output_bits_per_channel;
    result.demosaic_quality = receipt.demosaic_quality;
    result.output_color = receipt.output_color;
    result.gamma_inverse_power = receipt.gamma_inverse_power;
    result.gamma_linear_toe_slope = receipt.gamma_linear_toe_slope;
    result.dng_opcode_list_1_bytes = receipt.declared_dng_opcode_lists.dng_opcode_list_bytes[0];
    result.dng_opcode_list_2_bytes = receipt.declared_dng_opcode_lists.dng_opcode_list_bytes[1];
    result.dng_opcode_list_3_bytes = receipt.declared_dng_opcode_lists.dng_opcode_list_bytes[2];
    result.process_warnings = receipt.process_warnings;
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

    if (
        source.operation != FfiAdjustmentOperation::SmoothRgbToneCurve
        && source.operation != FfiAdjustmentOperation::PerceptualColor
        && !source.parameter_group_lengths.empty()
    ) {
        throw_invalid_adjustment_plan(
            "only operations with grouped parameter contracts accept group lengths"
        );
    }
    if (source.operation != FfiAdjustmentOperation::Lut3D && !source.payload.empty()) {
        throw_invalid_adjustment_plan(
            "only the 3D LUT operation accepts an immutable binary payload"
        );
    }

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
    case FfiAdjustmentOperation::SmoothRgbToneCurve: {
        if (source.parameter_group_lengths.size() != 4U) {
            throw_invalid_adjustment_plan(
                "smooth RGB tone curve requires four channel point counts"
            );
        }
        std::size_t total_point_count = 0U;
        for (const std::uint32_t count : source.parameter_group_lengths) {
            if (count < 2U || count > image::maximum_tone_curve_points) {
                throw_invalid_adjustment_plan(
                    "each smooth RGB tone curve channel requires 2 through 256 points"
                );
            }
            total_point_count += static_cast<std::size_t>(count);
        }
        if (source.parameters.size() != total_point_count * 2U) {
            throw_invalid_adjustment_plan(
                "smooth RGB tone curve grouped lengths do not match its flattened points"
            );
        }

        image::SmoothRgbToneCurve curve;
        curve.parameter_schema_version = source.parameter_schema_version;
        curve.implementation_version = source.implementation_version;
        std::size_t point_offset = 0U;
        const auto append_channel = [&](
            image::ToneCurveSet& channel,
            const std::uint32_t point_count
        ) {
            channel.points.clear();
            channel.points.reserve(static_cast<std::size_t>(point_count));
            for (std::uint32_t point = 0U; point < point_count; ++point) {
                const std::size_t parameter = (point_offset + point) * 2U;
                channel.points.push_back(image::ToneCurvePoint{
                    source.parameters[parameter],
                    source.parameters[parameter + 1U],
                });
            }
            point_offset += static_cast<std::size_t>(point_count);
        };
        append_channel(curve.master, source.parameter_group_lengths[0]);
        append_channel(curve.red, source.parameter_group_lengths[1]);
        append_channel(curve.green, source.parameter_group_lengths[2]);
        append_channel(curve.blue, source.parameter_group_lengths[3]);
        result.parameters = std::move(curve);
        break;
    }
    case FfiAdjustmentOperation::RgbWhiteBalance:
        require_parameter_count(source, 2U, "RGB white balance");
        result.parameters = image::RgbWhiteBalanceAdjustment{
            .temperature = source.parameters[0],
            .tint = source.parameters[1],
        };
        break;
    case FfiAdjustmentOperation::Saturation:
        require_parameter_count(source, 1U, "saturation");
        result.parameters = image::SaturationAdjustment{source.parameters[0]};
        break;
    case FfiAdjustmentOperation::SelectiveTone:
        if (source.parameter_schema_version
                != image::selective_tone_v3_parameter_schema_version
            || source.implementation_version
                != image::selective_tone_v3_implementation_version) {
            throw_invalid_adjustment_plan(
                "selective tone requires the complete self-guided filter contract"
            );
        }
        require_parameter_count(source, 4U, "selective tone");
        result.parameters = image::SelectiveToneAdjustment{
            .highlights = source.parameters[0],
            .shadows = source.parameters[1],
            .whites = source.parameters[2],
            .blacks = source.parameters[3],
        };
        break;
    case FfiAdjustmentOperation::PerceptualColor: {
        if (source.parameter_schema_version
                != image::perceptual_color_v2_parameter_schema_version
            || source.implementation_version
                != image::perceptual_color_v2_implementation_version
            || source.parameter_group_lengths.size() != 1U) {
            throw_invalid_adjustment_plan(
                "perceptual color requires the current ordered-range contract"
            );
        }
        const std::size_t additional_count = source.parameter_group_lengths[0];
        if (additional_count + 1U > image::maximum_point_color_ranges
            || source.parameters.size() != 32U + additional_count * 7U) {
            throw_invalid_adjustment_plan(
                "perceptual color has an invalid ordered range payload"
            );
        }
        if (source.parameters[25] != 0.0 && source.parameters[25] != 1.0) {
            throw_invalid_adjustment_plan(
                "perceptual color range enabled flag must be zero or one"
            );
        }
        image::PerceptualColorAdjustment parameters;
        parameters.vibrance = source.parameters[0];
        for (std::size_t index = 0; index < image::perceptual_hue_band_count; ++index) {
            parameters.hue[index] = source.parameters[1U + index];
            parameters.saturation[index] = source.parameters[9U + index];
            parameters.lightness[index] = source.parameters[17U + index];
        }
        parameters.color_range = image::PerceptualColorRange{
            .enabled = source.parameters[25] == 1.0,
            .center_degrees = source.parameters[26],
            .width_degrees = source.parameters[27],
            .softness = source.parameters[28],
            .hue_shift_degrees = source.parameters[29],
            .saturation = source.parameters[30],
            .lightness = source.parameters[31],
        };
        parameters.additional_color_ranges.reserve(additional_count);
        for (std::size_t range_index = 0U; range_index < additional_count; ++range_index) {
            const std::size_t offset = 32U + range_index * 7U;
            if (source.parameters[offset] != 0.0 && source.parameters[offset] != 1.0) {
                throw_invalid_adjustment_plan(
                    "perceptual color range enabled flag must be zero or one"
                );
            }
            parameters.additional_color_ranges.push_back(image::PerceptualColorRange{
                .enabled = source.parameters[offset] == 1.0,
                .center_degrees = source.parameters[offset + 1U],
                .width_degrees = source.parameters[offset + 2U],
                .softness = source.parameters[offset + 3U],
                .hue_shift_degrees = source.parameters[offset + 4U],
                .saturation = source.parameters[offset + 5U],
                .lightness = source.parameters[offset + 6U],
            });
        }
        result.parameters = parameters;
        break;
    }
    case FfiAdjustmentOperation::Lut3D: {
        require_parameter_count(source, 1U, "3D LUT");
        image::CubeLutAdjustment parameters{
            .lut = {},
            .intensity = source.parameters[0],
        };
        if (!source.payload.empty()) {
            parameters.lut = image::parse_cube_lut(std::string_view(
                reinterpret_cast<const char*>(source.payload.data()),
                source.payload.size()
            ));
        }
        result.parameters = std::move(parameters);
        break;
    }
    case FfiAdjustmentOperation::Sharpen: {
        if (source.parameter_schema_version
            != image::detail_effects_v3_parameter_schema_version) {
            throw_invalid_adjustment_plan(
                "detail and effects requires the current split-pass contract"
            );
        }
        image::DetailEffectsExecutionPass execution_pass;
        switch (source.implementation_version) {
        case image::technical_detail_v3_implementation_version:
            execution_pass = image::DetailEffectsExecutionPass::technical_detail;
            break;
        case image::color_grading_v3_implementation_version:
            execution_pass = image::DetailEffectsExecutionPass::color_grading;
            break;
        case image::finishing_effects_v3_implementation_version:
            execution_pass = image::DetailEffectsExecutionPass::finishing_effects;
            break;
        default:
            throw_invalid_adjustment_plan(
                "detail and effects execution pass is unsupported"
            );
        }
        require_parameter_count(source, 33U, "detail and effects");
        image::SharpenAdjustment parameters{
            .execution_pass = execution_pass,
            .amount = source.parameters[0],
            .radius = source.parameters[1],
            .threshold = source.parameters[2],
            .masking = source.parameters[3],
        };
        {
            parameters.denoise_luminance = source.parameters[4];
            parameters.denoise_detail = source.parameters[5];
            parameters.denoise_color = source.parameters[6];
            parameters.dehaze = source.parameters[7];
            parameters.defringe_purple_amount = source.parameters[8];
            parameters.defringe_purple_hue_low = source.parameters[9];
            parameters.defringe_purple_hue_high = source.parameters[10];
            parameters.defringe_green_amount = source.parameters[11];
            parameters.defringe_green_hue_low = source.parameters[12];
            parameters.defringe_green_hue_high = source.parameters[13];
            parameters.shadows_hue = source.parameters[14];
            parameters.shadows_saturation = source.parameters[15];
            parameters.shadows_luminance = source.parameters[16];
            parameters.midtones_hue = source.parameters[17];
            parameters.midtones_saturation = source.parameters[18];
            parameters.midtones_luminance = source.parameters[19];
            parameters.highlights_hue = source.parameters[20];
            parameters.highlights_saturation = source.parameters[21];
            parameters.highlights_luminance = source.parameters[22];
            parameters.grading_blending = source.parameters[23];
            parameters.grading_balance = source.parameters[24];
            parameters.grain_amount = source.parameters[25];
            parameters.grain_size = source.parameters[26];
            parameters.grain_roughness = source.parameters[27];
            parameters.vignette_amount = source.parameters[28];
            parameters.vignette_midpoint = source.parameters[29];
            parameters.vignette_roundness = source.parameters[30];
            parameters.vignette_feather = source.parameters[31];
            parameters.vignette_highlights = source.parameters[32];
        }
        result.parameters = parameters;
        break;
    }
    default:
        throw_invalid_adjustment_plan("adjustment node operation is unsupported");
    }

    return result;
}

[[nodiscard]] std::vector<image::AdjustmentNode> adjustment_nodes(
    const rust::Vec<FfiAdjustmentNode>& nodes
) {
    if (nodes.empty() || nodes.size() > maximum_adjustment_nodes) {
        throw_invalid_adjustment_plan(
            "adjustment render plan must contain between 1 and 256 nodes"
        );
    }

    std::vector<image::AdjustmentNode> result;
    result.reserve(nodes.size());
    for (const auto& source : nodes) {
        result.push_back(adjustment_node(source));
    }
    return result;
}

} // namespace

DecodeHandle::DecodeHandle(
    std::unique_ptr<image::DecoderProvider> provider,
    std::unique_ptr<image::DecodeSession> session,
    std::shared_ptr<const image::OpticsProvider> optics_provider
)
    : provider_(std::move(provider)), session_(std::move(session)),
      optics_provider_(std::move(optics_provider)) {}

DecodeHandle::~DecodeHandle() = default;

void DecodeHandle::configure_optics(const FfiOpticsSettings& settings) {
    image::OpticsSettings configured{
        settings.schema_version,
        settings.enabled,
        settings.correct_distortion,
        settings.correct_tca,
        settings.correct_vignetting,
        settings.automatic_scale,
        std::string(settings.camera_profile_maker),
        std::string(settings.camera_profile_model),
        std::string(settings.lens_profile_maker),
        std::string(settings.lens_profile_model),
    };
    // The signature function is the authoritative schema/combination validator
    // shared with cache identity construction.
    (void)image::optics_settings_signature(configured);
    optics_settings_ = configured;
}

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
    snapshot.iso_speed = metadata.iso_speed;
    snapshot.exposure_time_seconds = metadata.exposure_time_seconds;
    snapshot.aperture_f_number = metadata.aperture_f_number;
    snapshot.focal_length_mm = metadata.focal_length_mm;
    snapshot.captured_at_unix_seconds = metadata.captured_at_unix_seconds;
    snapshot.lens_make = rust::String(metadata.lens_make);
    snapshot.lens_model = rust::String(metadata.lens_model);
    snapshot.focal_length_35mm = metadata.focal_length_35mm;
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

FfiRawDevelopmentReceipt DecodeHandle::raw_development_receipt() const {
    return shadow::bridge::raw_development_receipt(raw_development_receipt_);
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
    const auto nodes = adjustment_nodes(request.nodes);
    const auto proxy = image::render_edited_reference_proxy_jpeg(
        *session_,
        nodes,
        image::ProxyRequest{request.max_edge, request.jpeg_quality},
        optics_provider_.get(),
        optics_settings_
    );
    return encoded_proxy(proxy);
}

std::unique_ptr<EditPreviewHandle> DecodeHandle::prepare_edit_preview(
    const std::uint32_t max_edge
) const {
    auto prepared = image::prepare_warm_edit_preview(
        *session_,
        max_edge,
        optics_provider_.get(),
        optics_settings_
    );
    raw_development_receipt_ = prepared.raw_development_receipt();
    return std::make_unique<EditPreviewHandle>(std::move(prepared));
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

FfiOpticsReceipt EditPreviewHandle::optics_receipt() const {
    return shadow::bridge::optics_receipt(session_.optics_receipt());
}

FfiRawDevelopmentReceipt EditPreviewHandle::raw_development_receipt() const {
    return shadow::bridge::raw_development_receipt(session_.raw_development_receipt());
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
    const auto nodes = adjustment_nodes(request.nodes);
    return encoded_proxy(session_.render_jpeg(nodes, request.jpeg_quality));
}

FfiAnalyzedEditPreview EditPreviewHandle::render_adjustment_plan_with_analysis(
    const FfiAdjustmentRenderRequest& request
) const {
    if (request.max_edge != session_.max_edge()) {
        throw image::DecodeError(
            image::DecodeErrorCode::invalid_request,
            0,
            "warm edit preview request does not match the prepared max edge"
        );
    }
    const auto nodes = adjustment_nodes(request.nodes);
    return analyzed_edit_preview(
        session_.render_jpeg_with_analysis(nodes, request.jpeg_quality)
    );
}

std::unique_ptr<FullEditDetailHandle> DecodeHandle::prepare_edit_detail() const {
    auto prepared = image::prepare_full_edit_detail(
        *session_,
        optics_provider_.get(),
        optics_settings_
    );
    raw_development_receipt_ = prepared.raw_development_receipt();
    return std::make_unique<FullEditDetailHandle>(std::move(prepared));
}

FullEditDetailHandle::FullEditDetailHandle(image::FullEditDetailSession session)
    : session_(std::move(session)) {}

FullEditDetailHandle::~FullEditDetailHandle() = default;

FfiDimensions FullEditDetailHandle::dimensions() const noexcept {
    return shadow::bridge::dimensions(session_.dimensions());
}

std::uint64_t FullEditDetailHandle::retained_bytes() const noexcept {
    return session_.retained_bytes();
}

FfiOpticsReceipt FullEditDetailHandle::optics_receipt() const {
    return shadow::bridge::optics_receipt(session_.optics_receipt());
}

FfiRawDevelopmentReceipt FullEditDetailHandle::raw_development_receipt() const {
    return shadow::bridge::raw_development_receipt(session_.raw_development_receipt());
}

FfiRenderedDetailTile FullEditDetailHandle::render_adjustment_plan_tile(
    const FfiAdjustmentDetailTileRequest& request
) const {
    const auto nodes = adjustment_nodes(request.nodes);
    return rendered_detail_tile(
        session_.render_rgb8(nodes, detail_tile_rect(request.rect))
    );
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
    return std::make_unique<DecodeHandle>(
        std::move(provider),
        std::move(session),
        image::make_lensfun_optics_provider()
    );
}

rust::Vec<FfiOpticsProfileCandidate> query_libraw_optics_profiles_utf8(const rust::Str path) {
    const std::string_view utf8_bytes(path.data(), path.size());
    std::u8string utf8_path;
    utf8_path.reserve(utf8_bytes.size());
    for (const char byte : utf8_bytes) {
        utf8_path.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    auto decoder = image::make_libraw_decoder_provider();
    auto session = decoder->open(std::filesystem::path(utf8_path));
    const auto provider = image::make_lensfun_optics_provider();
    const auto candidates = provider->profile_candidates(session->metadata());
    rust::Vec<FfiOpticsProfileCandidate> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        FfiOpticsProfileCandidate ffi;
        ffi.camera_maker = rust::String(candidate.camera_maker);
        ffi.camera_model = rust::String(candidate.camera_model);
        ffi.lens_maker = rust::String(candidate.lens_maker);
        ffi.lens_model = rust::String(candidate.lens_model);
        result.push_back(std::move(ffi));
    }
    return result;
}

rust::String libraw_provider_version() {
    const auto provider = image::make_libraw_decoder_provider();
    return rust::String(provider->info().version);
}

FfiDisplayLuma decode_jpeg_display_luma(
    const rust::Slice<const std::uint8_t> encoded,
    const std::uint32_t max_edge
) {
    const auto decoded = image::decode_jpeg_display_luma(
        std::span<const std::uint8_t>(encoded.data(), encoded.size()),
        max_edge
    );
    if (decoded.row_stride_samples > std::numeric_limits<std::uint32_t>::max()) {
        throw image::DecodeError(
            image::DecodeErrorCode::internal,
            0,
            "JPEG display-luma stride does not fit the bridge contract"
        );
    }

    FfiDisplayLuma result;
    result.width = decoded.dimensions.width;
    result.height = decoded.dimensions.height;
    result.stride = static_cast<std::uint32_t>(decoded.row_stride_samples);
    result.preprocessing_version = rust::String(decoded.preprocessing_version);
    result.samples.reserve(decoded.samples.size());
    for (const float sample : decoded.samples) {
        result.samples.push_back(sample);
    }
    return result;
}

} // namespace shadow::bridge
