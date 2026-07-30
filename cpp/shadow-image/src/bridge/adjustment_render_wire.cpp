#include "adjustment_render_wire.hpp"

#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/cxx_bridge.hpp>
#include <shadow/image/decoder_error.hpp>
#include <shadow/image/lut.hpp>
#include <shadow/image/photo_liquify.hpp>
#include <shadow/image/retouch.hpp>
#include <shadow/image/tone_curve.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::bridge::adjustment_render_wire {

namespace {

inline constexpr std::size_t maximum_adjustment_nodes = 256U;
inline constexpr std::size_t maximum_adjustment_node_id_bytes = 256U;

[[noreturn]] void throw_invalid_adjustment_plan(std::string message) {
    throw image::DecodeError(image::DecodeErrorCode::invalid_request, 0, std::move(message));
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
    if (source.node_id.empty() || source.node_id.size() > maximum_adjustment_node_id_bytes) {
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

    if (source.operation != FfiAdjustmentOperation::PerceptualColor
        && source.operation != FfiAdjustmentOperation::SpotHeal
        && !source.parameter_group_lengths.empty()) {
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
    case FfiAdjustmentOperation::OklabLightnessToneCurve: {
        if (source.parameters.size() % 2U != 0U) {
            throw_invalid_adjustment_plan(
                "Oklab lightness curve parameters must contain flattened x/y pairs"
            );
        }
        const std::size_t point_count = source.parameters.size() / 2U;
        if (point_count < 2U || point_count > image::maximum_tone_curve_points) {
            throw_invalid_adjustment_plan(
                "Oklab lightness curve must contain between 2 and 256 control points"
            );
        }
        image::OklabLightnessToneCurve curve{
            .parameter_schema_version = source.parameter_schema_version,
            .implementation_version = source.implementation_version,
            .lightness = {},
        };
        // ToneCurveSet intentionally defaults to an identity pair for the native
        // authoring API. The FFI wire contract, however, carries the complete
        // point sequence. Clear that default before appending the transmitted
        // points; otherwise every non-identity curve becomes
        // (0,0) -> (1,1) -> authored points, which fails the strictly-increasing
        // x-coordinate validation and leaves the UI showing its stale preview.
        curve.lightness.points.clear();
        curve.lightness.points.reserve(point_count);
        for (std::size_t index = 0U; index < source.parameters.size(); index += 2U) {
            curve.lightness.points.push_back(
                image::ToneCurvePoint{
                    source.parameters[index],
                    source.parameters[index + 1U],
                }
            );
        }
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
        if (source.parameter_schema_version != image::selective_tone_parameter_schema_version
            || source.implementation_version != image::selective_tone_implementation_version) {
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
        if (source.parameter_schema_version != image::perceptual_color_parameter_schema_version
            || source.implementation_version != image::perceptual_color_implementation_version
            || source.parameter_group_lengths.size() != 1U) {
            throw_invalid_adjustment_plan(
                "perceptual color requires the current Color Mixer and Selective Color contract"
            );
        }
        const std::size_t additional_count = source.parameter_group_lengths[0];
        if (additional_count + 1U > image::maximum_point_color_ranges
            || source.parameters.size() != 72U + additional_count * 7U) {
            throw_invalid_adjustment_plan("perceptual color has an invalid ordered range payload");
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
        if (source.parameters[32] != 0.0 && source.parameters[32] != 1.0) {
            throw_invalid_adjustment_plan("Selective Color relative flag must be zero or one");
        }
        parameters.selective_color_relative = source.parameters[32] == 1.0;
        parameters.selective_color_lightness_protection = source.parameters[33];
        for (std::size_t target = 0U; target < image::selective_color_target_count; ++target) {
            for (std::size_t component = 0U; component < image::selective_color_component_count;
                 ++component) {
                parameters.selective_color_cmyk[target][component] =
                    source.parameters
                        [34U + target * image::selective_color_component_count + component];
            }
        }
        parameters.global_a_balance = source.parameters[70];
        parameters.global_b_balance = source.parameters[71];
        parameters.additional_color_ranges.reserve(additional_count);
        for (std::size_t range_index = 0U; range_index < additional_count; ++range_index) {
            const std::size_t offset = 72U + range_index * 7U;
            if (source.parameters[offset] != 0.0 && source.parameters[offset] != 1.0) {
                throw_invalid_adjustment_plan(
                    "perceptual color range enabled flag must be zero or one"
                );
            }
            parameters.additional_color_ranges.push_back(
                image::PerceptualColorRange{
                    .enabled = source.parameters[offset] == 1.0,
                    .center_degrees = source.parameters[offset + 1U],
                    .width_degrees = source.parameters[offset + 2U],
                    .softness = source.parameters[offset + 3U],
                    .hue_shift_degrees = source.parameters[offset + 4U],
                    .saturation = source.parameters[offset + 5U],
                    .lightness = source.parameters[offset + 6U],
                }
            );
        }
        result.parameters = parameters;
        break;
    }
    case FfiAdjustmentOperation::OklabColorWarper: {
        if (source.parameter_schema_version != image::oklab_color_warper_parameter_schema_version
            || source.implementation_version != image::oklab_color_warper_implementation_version) {
            throw_invalid_adjustment_plan(
                "Oklab Color Warper requires the current fixed lattice contract"
            );
        }
        constexpr std::size_t parameter_count =
            1U + image::oklab_color_warper_control_point_count * 2U;
        require_parameter_count(source, parameter_count, "Oklab Color Warper");
        image::OklabColorWarperAdjustment parameters;
        parameters.strength = source.parameters[0];
        for (std::size_t index = 0U; index < image::oklab_color_warper_control_point_count;
             ++index) {
            parameters.control_points[index] = image::OklabColorWarperControlPoint{
                .a_offset = source.parameters[1U + index * 2U],
                .b_offset = source.parameters[2U + index * 2U],
            };
        }
        result.parameters = std::move(parameters);
        break;
    }
    case FfiAdjustmentOperation::Lut3D: {
        require_parameter_count(source, 1U, "3D LUT");
        image::CubeLutAdjustment parameters{
            .lut = {},
            .intensity = source.parameters[0],
        };
        if (!source.payload.empty()) {
            parameters.lut = image::parse_cube_lut(
                std::string_view(
                    reinterpret_cast<const char*>(source.payload.data()),
                    source.payload.size()
                )
            );
        }
        result.parameters = std::move(parameters);
        break;
    }
    case FfiAdjustmentOperation::Sharpen: {
        if (source.parameter_schema_version != image::detail_effects_parameter_schema_version) {
            throw_invalid_adjustment_plan(
                "detail and effects requires the current split-pass contract"
            );
        }
        image::DetailEffectsExecutionPass execution_pass;
        switch (source.detail_effects_pass) {
        case FfiDetailEffectsPass::TechnicalDetail:
            execution_pass = image::DetailEffectsExecutionPass::technical_detail;
            break;
        case FfiDetailEffectsPass::ColorGrading:
            execution_pass = image::DetailEffectsExecutionPass::color_grading;
            break;
        case FfiDetailEffectsPass::FinishingEffects:
            execution_pass = image::DetailEffectsExecutionPass::finishing_effects;
            break;
        }
        require_parameter_count(source, 37U, "detail and effects");
        image::SharpenAdjustment parameters{
            .execution_pass = execution_pass,
            .amount = source.parameters[0],
            .radius = source.parameters[1],
            .threshold = source.parameters[2],
            .masking = source.parameters[3],
        };
        {
            parameters.clarity = source.parameters[4];
            parameters.texture = source.parameters[5];
            parameters.local_contrast = source.parameters[6];
            parameters.local_contrast_scale = source.parameters[7];
            parameters.denoise_luminance = source.parameters[8];
            parameters.denoise_detail = source.parameters[9];
            parameters.denoise_color = source.parameters[10];
            parameters.dehaze = source.parameters[11];
            parameters.defringe_purple_amount = source.parameters[12];
            parameters.defringe_purple_hue_low = source.parameters[13];
            parameters.defringe_purple_hue_high = source.parameters[14];
            parameters.defringe_green_amount = source.parameters[15];
            parameters.defringe_green_hue_low = source.parameters[16];
            parameters.defringe_green_hue_high = source.parameters[17];
            parameters.shadows_hue = source.parameters[18];
            parameters.shadows_saturation = source.parameters[19];
            parameters.shadows_luminance = source.parameters[20];
            parameters.midtones_hue = source.parameters[21];
            parameters.midtones_saturation = source.parameters[22];
            parameters.midtones_luminance = source.parameters[23];
            parameters.highlights_hue = source.parameters[24];
            parameters.highlights_saturation = source.parameters[25];
            parameters.highlights_luminance = source.parameters[26];
            parameters.grading_blending = source.parameters[27];
            parameters.grading_balance = source.parameters[28];
            parameters.grain_amount = source.parameters[29];
            parameters.grain_size = source.parameters[30];
            parameters.grain_roughness = source.parameters[31];
            parameters.vignette_amount = source.parameters[32];
            parameters.vignette_midpoint = source.parameters[33];
            parameters.vignette_roundness = source.parameters[34];
            parameters.vignette_feather = source.parameters[35];
            parameters.vignette_highlights = source.parameters[36];
        }
        result.parameters = parameters;
        break;
    }
    case FfiAdjustmentOperation::SpotHeal: {
        if (source.parameter_group_lengths.size() < 2U) {
            throw_invalid_adjustment_plan(
                "spot-heal requires target and continuous-stroke count parameter groups"
            );
        }
        const std::size_t target_count = source.parameter_group_lengths[0];
        const std::size_t stroke_count = source.parameter_group_lengths[1];
        if ((target_count == 0U && stroke_count == 0U) || target_count > 64U || stroke_count > 64U
            || source.parameter_group_lengths.size() != 2U + stroke_count) {
            throw_invalid_adjustment_plan(
                "spot-heal must contain bounded complete repair targets or continuous strokes"
            );
        }
        std::size_t expected_parameter_count = target_count * 7U;
        for (std::size_t stroke_index = 0U; stroke_index < stroke_count; ++stroke_index) {
            const std::size_t point_count = source.parameter_group_lengths[2U + stroke_index];
            if (point_count == 0U || point_count > 512U) {
                throw_invalid_adjustment_plan(
                    "a continuous repair stroke must contain 1 through 512 points"
                );
            }
            expected_parameter_count += 5U + point_count * 2U;
        }
        if (source.parameters.size() != expected_parameter_count) {
            throw_invalid_adjustment_plan(
                "spot-heal payload does not match its target and stroke groups"
            );
        }
        image::SpotHealAdjustment parameters;
        parameters.spots.reserve(target_count);
        for (std::size_t index = 0U; index < target_count; ++index) {
            const std::size_t offset = index * 7U;
            const double encoded_radius = source.parameters[offset + 2U];
            const double encoded_mode = source.parameters[offset + 3U];
            if (!std::isfinite(source.parameters[offset])
                || !std::isfinite(source.parameters[offset + 1U]) || !std::isfinite(encoded_radius)
                || !std::isfinite(encoded_mode) || !std::isfinite(source.parameters[offset + 4U])
                || !std::isfinite(source.parameters[offset + 5U])
                || !std::isfinite(source.parameters[offset + 6U]) || source.parameters[offset] < 0.0
                || source.parameters[offset] > 1.0 || source.parameters[offset + 1U] < 0.0
                || source.parameters[offset + 1U] > 1.0 || encoded_radius < 1.0
                || encoded_radius > 128.0 || std::floor(encoded_radius) != encoded_radius
                || (encoded_mode != 0.0 && encoded_mode != 1.0)
                || source.parameters[offset + 4U] < -8.0 || source.parameters[offset + 4U] > 8.0
                || source.parameters[offset + 5U] < -8.0 || source.parameters[offset + 5U] > 8.0
                || source.parameters[offset + 6U] < 0.0 || source.parameters[offset + 6U] > 1.0) {
                throw_invalid_adjustment_plan(
                    "spot-heal target behavior is outside the supported range"
                );
            }
            parameters.spots.push_back(
                image::SpotHealTarget{
                    .center_x = source.parameters[offset],
                    .center_y = source.parameters[offset + 1U],
                    .radius_level_zero_pixels = static_cast<std::uint16_t>(encoded_radius),
                    .mode = encoded_mode == 0.0 ? image::SpotRepairMode::heal
                                                : image::SpotRepairMode::clone,
                    .source_offset_x_radii = source.parameters[offset + 4U],
                    .source_offset_y_radii = source.parameters[offset + 5U],
                    .feather = source.parameters[offset + 6U],
                }
            );
        }
        parameters.strokes.reserve(stroke_count);
        std::size_t offset = target_count * 7U;
        for (std::size_t stroke_index = 0U; stroke_index < stroke_count; ++stroke_index) {
            const std::size_t point_count = source.parameter_group_lengths[2U + stroke_index];
            const double encoded_radius = source.parameters[offset];
            const double encoded_mode = source.parameters[offset + 1U];
            if (!std::isfinite(encoded_radius) || !std::isfinite(encoded_mode)
                || !std::isfinite(source.parameters[offset + 2U])
                || !std::isfinite(source.parameters[offset + 3U])
                || !std::isfinite(source.parameters[offset + 4U]) || encoded_radius < 1.0
                || encoded_radius > 128.0 || std::floor(encoded_radius) != encoded_radius
                || (encoded_mode != 0.0 && encoded_mode != 1.0)
                || source.parameters[offset + 2U] < -8.0 || source.parameters[offset + 2U] > 8.0
                || source.parameters[offset + 3U] < -8.0 || source.parameters[offset + 3U] > 8.0
                || source.parameters[offset + 4U] < 0.0 || source.parameters[offset + 4U] > 1.0) {
                throw_invalid_adjustment_plan(
                    "continuous spot-heal behavior is outside the supported range"
                );
            }
            image::RetouchStroke stroke{
                .radius_level_zero_pixels = static_cast<std::uint16_t>(encoded_radius),
                .mode = encoded_mode == 0.0 ? image::SpotRepairMode::heal
                                            : image::SpotRepairMode::clone,
                .source_offset_x_radii = source.parameters[offset + 2U],
                .source_offset_y_radii = source.parameters[offset + 3U],
                .feather = source.parameters[offset + 4U],
            };
            stroke.points.reserve(point_count);
            offset += 5U;
            for (std::size_t point_index = 0U; point_index < point_count; ++point_index) {
                const double x = source.parameters[offset + point_index * 2U];
                const double y = source.parameters[offset + point_index * 2U + 1U];
                if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0
                    || y > 1.0) {
                    throw_invalid_adjustment_plan(
                        "continuous spot-heal points must be normalized to 0..=1"
                    );
                }
                stroke.points.push_back(image::RetouchStrokePoint{.x = x, .y = y});
            }
            offset += point_count * 2U;
            parameters.strokes.push_back(std::move(stroke));
        }
        result.parameters = std::move(parameters);
        break;
    }
    default:
        throw_invalid_adjustment_plan("adjustment node operation is unsupported");
    }

    return result;
}

} // namespace

[[nodiscard]] std::vector<image::AdjustmentNode>
adjustment_nodes(const rust::Vec<FfiAdjustmentNode>& nodes) {
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

[[nodiscard]] std::optional<std::vector<image::AdjustmentLayer>>
adjustment_layers(const rust::Vec<FfiAdjustmentNode>& source) {
    const bool has_boundaries =
        std::any_of(source.begin(), source.end(), [](const FfiAdjustmentNode& node) {
            return node.operation == FfiAdjustmentOperation::LocalMaskLayerStart
                   || node.operation == FfiAdjustmentOperation::LocalMaskLayerEnd;
        });
    if (!has_boundaries) {
        return std::nullopt;
    }
    if (source.empty() || source.size() > maximum_adjustment_nodes) {
        throw_invalid_adjustment_plan(
            "local-mask adjustment stream must contain between 1 and 256 nodes"
        );
    }

    std::vector<image::AdjustmentLayer> layers;
    std::optional<image::AdjustmentLayer> open_layer;
    for (const auto& node : source) {
        if (node.operation == FfiAdjustmentOperation::LocalMaskLayerStart) {
            if (open_layer.has_value()) {
                throw_invalid_adjustment_plan("local-mask layers may not nest");
            }
            if (node.parameter_schema_version != image::adjustment_parameter_schema_version
                || node.implementation_version != image::adjustment_implementation_version
                || !node.payload.empty() || node.parameters.size() < 10U) {
                throw_invalid_adjustment_plan("local-mask layer start has an invalid contract");
            }
            for (const double value : node.parameters) {
                if (!std::isfinite(value)) {
                    throw_invalid_adjustment_plan(
                        "local-mask layer start has a non-finite parameter"
                    );
                }
            }
            const double opacity = node.parameters[0];
            const double kind = node.parameters[1];
            const double invert = node.parameters[9];
            if (opacity < 0.0 || opacity > 1.0
                || (kind != 0.0 && kind != 1.0 && kind != 2.0 && kind != 3.0 && kind != 4.0
                    && kind != 5.0)
                || (invert != 0.0 && invert != 1.0)) {
                throw_invalid_adjustment_plan(
                    "local-mask layer start has an out-of-range parameter"
                );
            }
            const bool brush = kind == 3.0;
            if ((!brush && (!node.parameter_group_lengths.empty() || node.parameters.size() != 10U))
                || (brush
                    && (node.parameter_group_lengths.size() != 1U
                        || node.parameters.size()
                               != 10U
                                      + static_cast<std::size_t>(node.parameter_group_lengths[0])
                                            * 3U
                        || node.parameter_group_lengths[0] > 4096U))) {
                throw_invalid_adjustment_plan("local-mask layer start has invalid brush groups");
            }
            image::AdjustmentLayer layer{
                .layer_id = std::string(node.node_id.data(), node.node_id.size()),
                .enabled = node.enabled,
                .opacity = opacity,
                .mask = std::nullopt,
                .nodes = {},
            };
            if (kind == 1.0) {
                layer.mask = image::LocalMask{
                    .kind = image::LocalMaskKind::linear_gradient,
                    .x0 = node.parameters[2],
                    .y0 = node.parameters[3],
                    .x1 = node.parameters[4],
                    .y1 = node.parameters[5],
                    .invert = invert == 1.0,
                };
            } else if (kind == 2.0) {
                layer.mask = image::LocalMask{
                    .kind = image::LocalMaskKind::radial_gradient,
                    .x0 = node.parameters[2],
                    .y0 = node.parameters[3],
                    .radius_x = node.parameters[6],
                    .radius_y = node.parameters[7],
                    .feather = node.parameters[8],
                    .invert = invert == 1.0,
                };
            } else if (kind == 3.0) {
                std::vector<image::LocalMaskPoint> points;
                points.reserve(node.parameter_group_lengths[0]);
                for (std::size_t offset = 10U; offset < node.parameters.size(); offset += 3U) {
                    const double begins_stroke = node.parameters[offset + 2U];
                    if (begins_stroke != 0.0 && begins_stroke != 1.0) {
                        throw_invalid_adjustment_plan(
                            "local-mask brush point has an invalid stroke marker"
                        );
                    }
                    points.push_back(
                        image::LocalMaskPoint{
                            .x = node.parameters[offset],
                            .y = node.parameters[offset + 1U],
                            .begins_stroke = begins_stroke == 1.0,
                        }
                    );
                }
                layer.mask = image::LocalMask{
                    .kind = image::LocalMaskKind::brush,
                    .radius_x = node.parameters[6],
                    .feather = node.parameters[8],
                    .invert = invert == 1.0,
                    .points = std::move(points),
                };
            } else if (kind == 4.0) {
                layer.mask = image::LocalMask{
                    .kind = image::LocalMaskKind::luminance_range,
                    .x0 = node.parameters[2],
                    .x1 = node.parameters[4],
                    .feather = node.parameters[8],
                    .invert = invert == 1.0,
                };
            } else if (kind == 5.0) {
                layer.mask = image::LocalMask{
                    .kind = image::LocalMaskKind::color_range,
                    .x0 = node.parameters[2],
                    .x1 = node.parameters[4],
                    .feather = node.parameters[8],
                    .invert = invert == 1.0,
                };
            }
            open_layer = std::move(layer);
            continue;
        }
        if (node.operation == FfiAdjustmentOperation::LocalMaskLayerEnd) {
            if (!open_layer.has_value() || !node.parameters.empty() || !node.payload.empty()
                || !node.parameter_group_lengths.empty()) {
                throw_invalid_adjustment_plan("local-mask layer end has no matching valid start");
            }
            if (open_layer->nodes.empty()) {
                throw_invalid_adjustment_plan(
                    "local-mask layer must contain at least one adjustment"
                );
            }
            layers.push_back(std::move(*open_layer));
            open_layer.reset();
            continue;
        }
        if (!open_layer.has_value()) {
            throw_invalid_adjustment_plan(
                "local-mask adjustment appears outside a complete layer boundary"
            );
        }
        open_layer->nodes.push_back(adjustment_node(node));
    }
    // Sixteen user Grade Nodes plus one photo-local repair layer. The latter
    // is compiler-owned and never appears as a second user node limit.
    if (open_layer.has_value() || layers.empty() || layers.size() > 17U) {
        throw_invalid_adjustment_plan("local-mask layer stream is incomplete or exceeds 17 layers");
    }
    return layers;
}

std::optional<image::PhotoLiquify> photo_liquify(const FfiPhotoLiquify& source) {
    if (!source.present) {
        if (!source.points.empty() || !source.stroke_point_counts.empty()
            || !source.stroke_parameters.empty()) {
            throw_invalid_adjustment_plan(
                "absent photo liquify must use the canonical empty payload"
            );
        }
        return std::nullopt;
    }
    if (source.stroke_point_counts.empty()
        || source.stroke_point_counts.size() > image::maximum_photo_liquify_strokes
        || source.stroke_parameters.size() != source.stroke_point_counts.size() * 3U) {
        throw_invalid_adjustment_plan(
            "photo liquify contains invalid stroke partitions or parameters"
        );
    }

    std::size_t expected_point_count = 0U;
    for (const std::uint32_t point_count : source.stroke_point_counts) {
        if (point_count < 2U || point_count > image::maximum_photo_liquify_points_per_stroke) {
            throw_invalid_adjustment_plan("photo liquify push gesture has an invalid point count");
        }
        expected_point_count += point_count;
    }
    if (expected_point_count != source.points.size()) {
        throw_invalid_adjustment_plan(
            "photo liquify point payload does not match its stroke partitions"
        );
    }

    image::PhotoLiquify result;
    result.strokes.reserve(source.stroke_point_counts.size());
    std::size_t point_offset = 0U;
    for (std::size_t stroke_index = 0U; stroke_index < source.stroke_point_counts.size();
         ++stroke_index) {
        image::PhotoLiquifyPushStroke stroke{
            .radius = source.stroke_parameters[stroke_index * 3U],
            .strength = source.stroke_parameters[stroke_index * 3U + 1U],
            .hardness = source.stroke_parameters[stroke_index * 3U + 2U],
        };
        const std::size_t point_count = source.stroke_point_counts[stroke_index];
        stroke.points.reserve(point_count);
        for (std::size_t point_index = 0U; point_index < point_count; ++point_index) {
            const auto& point = source.points[point_offset + point_index];
            stroke.points.push_back(
                image::PhotoLiquifyPoint{
                    .x = point.x,
                    .y = point.y,
                    .pressure = point.pressure,
                }
            );
        }
        point_offset += point_count;
        result.strokes.push_back(std::move(stroke));
    }
    image::validate_photo_liquify(result);
    return result;
}

} // namespace shadow::bridge::adjustment_render_wire
