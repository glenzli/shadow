#include <shadow/image/adjustment_execution.hpp>
#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_error.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include "../acceleration/image_acceleration_policy.hpp"
#include "metal_adjustment_execution.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace shadow::image {

namespace {

[[nodiscard]] std::optional<std::string>
metal_ineligibility(const EditExecutionPlan& plan, const std::span<const AdjustmentNode> nodes) {
    for (const auto& segment : plan.segments) {
        if (segment.locality != AdjustmentLocality::pixel_local) {
            return "Metal adjustment requires every active node to be pixel-local";
        }
        for (const auto& step : segment.steps) {
            if (step.node_index >= nodes.size()
                || operation(nodes[step.node_index].parameters) != step.operation) {
                return "Metal adjustment plan no longer matches its source nodes";
            }
            switch (step.operation) {
            case AdjustmentOperation::rgb_white_balance:
            case AdjustmentOperation::exposure:
            case AdjustmentOperation::contrast:
            case AdjustmentOperation::saturation:
            case AdjustmentOperation::oklab_lightness_tone_curve:
            case AdjustmentOperation::oklab_opponent_tone_curves:
            case AdjustmentOperation::oklab_color_warper:
            case AdjustmentOperation::lut_3d:
                break;
            case AdjustmentOperation::perceptual_color: {
                const auto* parameters =
                    std::get_if<PerceptualColorAdjustment>(&nodes[step.node_index].parameters);
                if (parameters == nullptr) {
                    return "Metal adjustment plan has an invalid perceptual color node";
                }
                break;
            }
            case AdjustmentOperation::sharpen: {
                const auto* parameters =
                    std::get_if<SharpenAdjustment>(&nodes[step.node_index].parameters);
                if (parameters == nullptr
                    || parameters->execution_pass != DetailEffectsExecutionPass::color_grading
                    || parameters->clarity != 0.0 || parameters->texture != 0.0
                    || parameters->local_contrast != 0.0) {
                    return "Metal adjustment supports only pixel-local color grading "
                           "from the Detail & Effects node";
                }
                break;
            }
            case AdjustmentOperation::selective_tone:
            case AdjustmentOperation::spot_heal:
                return "Metal adjustment does not support active operation "
                       + std::string(operation_id(step.operation));
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] AdjustmentExecutionResult execute_on_cpu(
    const FloatRgbImage& input,
    const std::span<const AdjustmentNode> nodes,
    const AdjustmentExecutionContext context,
    const bool fell_back,
    std::string diagnostic
) {
    auto pixels = execute_adjustment_nodes(input, nodes, context);
    AdjustmentExecutionResult result{
        .pixels = std::move(pixels),
        .backend = AdjustmentBackend::cpu,
        .fell_back = fell_back,
        .diagnostic = std::move(diagnostic),
    };
    if (!result.valid()) {
        throw EditError(
            EditErrorCode::numeric_overflow,
            std::nullopt,
            "CPU adjustment backend produced an invalid result"
        );
    }
    return result;
}

[[noreturn]] void throw_forced_metal_failure(std::string diagnostic) {
    if (diagnostic.empty()) {
        diagnostic = "Metal adjustment declined the complete adjustment stage";
    }
    throw EditError(EditErrorCode::backend_failure, std::nullopt, std::move(diagnostic));
}

} // namespace

std::string_view adjustment_backend_identity(const AdjustmentBackend backend) noexcept {
    switch (backend) {
    case AdjustmentBackend::cpu:
        return "shadow-adjustment-cpu-v1;math=f64";
    case AdjustmentBackend::metal:
        return "shadow-adjustment-metal-v1;abi=1;math=f32-safe;"
               "ops=wb,exposure,contrast,saturation,perceptual,opponent-balance,selective-color,"
               "curve,opponent-curves,grading,lut";
    }
    return "shadow-adjustment-unknown";
}

bool adjustment_backend_available(const AdjustmentBackend backend) noexcept {
    switch (backend) {
    case AdjustmentBackend::cpu:
        return true;
    case AdjustmentBackend::metal:
        return detail::metal_adjustment_available();
    }
    return false;
}

AdjustmentBackendMode adjustment_backend_mode_from_environment() {
    const auto preference = detail::image_acceleration_preference_from_environment();
    if (!preference.has_value()) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "SHADOW_IMAGE_ACCELERATION must be auto, cpu, or metal"
        );
    }
    if (*preference == detail::ImageAccelerationPreference::automatic) {
        return AdjustmentBackendMode::automatic;
    }
    if (*preference == detail::ImageAccelerationPreference::cpu) {
        return AdjustmentBackendMode::cpu;
    }
    return AdjustmentBackendMode::metal;
}

bool AdjustmentExecutionResult::valid() const noexcept {
    const bool known_backend =
        backend == AdjustmentBackend::cpu || backend == AdjustmentBackend::metal;
    if (!known_backend || pixels.dimensions.width == 0U || pixels.dimensions.height == 0U
        || pixels.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || pixels.row_stride_bytes
               < static_cast<std::size_t>(pixels.dimensions.width) * 3U * sizeof(float)
        || pixels.row_stride_bytes % sizeof(float) != 0U
        || (fell_back && (backend != AdjustmentBackend::cpu || diagnostic.empty()))
        || (!fell_back && !diagnostic.empty())) {
        return false;
    }
    const std::size_t row_floats = pixels.row_stride_bytes / sizeof(float);
    if (row_floats > std::numeric_limits<std::size_t>::max()
                         / static_cast<std::size_t>(pixels.dimensions.height)) {
        return false;
    }
    // Finiteness is an execution contract, not a post-hoc extra raster pass: the CPU oracle
    // validates its immutable input and checked-converts every node result, while Metal validates
    // the same input during shared host preparation and raises an atomic failure after every GPU
    // operation. Re-scanning here would add a full memory-bandwidth pass to every slider update.
    return pixels.samples.size() == row_floats * static_cast<std::size_t>(pixels.dimensions.height);
}

AdjustmentExecutionResult execute_adjustment_nodes_with_backend(
    const FloatRgbImage& input,
    const std::span<const AdjustmentNode> nodes,
    const AdjustmentExecutionContext context,
    const AdjustmentBackendMode backend_mode
) {
    if (backend_mode != AdjustmentBackendMode::automatic
        && backend_mode != AdjustmentBackendMode::cpu
        && backend_mode != AdjustmentBackendMode::metal) {
        throw EditError(
            EditErrorCode::invalid_parameter,
            std::nullopt,
            "adjustment dispatcher received an unknown backend mode"
        );
    }

    // Compile and validate every source node before inspecting backend eligibility. Disabled
    // malformed nodes therefore fail identically on CPU, Metal and automatic selection.
    const EditExecutionPlan plan = compile_edit_execution_plan(
        nodes,
        input.level_zero_to_raster_scale_x,
        input.level_zero_to_raster_scale_y
    );
    if (plan.segments.empty() || backend_mode == AdjustmentBackendMode::cpu) {
        return execute_on_cpu(input, nodes, context, false, {});
    }

    if (const auto ineligible = metal_ineligibility(plan, nodes); ineligible.has_value()) {
        if (backend_mode == AdjustmentBackendMode::metal) {
            throw_forced_metal_failure(*ineligible);
        }
        return execute_on_cpu(input, nodes, context, true, *ineligible);
    }

    auto preparation = detail::prepare_metal_adjustment(input, nodes, plan, context);
    if (!preparation.program.has_value()) {
        if (backend_mode == AdjustmentBackendMode::metal) {
            throw_forced_metal_failure(std::move(preparation.diagnostic));
        }
        return execute_on_cpu(
            input,
            nodes,
            context,
            true,
            preparation.diagnostic.empty() ? "Metal adjustment could not prepare the complete stage"
                                           : std::move(preparation.diagnostic)
        );
    }

    auto attempt = detail::try_execute_adjustments_metal(input, *preparation.program);
    if (attempt.output.has_value()) {
        AdjustmentExecutionResult result{
            .pixels = std::move(*attempt.output),
            .backend = AdjustmentBackend::metal,
        };
        if (!result.valid()) {
            if (backend_mode == AdjustmentBackendMode::metal) {
                throw_forced_metal_failure(
                    "Metal adjustment returned an invalid complete-stage result"
                );
            }
            return execute_on_cpu(
                input,
                nodes,
                context,
                true,
                "Metal adjustment returned an invalid result; the complete stage was replayed"
            );
        }
        return result;
    }

    if (backend_mode == AdjustmentBackendMode::metal) {
        throw_forced_metal_failure(std::move(attempt.diagnostic));
    }
    return execute_on_cpu(
        input,
        nodes,
        context,
        true,
        attempt.diagnostic.empty()
            ? "Metal adjustment declined the request; the complete stage was replayed"
            : std::move(attempt.diagnostic)
    );
}

AdjustmentExecutionResult execute_adjustment_nodes_accelerated(
    const FloatRgbImage& input,
    const std::span<const AdjustmentNode> nodes,
    const AdjustmentExecutionContext context
) {
    return execute_adjustment_nodes_with_backend(
        input,
        nodes,
        context,
        adjustment_backend_mode_from_environment()
    );
}

} // namespace shadow::image
