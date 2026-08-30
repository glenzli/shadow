#include "metal_adjustment_program.hpp"

#include "creative_detail_grading.hpp"
#include "edit_execution_validation.hpp"
#include "oklab_color_warper.hpp"
#include "perceptual_color.hpp"
#include "perceptual_contrast.hpp"
#include "tone_curve_internal.hpp"
#include "working_color_math.hpp"

#include <shadow/image/adjustment_parameters.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace shadow::image {

namespace {

static_assert(sizeof(float) == 4U);
static_assert(std::numeric_limits<float>::is_iec559);

constexpr std::size_t rgb_channels = 3U;

} // namespace

namespace detail {

MetalAdjustmentPreparation prepare_metal_adjustment(
    const FloatRgbImage& input,
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const AdjustmentExecutionContext context,
    const bool input_already_validated
) {
    if (!input_already_validated) {
        validate_edit_image(input);
    }
    static_cast<void>(validate_edit_execution_context(input, context));
    if (plan.source_node_count != nodes.size()) {
        return MetalAdjustmentPreparation{
            .program = std::nullopt,
            .diagnostic = "Metal adjustment plan no longer matches its source nodes",
        };
    }

    std::size_t step_count = 0U;
    std::size_t curve_segment_count = 0U;
    std::size_t lut_entry_count = 0U;
    std::size_t perceptual_mixer_entry_count = 0U;
    std::size_t perceptual_range_entry_count = 0U;
    std::size_t selective_color_entry_count = 0U;
    const auto checked_resource_add = [](std::size_t& total, const std::size_t addition) {
        constexpr std::size_t maximum =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
        if (total > maximum || addition > maximum - total) {
            return false;
        }
        total += addition;
        return true;
    };
    for (const auto& segment : plan.segments) {
        if (segment.locality != AdjustmentLocality::pixel_local) {
            return MetalAdjustmentPreparation{
                .program = std::nullopt,
                .diagnostic = "Metal adjustment cannot prepare a neighborhood segment",
            };
        }
        for (const auto& step : segment.steps) {
            if (step.node_index >= nodes.size()) {
                return MetalAdjustmentPreparation{
                    .program = std::nullopt,
                    .diagnostic = "Metal adjustment plan references a missing source node",
                };
            }
            const AdjustmentNode& node = nodes[step.node_index];
            if (operation(node.parameters) != step.operation) {
                return MetalAdjustmentPreparation{
                    .program = std::nullopt,
                    .diagnostic =
                        "Metal adjustment plan operation no longer matches its source node",
                };
            }
            std::size_t emitted_operation_count = 1U;
            if (step.operation == AdjustmentOperation::oklab_lightness_tone_curve) {
                const auto& parameters = std::get<OklabLightnessToneCurve>(node.parameters);
                if (parameters.lightness.points.size() < 2U
                    || !checked_resource_add(
                        curve_segment_count,
                        parameters.lightness.points.size() - 1U
                    )) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal curve segment table exceeds its uint32 ABI",
                    };
                }
            } else if (step.operation == AdjustmentOperation::oklab_opponent_tone_curves) {
                const auto& parameters = std::get<OklabOpponentToneCurves>(node.parameters);
                if (parameters.a.points.size() < 2U || parameters.b.points.size() < 2U
                    || !checked_resource_add(curve_segment_count, parameters.a.points.size() - 1U)
                    || !checked_resource_add(
                        curve_segment_count,
                        parameters.b.points.size() - 1U
                    )) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal opponent-curve segment table exceeds its uint32 ABI",
                    };
                }
            } else if (step.operation == AdjustmentOperation::lut_3d) {
                const auto& parameters = std::get<CubeLutAdjustment>(node.parameters);
                if (!checked_resource_add(lut_entry_count, parameters.lut.entries.size())) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal LUT entry table exceeds its uint32 ABI",
                    };
                }
            } else if (step.operation == AdjustmentOperation::oklab_color_warper) {
                const auto& parameters = std::get<OklabColorWarperAdjustment>(node.parameters);
                if (!checked_resource_add(
                        perceptual_mixer_entry_count,
                        parameters.control_points.size()
                    )) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal Color Warper control table exceeds its uint32 ABI",
                    };
                }
            } else if (step.operation == AdjustmentOperation::perceptual_color) {
                const auto& parameters = std::get<PerceptualColorAdjustment>(node.parameters);
                const PerceptualColorStages stages = classify_perceptual_color(parameters);
                emitted_operation_count = stages.active_stage_count();
                if (emitted_operation_count == 0U) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic =
                            "Metal perceptual-color plan contains no active sub-operation",
                    };
                }
                if (stages.hue_mapping_active()
                    && (!checked_resource_add(
                            perceptual_mixer_entry_count,
                            perceptual_hue_band_count
                        )
                        || !checked_resource_add(
                            perceptual_range_entry_count,
                            parameters.additional_color_ranges.size()
                        ))) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic =
                            "Metal perceptual-color resource tables exceed their uint32 ABI",
                    };
                }
                if (stages.selective_color_active()
                    && !checked_resource_add(
                        selective_color_entry_count,
                        selective_color_target_count
                    )) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal Selective Color table exceeds its uint32 ABI",
                    };
                }
            }
            if (!checked_resource_add(step_count, emitted_operation_count)) {
                return MetalAdjustmentPreparation{
                    .program = std::nullopt,
                    .diagnostic = "Metal adjustment operation count exceeds its uint32 ABI",
                };
            }
        }
    }
    if (step_count == 0U) {
        return MetalAdjustmentPreparation{
            .program = std::nullopt,
            .diagnostic = "Metal adjustment received no executable operations",
        };
    }
    if (input.dimensions.width > std::numeric_limits<std::uint32_t>::max() / 3U) {
        return MetalAdjustmentPreparation{
            .program = std::nullopt,
            .diagnostic = "Metal adjustment row width exceeds its uint32 ABI",
        };
    }

    PreparedMetalAdjustment prepared;
    prepared.invocation.width = input.dimensions.width;
    prepared.invocation.height = input.dimensions.height;
    prepared.invocation.input_row_floats = input.dimensions.width * 3U;
    prepared.invocation.output_row_floats = input.dimensions.width * 3U;
    prepared.invocation.step_count = static_cast<std::uint32_t>(step_count);
    prepared.invocation.curve_segment_count = static_cast<std::uint32_t>(curve_segment_count);
    prepared.invocation.lut_entry_count = static_cast<std::uint32_t>(lut_entry_count);
    prepared.invocation.perceptual_mixer_entry_count =
        static_cast<std::uint32_t>(perceptual_mixer_entry_count);
    prepared.invocation.perceptual_range_entry_count =
        static_cast<std::uint32_t>(perceptual_range_entry_count);
    prepared.invocation.selective_color_entry_count =
        static_cast<std::uint32_t>(selective_color_entry_count);
    prepared.operations.reserve(step_count);
    prepared.curve_segments.reserve(curve_segment_count);
    prepared.lut_entries.reserve(lut_entry_count);
    prepared.perceptual_mixer_entries.reserve(perceptual_mixer_entry_count);
    prepared.perceptual_range_entries.reserve(perceptual_range_entry_count);
    prepared.selective_color_entries.reserve(selective_color_entry_count);

    const auto checked_parameter_float = [](const double value) -> std::optional<float> {
        if (!std::isfinite(value) || value > static_cast<double>(std::numeric_limits<float>::max())
            || value < -static_cast<double>(std::numeric_limits<float>::max())) {
            return std::nullopt;
        }
        const float converted = static_cast<float>(value);
        return std::isfinite(converted) ? std::optional<float>{converted} : std::nullopt;
    };
    const auto checked_semantic_float =
        [&checked_parameter_float](const double value) -> std::optional<float> {
        const auto converted = checked_parameter_float(value);
        if (!converted.has_value()) {
            return std::nullopt;
        }
        if (value != 0.0
            && (*converted == 0.0F || std::abs(*converted) < std::numeric_limits<float>::min())) {
            return std::nullopt;
        }
        // Perceptual controls are authored as bounded doubles but executed as fp32 on Metal.
        // Reject a future parameter extension that would quantize beyond a small number of
        // float ULPs rather than silently selecting another hue/range or CMYK amount.
        const double tolerance = 16.0 * static_cast<double>(std::numeric_limits<float>::epsilon())
                                 * std::max(1.0, std::abs(value));
        if (std::abs(static_cast<double>(*converted) - value) > tolerance) {
            return std::nullopt;
        }
        return converted;
    };
    const auto checked_normalized_interval = [&checked_parameter_float](
                                                 const double source_minimum,
                                                 const double source_maximum,
                                                 const double maximum_relative_quantization
                                             ) -> std::optional<std::array<float, 2U>> {
        const auto minimum = checked_parameter_float(source_minimum);
        const auto maximum = checked_parameter_float(source_maximum);
        if (!minimum.has_value() || !maximum.has_value() || !(*minimum < *maximum)) {
            return std::nullopt;
        }
        const double source_span = source_maximum - source_minimum;
        const float metal_span = *maximum - *minimum;
        if (!std::isfinite(source_span) || !(source_span > 0.0) || !std::isfinite(metal_span)
            || metal_span < std::numeric_limits<float>::min()) {
            return std::nullopt;
        }
        // Metal has no fp64 arithmetic. Reject intervals whose fp32 endpoints would move the
        // normalized coordinate materially instead of silently sampling a different curve/LUT.
        const double endpoint_error = std::max(
            std::abs(static_cast<double>(*minimum) - source_minimum),
            std::abs(static_cast<double>(*maximum) - source_maximum)
        );
        const double span_error = std::abs(static_cast<double>(metal_span) - source_span);
        if (endpoint_error / source_span > maximum_relative_quantization
            || span_error / source_span > maximum_relative_quantization) {
            return std::nullopt;
        }
        return std::array<float, 2U>{*minimum, *maximum};
    };
    const auto fill_matrix_rows = [&checked_parameter_float](
                                      const Matrix3& matrix,
                                      std::array<float, 4U>& row_0,
                                      std::array<float, 4U>& row_1,
                                      std::array<float, 4U>& row_2
                                  ) {
        std::array<std::array<float, 4U>*, 3U> rows{&row_0, &row_1, &row_2};
        for (std::size_t row = 0U; row < 3U; ++row) {
            for (std::size_t column = 0U; column < 3U; ++column) {
                const auto converted = checked_parameter_float(matrix[row][column]);
                if (!converted.has_value()) {
                    return false;
                }
                (*rows[row])[column] = *converted;
            }
        }
        return true;
    };
    const auto fill_vector = [&checked_parameter_float](
                                 const std::array<double, 4U>& source,
                                 std::array<float, 4U>& destination
                             ) {
        for (std::size_t component = 0U; component < source.size(); ++component) {
            const auto converted = checked_parameter_float(source[component]);
            if (!converted.has_value()) {
                return false;
            }
            destination[component] = *converted;
        }
        return true;
    };
    const auto fill_semantic_vector = [&checked_semantic_float](
                                          const std::array<double, 4U>& source,
                                          std::array<float, 4U>& destination
                                      ) {
        for (std::size_t component = 0U; component < source.size(); ++component) {
            const auto converted = checked_semantic_float(source[component]);
            if (!converted.has_value()) {
                return false;
            }
            destination[component] = *converted;
        }
        return true;
    };
    for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
        const auto coefficient =
            checked_parameter_float(input.working_space.luminance_coefficients[channel]);
        if (!coefficient.has_value()) {
            return MetalAdjustmentPreparation{
                .program = std::nullopt,
                .diagnostic = "Metal working-space luminance coefficients exceed finite fp32 range",
            };
        }
        prepared.invocation.working_luminance[channel] = *coefficient;
    }

    std::optional<WorkingSpaceTransform> working_transform;
    for (const auto& segment : plan.segments) {
        for (const auto& step : segment.steps) {
            if (step.node_index >= nodes.size()
                || step.node_index > std::numeric_limits<std::uint32_t>::max()) {
                return MetalAdjustmentPreparation{
                    .program = std::nullopt,
                    .diagnostic = "Metal adjustment source-node index exceeds its uint32 ABI",
                };
            }
            const AdjustmentNode& node = nodes[step.node_index];
            if (operation(node.parameters) != step.operation) {
                return MetalAdjustmentPreparation{
                    .program = std::nullopt,
                    .diagnostic =
                        "Metal adjustment plan operation no longer matches its source node",
                };
            }

            MetalAdjustmentOp operation_record{
                .source_node_index = static_cast<std::uint32_t>(step.node_index),
            };
            switch (step.operation) {
            case AdjustmentOperation::rgb_white_balance: {
                operation_record.opcode =
                    static_cast<std::uint32_t>(MetalAdjustmentOpcode::rgb_white_balance);
                const auto& parameters = std::get<RgbWhiteBalanceAdjustment>(node.parameters);
                const Matrix3 adaptation = prepare_rgb_white_balance_matrix(
                    input.working_space,
                    parameters,
                    node,
                    step.node_index
                );
                if (!fill_matrix_rows(
                        adaptation,
                        operation_record.parameter_0,
                        operation_record.parameter_1,
                        operation_record.parameter_2
                    )) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal white-balance matrix exceeds finite fp32 range",
                    };
                }
                break;
            }
            case AdjustmentOperation::exposure: {
                operation_record.opcode =
                    static_cast<std::uint32_t>(MetalAdjustmentOpcode::exposure);
                const auto& parameters = std::get<ExposureAdjustment>(node.parameters);
                const auto gain = checked_parameter_float(std::exp2(parameters.stops));
                if (!gain.has_value() || *gain <= 0.0F) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal exposure gain exceeds finite fp32 range",
                    };
                }
                operation_record.parameter_0[0] = *gain;
                break;
            }
            case AdjustmentOperation::contrast: {
                operation_record.opcode =
                    static_cast<std::uint32_t>(MetalAdjustmentOpcode::contrast);
                const auto& parameters = std::get<ContrastAdjustment>(node.parameters);
                const PreparedPerceptualContrast prepared_contrast =
                    prepare_perceptual_contrast(parameters, node, step.node_index);
                const auto pivot_float =
                    checked_parameter_float(prepared_contrast.pivot_lightness());
                const auto amount_float =
                    checked_parameter_float(prepared_contrast.signed_amount());
                if (!pivot_float.has_value() || !amount_float.has_value()) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal contrast parameters exceed finite fp32 range",
                    };
                }
                operation_record.parameter_0 = {
                    *pivot_float,
                    *amount_float,
                    prepared_contrast.collapses_to_pivot() ? 1.0F : 0.0F,
                    0.0F,
                };
                if (!working_transform.has_value()) {
                    working_transform =
                        prepare_working_space_transform(input.working_space, node, step.node_index);
                }
                break;
            }
            case AdjustmentOperation::saturation: {
                operation_record.opcode =
                    static_cast<std::uint32_t>(MetalAdjustmentOpcode::saturation);
                const auto& parameters = std::get<SaturationAdjustment>(node.parameters);
                const auto factor = checked_parameter_float(parameters.factor);
                if (!factor.has_value()) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal saturation factor exceeds finite fp32 range",
                    };
                }
                operation_record.parameter_0[0] = *factor;
                if (!working_transform.has_value()) {
                    working_transform =
                        prepare_working_space_transform(input.working_space, node, step.node_index);
                }
                break;
            }
            case AdjustmentOperation::oklab_lightness_tone_curve: {
                operation_record.opcode =
                    static_cast<std::uint32_t>(MetalAdjustmentOpcode::oklab_lightness_tone_curve);
                const auto& parameters = std::get<OklabLightnessToneCurve>(node.parameters);
                const PreparedSmoothToneCurve curve =
                    prepare_oklab_lightness_tone_curve_node(parameters, node, step.node_index);
                const ToneCurveSet& source = curve.source();
                const std::span<const double> knot_derivatives = curve.knot_derivatives();
                const std::size_t segment_count = curve.segment_count();
                if (curve.is_identity() || segment_count == 0U
                    || prepared.curve_segments.size() > std::numeric_limits<std::uint32_t>::max()
                    || segment_count > std::numeric_limits<std::uint32_t>::max()
                    || prepared.curve_segments.size()
                           > std::numeric_limits<std::uint32_t>::max() - segment_count) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic =
                            "Metal curve resource range is inconsistent with its active plan",
                    };
                }
                operation_record.resource_offset =
                    static_cast<std::uint32_t>(prepared.curve_segments.size());
                operation_record.resource_count = static_cast<std::uint32_t>(segment_count);
                for (std::size_t index = 0U; index < segment_count; ++index) {
                    const ToneCurvePoint left = source.points[index];
                    const ToneCurvePoint right = source.points[index + 1U];
                    const auto metal_interval = checked_normalized_interval(
                        left.x,
                        right.x,
                        128.0 * static_cast<double>(std::numeric_limits<float>::epsilon())
                    );
                    if (!metal_interval.has_value()) {
                        return MetalAdjustmentPreparation{
                            .program = std::nullopt,
                            .diagnostic =
                                "Metal curve knots cannot preserve their interval in fp32",
                        };
                    }
                    MetalCurveSegment segment_record;
                    if (!fill_vector(
                            {
                                static_cast<double>((*metal_interval)[0]),
                                left.y,
                                knot_derivatives[index],
                                0.0,
                            },
                            segment_record.left
                        )
                        || !fill_vector(
                            {
                                static_cast<double>((*metal_interval)[1]),
                                right.y,
                                knot_derivatives[index + 1U],
                                0.0,
                            },
                            segment_record.right
                        )) {
                        return MetalAdjustmentPreparation{
                            .program = std::nullopt,
                            .diagnostic = "Metal curve segment exceeds finite fp32 range",
                        };
                    }
                    prepared.curve_segments.push_back(segment_record);
                }
                operation_record.parameter_0 =
                    prepared.curve_segments[operation_record.resource_offset].left;
                operation_record.parameter_1 =
                    prepared
                        .curve_segments
                            [static_cast<std::size_t>(operation_record.resource_offset)
                             + segment_count - 1U]
                        .right;
                if (!working_transform.has_value()) {
                    working_transform =
                        prepare_working_space_transform(input.working_space, node, step.node_index);
                }
                break;
            }
            case AdjustmentOperation::oklab_opponent_tone_curves: {
                operation_record.opcode =
                    static_cast<std::uint32_t>(MetalAdjustmentOpcode::oklab_opponent_tone_curves);
                const auto& parameters = std::get<OklabOpponentToneCurves>(node.parameters);
                const PreparedOklabOpponentToneCurves curves =
                    prepare_oklab_opponent_tone_curves_node(parameters, node, step.node_index);
                const std::array axes{&curves.a, &curves.b};
                for (std::size_t axis = 0U; axis < axes.size(); ++axis) {
                    const PreparedSmoothToneCurve& prepared_curve = *axes[axis];
                    const ToneCurveSet& source = prepared_curve.source();
                    const std::span<const double> knot_derivatives =
                        prepared_curve.knot_derivatives();
                    const std::size_t segment_count = prepared_curve.segment_count();
                    if (segment_count == 0U
                        || prepared.curve_segments.size()
                               > std::numeric_limits<std::uint32_t>::max()
                        || segment_count > std::numeric_limits<std::uint32_t>::max()
                        || prepared.curve_segments.size()
                               > std::numeric_limits<std::uint32_t>::max() - segment_count) {
                        return MetalAdjustmentPreparation{
                            .program = std::nullopt,
                            .diagnostic = "Metal opponent-curve resource range is inconsistent "
                                          "with its active plan",
                        };
                    }
                    const std::uint32_t resource_offset =
                        static_cast<std::uint32_t>(prepared.curve_segments.size());
                    const std::uint32_t resource_count = static_cast<std::uint32_t>(segment_count);
                    if (axis == 0U) {
                        operation_record.resource_offset = resource_offset;
                        operation_record.resource_count = resource_count;
                    } else {
                        operation_record.secondary_resource_offset = resource_offset;
                        operation_record.secondary_resource_count = resource_count;
                    }
                    for (std::size_t index = 0U; index < segment_count; ++index) {
                        const ToneCurvePoint left = source.points[index];
                        const ToneCurvePoint right = source.points[index + 1U];
                        const auto metal_interval = checked_normalized_interval(
                            left.x,
                            right.x,
                            128.0 * static_cast<double>(std::numeric_limits<float>::epsilon())
                        );
                        if (!metal_interval.has_value()) {
                            return MetalAdjustmentPreparation{
                                .program = std::nullopt,
                                .diagnostic = "Metal opponent-curve knots cannot preserve their "
                                              "interval in fp32",
                            };
                        }
                        MetalCurveSegment segment_record;
                        if (!fill_vector(
                                {
                                    static_cast<double>((*metal_interval)[0]),
                                    left.y,
                                    knot_derivatives[index],
                                    0.0,
                                },
                                segment_record.left
                            )
                            || !fill_vector(
                                {
                                    static_cast<double>((*metal_interval)[1]),
                                    right.y,
                                    knot_derivatives[index + 1U],
                                    0.0,
                                },
                                segment_record.right
                            )) {
                            return MetalAdjustmentPreparation{
                                .program = std::nullopt,
                                .diagnostic =
                                    "Metal opponent-curve segment exceeds finite fp32 range",
                            };
                        }
                        prepared.curve_segments.push_back(segment_record);
                    }
                }
                if (!working_transform.has_value()) {
                    working_transform =
                        prepare_working_space_transform(input.working_space, node, step.node_index);
                }
                break;
            }
            case AdjustmentOperation::oklab_color_warper: {
                operation_record.opcode =
                    static_cast<std::uint32_t>(MetalAdjustmentOpcode::oklab_color_warper);
                const auto& parameters = std::get<OklabColorWarperAdjustment>(node.parameters);
                if (parameters.control_points.size() != oklab_color_warper_control_point_count
                    || prepared.perceptual_mixer_entries.size()
                           > std::numeric_limits<std::uint32_t>::max()
                    || parameters.control_points.size()
                           > std::numeric_limits<std::uint32_t>::max()
                                 - prepared.perceptual_mixer_entries.size()) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal Color Warper resource range is inconsistent with its "
                                      "active plan",
                    };
                }
                const auto strength = checked_semantic_float(parameters.strength);
                const auto half_extent = checked_semantic_float(oklab_color_warper_half_extent);
                // Keep the feather an authored part of the transient program. It is not a
                // Recipe control, but putting it beside the strength makes the CPU/Metal
                // boundary fade explicit and keeps the shader independent of a magic value.
                const auto feather = checked_semantic_float(oklab_color_warper_edge_feather);
                if (!strength.has_value() || !half_extent.has_value() || !feather.has_value()) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic =
                            "Metal Color Warper parameters cannot preserve their fp32 semantics",
                    };
                }
                operation_record.resource_offset =
                    static_cast<std::uint32_t>(prepared.perceptual_mixer_entries.size());
                operation_record.resource_count =
                    static_cast<std::uint32_t>(parameters.control_points.size());
                operation_record.parameter_0 = {
                    *strength,
                    *half_extent,
                    *feather,
                    0.0F,
                };
                for (const auto& point : parameters.control_points) {
                    MetalPerceptualMixerEntry entry;
                    if (!fill_semantic_vector(
                            {point.a_offset, point.b_offset, 0.0, 0.0},
                            entry.value
                        )) {
                        return MetalAdjustmentPreparation{
                            .program = std::nullopt,
                            .diagnostic =
                                "Metal Color Warper controls cannot preserve their fp32 semantics",
                        };
                    }
                    prepared.perceptual_mixer_entries.push_back(entry);
                }
                if (!working_transform.has_value()) {
                    working_transform =
                        prepare_working_space_transform(input.working_space, node, step.node_index);
                }
                break;
            }
            case AdjustmentOperation::lut_3d: {
                operation_record.opcode = static_cast<std::uint32_t>(MetalAdjustmentOpcode::lut_3d);
                const auto& parameters = std::get<CubeLutAdjustment>(node.parameters);
                const std::size_t lut_size = parameters.lut.size;
                if (lut_size < 2U || lut_size > 65U
                    || lut_size > std::numeric_limits<std::size_t>::max() / lut_size
                    || lut_size * lut_size > std::numeric_limits<std::size_t>::max() / lut_size) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal LUT dimensions overflowed",
                    };
                }
                const std::size_t expected_entries = lut_size * lut_size * lut_size;
                if (parameters.lut.entries.size() != expected_entries
                    || prepared.lut_entries.size() > std::numeric_limits<std::uint32_t>::max()
                    || lut_size > std::numeric_limits<std::uint32_t>::max()
                    || expected_entries > std::numeric_limits<std::uint32_t>::max()
                                              - prepared.lut_entries.size()) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic =
                            "Metal LUT resource range is inconsistent with its active plan",
                    };
                }
                operation_record.resource_offset =
                    static_cast<std::uint32_t>(prepared.lut_entries.size());
                // For LUT operations resource_count is the cube edge length. The total entry
                // count lives in the invocation and bounds the red-fastest N^3 address range.
                operation_record.resource_count = static_cast<std::uint32_t>(lut_size);
                const auto intensity = checked_parameter_float(parameters.intensity);
                std::array<std::array<float, 2U>, rgb_channels> metal_domains{};
                bool domains_are_representable = intensity.has_value();
                for (std::size_t channel = 0U; channel < rgb_channels && domains_are_representable;
                     ++channel) {
                    const auto interval = checked_normalized_interval(
                        parameters.lut.domain_min[channel],
                        parameters.lut.domain_max[channel],
                        16.0 * static_cast<double>(std::numeric_limits<float>::epsilon())
                    );
                    if (!interval.has_value()) {
                        domains_are_representable = false;
                    } else {
                        metal_domains[channel] = *interval;
                    }
                }
                if (!domains_are_representable) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic =
                            "Metal LUT domain cannot preserve normalized coordinates in fp32",
                    };
                }
                operation_record.parameter_0 = {
                    *intensity,
                    metal_domains[0][0],
                    metal_domains[1][0],
                    metal_domains[2][0],
                };
                operation_record.parameter_1 = {
                    metal_domains[0][1],
                    metal_domains[1][1],
                    metal_domains[2][1],
                    0.0F,
                };
                for (const auto& entry : parameters.lut.entries) {
                    MetalLutEntry entry_record;
                    for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                        const auto converted = checked_parameter_float(entry[channel]);
                        if (!converted.has_value()) {
                            return MetalAdjustmentPreparation{
                                .program = std::nullopt,
                                .diagnostic = "Metal LUT entry exceeds finite fp32 range",
                            };
                        }
                        entry_record.value[channel] = *converted;
                    }
                    prepared.lut_entries.push_back(entry_record);
                }
                break;
            }
            case AdjustmentOperation::sharpen: {
                const auto& parameters = std::get<SharpenAdjustment>(node.parameters);
                if (parameters.execution_pass != DetailEffectsExecutionPass::color_grading
                    || parameters.clarity != 0.0 || parameters.texture != 0.0
                    || parameters.local_contrast != 0.0) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic =
                            "Metal color grading requires the pixel-local grading-only pass",
                    };
                }
                operation_record.opcode =
                    static_cast<std::uint32_t>(MetalAdjustmentOpcode::color_grading);
                const PreparedColorGrading grading = prepare_color_grading(parameters);
                if (!fill_vector(
                        {
                            grading.shadows().delta_a(),
                            grading.shadows().delta_b(),
                            grading.shadows().delta_lightness(),
                            grading.center(),
                        },
                        operation_record.parameter_0
                    )
                    || !fill_vector(
                        {
                            grading.midtones().delta_a(),
                            grading.midtones().delta_b(),
                            grading.midtones().delta_lightness(),
                            grading.width(),
                        },
                        operation_record.parameter_1
                    )
                    || !fill_vector(
                        {
                            grading.highlights().delta_a(),
                            grading.highlights().delta_b(),
                            grading.highlights().delta_lightness(),
                            0.0,
                        },
                        operation_record.parameter_2
                    )) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic = "Metal color-grading parameters exceed finite fp32 range",
                    };
                }
                if (!working_transform.has_value()) {
                    working_transform =
                        prepare_working_space_transform(input.working_space, node, step.node_index);
                }
                break;
            }
            case AdjustmentOperation::perceptual_color: {
                const auto& parameters = std::get<PerceptualColorAdjustment>(node.parameters);
                const PerceptualColorStages stages = classify_perceptual_color(parameters);
                if (stages.neutral()) {
                    return MetalAdjustmentPreparation{
                        .program = std::nullopt,
                        .diagnostic =
                            "Metal perceptual-color plan contains no active sub-operation",
                    };
                }

                if (stages.hue_mapping_active()) {
                    MetalAdjustmentOp mapping_record{
                        .opcode =
                            static_cast<std::uint32_t>(MetalAdjustmentOpcode::perceptual_mapping),
                        .source_node_index = static_cast<std::uint32_t>(step.node_index),
                    };
                    if (prepared.perceptual_mixer_entries.size()
                            > std::numeric_limits<std::uint32_t>::max()
                        || perceptual_hue_band_count
                               > std::numeric_limits<std::uint32_t>::max()
                                     - prepared.perceptual_mixer_entries.size()
                        || prepared.perceptual_range_entries.size()
                               > std::numeric_limits<std::uint32_t>::max()
                        || parameters.additional_color_ranges.size()
                               > std::numeric_limits<std::uint32_t>::max()
                                     - prepared.perceptual_range_entries.size()) {
                        return MetalAdjustmentPreparation{
                            .program = std::nullopt,
                            .diagnostic = "Metal perceptual-color resource range is inconsistent "
                                          "with its active plan",
                        };
                    }
                    mapping_record.resource_offset =
                        static_cast<std::uint32_t>(prepared.perceptual_mixer_entries.size());
                    mapping_record.resource_count =
                        static_cast<std::uint32_t>(perceptual_hue_band_count);
                    mapping_record.secondary_resource_offset =
                        static_cast<std::uint32_t>(prepared.perceptual_range_entries.size());
                    mapping_record.secondary_resource_count =
                        static_cast<std::uint32_t>(parameters.additional_color_ranges.size());
                    if (!fill_semantic_vector(
                            {
                                parameters.vibrance,
                                parameters.color_range.enabled ? 1.0 : 0.0,
                                parameters.color_range.center_degrees,
                                parameters.color_range.width_degrees,
                            },
                            mapping_record.parameter_0
                        )
                        || !fill_semantic_vector(
                            {
                                parameters.color_range.softness,
                                parameters.color_range.hue_shift_degrees,
                                parameters.color_range.saturation,
                                parameters.color_range.lightness,
                            },
                            mapping_record.parameter_1
                        )) {
                        return MetalAdjustmentPreparation{
                            .program = std::nullopt,
                            .diagnostic = "Metal primary Point Color parameters cannot preserve "
                                          "their fp32 semantics",
                        };
                    }
                    const auto hue_anchors = perceptual_color_hue_anchors();
                    for (std::size_t band = 0U; band < perceptual_hue_band_count; ++band) {
                        MetalPerceptualMixerEntry entry;
                        if (!fill_semantic_vector(
                                {
                                    hue_anchors[band],
                                    30.0 * parameters.hue[band],
                                    parameters.saturation[band],
                                    parameters.lightness[band],
                                },
                                entry.value
                            )) {
                            return MetalAdjustmentPreparation{
                                .program = std::nullopt,
                                .diagnostic = "Metal perceptual mixer entries cannot preserve "
                                              "their fp32 semantics",
                            };
                        }
                        prepared.perceptual_mixer_entries.push_back(entry);
                    }
                    for (const auto& range : parameters.additional_color_ranges) {
                        MetalPerceptualRange entry;
                        if (!fill_semantic_vector(
                                {
                                    range.enabled ? 1.0 : 0.0,
                                    range.center_degrees,
                                    range.width_degrees,
                                    range.softness,
                                },
                                entry.selection
                            )
                            || !fill_semantic_vector(
                                {
                                    range.hue_shift_degrees,
                                    range.saturation,
                                    range.lightness,
                                    0.0,
                                },
                                entry.adjustment
                            )) {
                            return MetalAdjustmentPreparation{
                                .program = std::nullopt,
                                .diagnostic = "Metal ordered Point Color range cannot preserve "
                                              "its fp32 semantics",
                            };
                        }
                        prepared.perceptual_range_entries.push_back(entry);
                    }

                    // CPU applies the hue/range mapping, global opponent balance, and
                    // Selective Color stages in this exact order inside one source node.
                    // The transient operations preserve that sequence without changing the
                    // durable execution plan or Recipe schema.
                    prepared.operations.push_back(mapping_record);
                }

                if (stages.opponent_balance_active()) {
                    MetalAdjustmentOp balance_record{
                        .opcode = static_cast<std::uint32_t>(
                            MetalAdjustmentOpcode::oklab_opponent_balance
                        ),
                        .source_node_index = static_cast<std::uint32_t>(step.node_index),
                    };
                    if (!fill_semantic_vector(
                            {
                                parameters.global_a_balance,
                                parameters.global_b_balance,
                                0.0,
                                0.0,
                            },
                            balance_record.parameter_0
                        )) {
                        return MetalAdjustmentPreparation{
                            .program = std::nullopt,
                            .diagnostic =
                                "Metal global Oklab balance cannot preserve its fp32 semantics",
                        };
                    }
                    prepared.operations.push_back(balance_record);
                }

                if (stages.selective_color_active()) {
                    operation_record = MetalAdjustmentOp{
                        .opcode =
                            static_cast<std::uint32_t>(MetalAdjustmentOpcode::selective_color),
                        .source_node_index = static_cast<std::uint32_t>(step.node_index),
                    };
                    if (prepared.selective_color_entries.size()
                            > std::numeric_limits<std::uint32_t>::max()
                        || selective_color_target_count
                               > std::numeric_limits<std::uint32_t>::max()
                                     - prepared.selective_color_entries.size()) {
                        return MetalAdjustmentPreparation{
                            .program = std::nullopt,
                            .diagnostic = "Metal Selective Color resource range is inconsistent "
                                          "with its active plan",
                        };
                    }
                    operation_record.resource_offset =
                        static_cast<std::uint32_t>(prepared.selective_color_entries.size());
                    operation_record.resource_count =
                        static_cast<std::uint32_t>(selective_color_target_count);
                    if (!fill_semantic_vector(
                            {
                                parameters.selective_color_relative ? 1.0 : 0.0,
                                parameters.selective_color_lightness_protection,
                                0.0,
                                0.0,
                            },
                            operation_record.parameter_0
                        )) {
                        return MetalAdjustmentPreparation{
                            .program = std::nullopt,
                            .diagnostic =
                                "Metal Selective Color mode cannot preserve its fp32 semantics",
                        };
                    }
                    for (const auto& target : parameters.selective_color_cmyk) {
                        MetalSelectiveColorEntry entry;
                        if (!fill_semantic_vector(target, entry.cmyk)) {
                            return MetalAdjustmentPreparation{
                                .program = std::nullopt,
                                .diagnostic = "Metal Selective Color table cannot preserve "
                                              "its fp32 semantics",
                            };
                        }
                        prepared.selective_color_entries.push_back(entry);
                    }
                    prepared.operations.push_back(operation_record);
                }
                if (!working_transform.has_value()) {
                    working_transform =
                        prepare_working_space_transform(input.working_space, node, step.node_index);
                }
                // PerceptualColor can lower to multiple adjacent Metal operations; they have
                // all been appended above, so skip the one-record epilogue used by simpler
                // adjustment kinds.
                continue;
            }
            case AdjustmentOperation::selective_tone:
            case AdjustmentOperation::spot_heal:
            case AdjustmentOperation::image_completion:
                return MetalAdjustmentPreparation{
                    .program = std::nullopt,
                    .diagnostic = "Metal adjustment received an unsupported operation",
                };
            }
            prepared.operations.push_back(operation_record);
        }
    }

    if (working_transform.has_value()
        && (!fill_matrix_rows(
                working_transform->rgb_to_xyz,
                prepared.invocation.rgb_to_xyz_row_0,
                prepared.invocation.rgb_to_xyz_row_1,
                prepared.invocation.rgb_to_xyz_row_2
            )
            || !fill_matrix_rows(
                working_transform->xyz_to_rgb,
                prepared.invocation.xyz_to_rgb_row_0,
                prepared.invocation.xyz_to_rgb_row_1,
                prepared.invocation.xyz_to_rgb_row_2
            ))) {
        return MetalAdjustmentPreparation{
            .program = std::nullopt,
            .diagnostic = "Metal working-space transform exceeds finite fp32 range",
        };
    }
    if (prepared.operations.size() != step_count
        || prepared.curve_segments.size() != curve_segment_count
        || prepared.lut_entries.size() != lut_entry_count
        || prepared.perceptual_mixer_entries.size() != perceptual_mixer_entry_count
        || prepared.perceptual_range_entries.size() != perceptual_range_entry_count
        || prepared.selective_color_entries.size() != selective_color_entry_count) {
        return MetalAdjustmentPreparation{
            .program = std::nullopt,
            .diagnostic = "Metal adjustment resource compilation produced inconsistent counts "
                          "(operations "
                          + std::to_string(prepared.operations.size()) + "/"
                          + std::to_string(step_count) + ", curves "
                          + std::to_string(prepared.curve_segments.size()) + "/"
                          + std::to_string(curve_segment_count) + ", LUT "
                          + std::to_string(prepared.lut_entries.size()) + "/"
                          + std::to_string(lut_entry_count) + ", mixer "
                          + std::to_string(prepared.perceptual_mixer_entries.size()) + "/"
                          + std::to_string(perceptual_mixer_entry_count) + ", ranges "
                          + std::to_string(prepared.perceptual_range_entries.size()) + "/"
                          + std::to_string(perceptual_range_entry_count) + ", selective "
                          + std::to_string(prepared.selective_color_entries.size()) + "/"
                          + std::to_string(selective_color_entry_count) + ")",
        };
    }
    return MetalAdjustmentPreparation{
        .program = std::move(prepared),
        .diagnostic = {},
    };
}

} // namespace detail

} // namespace shadow::image
