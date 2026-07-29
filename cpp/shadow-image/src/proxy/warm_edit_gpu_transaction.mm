#include "warm_edit_gpu_transaction.hpp"

#include "warm_edit_gpu_stage_encoder.hpp"

#include <shadow/image/edit_execution_plan.hpp>

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>
#include <variant>

namespace shadow::image::detail {

namespace {

struct WarmStagePostView final {
    std::span<const AdjustmentNode> nodes;
    const EditExecutionPlan* plan = nullptr;
};

[[nodiscard]] std::optional<WarmStagePostView> stage_post_view(
    const WarmGpuNeighbourhoodStage& stage
) {
    return std::visit(
        [](const auto& value) -> std::optional<WarmStagePostView> {
            using Stage = std::decay_t<decltype(value)>;
            if constexpr (requires(Stage candidate) { candidate.post_nodes; }) {
                if (value.post_nodes.empty()) {
                    return std::nullopt;
                }
                return WarmStagePostView{
                    .nodes = value.post_nodes,
                    .plan = &value.after,
                };
            } else {
                return std::nullopt;
            }
        },
        stage
    );
}

[[nodiscard]] FloatRgbImage source_layout(const WarmGpuResidentLayout& layout) {
    return FloatRgbImage{
        .dimensions = layout.dimensions,
        .row_stride_bytes = layout.source_row_stride_bytes,
        .pixel_format = layout.pixel_format,
        .transfer_function = layout.transfer_function,
        .reference = layout.reference,
        .working_space = layout.working_space,
        .level_zero_to_raster_scale_x = layout.level_zero_to_raster_scale_x,
        .level_zero_to_raster_scale_y = layout.level_zero_to_raster_scale_y,
        // The immutable source was fully validated before upload. Lowering consumes layout and
        // color metadata only, so a second full-raster finiteness scan would be redundant.
        .samples = {},
    };
}

} // namespace

std::vector<PreparedWarmProgram*> warm_gpu_transaction_programs(
    PreparedWarmTransaction& transaction
) {
    std::vector<PreparedWarmProgram*> result;
    result.reserve(transaction.passes.size() * 2U + 1U);
    for (PreparedWarmPass& pass : transaction.passes) {
        result.push_back(&pass.before);
        if (pass.post.has_value()) {
            result.push_back(&*pass.post);
        }
    }
    result.push_back(&transaction.final_program);
    return result;
}

std::vector<const PreparedWarmProgram*> warm_gpu_transaction_programs(
    const PreparedWarmTransaction& transaction
) {
    std::vector<const PreparedWarmProgram*> result;
    result.reserve(transaction.passes.size() * 2U + 1U);
    for (const PreparedWarmPass& pass : transaction.passes) {
        result.push_back(&pass.before);
        if (pass.post.has_value()) {
            result.push_back(&*pass.post);
        }
    }
    result.push_back(&transaction.final_program);
    return result;
}

WarmTransactionPreparation prepare_warm_gpu_transaction(
    WarmGpuResidentResources& resident,
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const WarmEditGpuRenderContext render_context,
    const std::uint32_t first_input_row_floats,
    std::size_t& operation_count,
    const std::stop_token cancellation
) {
    const WarmGpuResidentLayout& layout = resident.layout();
    const FloatRgbImage layout_only_source = source_layout(layout);
    std::string diagnostic;
    const auto prepare_program = [
        &diagnostic,
        &layout,
        &layout_only_source,
        &render_context
    ](
        const std::span<const AdjustmentNode> program_nodes,
        const EditExecutionPlan& candidate,
        const std::uint32_t input_row_floats,
        const std::uint32_t output_row_floats
    ) -> std::optional<PreparedMetalAdjustment> {
        PreparedMetalAdjustment result;
        if (candidate.segments.empty()) {
            result.invocation.width = layout.dimensions.width;
            result.invocation.height = layout.dimensions.height;
            result.invocation.step_count = 0U;
        } else {
            auto preparation = prepare_metal_adjustment(
                layout_only_source,
                program_nodes,
                candidate,
                render_context.adjustment,
                true
            );
            if (!preparation.program.has_value()) {
                diagnostic = preparation.diagnostic.empty()
                    ? "session-resident Metal could not prepare a pixel-local adjustment segment"
                    : std::move(preparation.diagnostic);
                return std::nullopt;
            }
            result = std::move(*preparation.program);
        }
        result.invocation.input_row_floats = input_row_floats;
        result.invocation.output_row_floats = output_row_floats;
        return result;
    };

    PreparedWarmTransaction result{
        .render_plan = prepare_warm_gpu_render_plan(
            nodes,
            plan,
            layout.dimensions,
            layout.working_space,
            layout.level_zero_to_raster_scale_x,
            layout.level_zero_to_raster_scale_y
        ),
        .had_active_adjustments = !plan.segments.empty(),
    };
    if (!result.render_plan.complete) {
        return WarmTransactionPreparation{
            .diagnostic =
                "session-resident Metal cannot lower every neighborhood stage in this adjustment plan",
        };
    }

    const std::uint32_t packed_row_floats = static_cast<std::uint32_t>(
        layout.adjusted_row_stride_bytes / sizeof(float)
    );
    result.passes.reserve(result.render_plan.passes.size());
    for (std::size_t index = 0U; index < result.render_plan.passes.size(); ++index) {
        const WarmGpuRenderPass& pass = result.render_plan.passes[index];
        auto before = prepare_program(
            nodes,
            pass.before,
            index == 0U ? first_input_row_floats : packed_row_floats,
            packed_row_floats
        );
        if (!before.has_value()) {
            return WarmTransactionPreparation{.diagnostic = std::move(diagnostic)};
        }
        PreparedWarmPass prepared{
            .plan_index = index,
            .before = PreparedWarmProgram{.program = std::move(*before)},
        };
        if (const auto post = stage_post_view(pass.neighbourhood); post.has_value()) {
            auto post_program = prepare_program(
                post->nodes,
                *post->plan,
                packed_row_floats,
                packed_row_floats
            );
            if (!post_program.has_value()) {
                return WarmTransactionPreparation{.diagnostic = std::move(diagnostic)};
            }
            prepared.post.emplace(
                PreparedWarmProgram{.program = std::move(*post_program)}
            );
        }
        result.passes.push_back(std::move(prepared));
    }

    auto final_program = prepare_program(
        nodes,
        result.render_plan.after,
        result.render_plan.passes.empty() ? first_input_row_floats : packed_row_floats,
        packed_row_floats
    );
    if (!final_program.has_value()) {
        return WarmTransactionPreparation{.diagnostic = std::move(diagnostic)};
    }
    result.final_program.program = std::move(*final_program);

    for (PreparedWarmProgram* program : warm_gpu_transaction_programs(result)) {
        if (operation_count > resident.operation_capacity()
            || program->program.operations.size()
                > resident.operation_capacity() - operation_count) {
            return WarmTransactionPreparation{
                .diagnostic =
                    "session-resident Metal composed plan exceeds its 256-operation slot capacity",
            };
        }
        program->operation_offset_bytes = operation_count * sizeof(MetalAdjustmentOp);
        operation_count += program->program.operations.size();

        auto attempt = resident.acquire_program_buffers(program->program, cancellation);
        if (attempt.cancelled) {
            return WarmTransactionPreparation{.cancelled = true};
        }
        if (!attempt.diagnostic.empty()) {
            return WarmTransactionPreparation{.diagnostic = std::move(attempt.diagnostic)};
        }
        program->buffers = std::move(attempt.buffers);
    }
    return WarmTransactionPreparation{.transaction = std::move(result)};
}

std::string ensure_warm_gpu_transaction_resources(
    WarmGpuSlotLease& slot,
    const PreparedWarmTransaction& transaction
) {
    for (const PreparedWarmPass& pass : transaction.passes) {
        const std::string diagnostic = ensure_warm_gpu_stage_resources(
            slot,
            transaction.render_plan.passes[pass.plan_index].neighbourhood
        );
        if (!diagnostic.empty()) {
            return diagnostic;
        }
    }
    return {};
}

void copy_warm_gpu_transaction_operations(
    id<MTLBuffer> destination,
    const PreparedWarmTransaction& transaction
) {
    auto* bytes = static_cast<std::byte*>([destination contents]);
    for (const PreparedWarmProgram* program : warm_gpu_transaction_programs(transaction)) {
        const std::size_t size =
            program->program.operations.size() * sizeof(MetalAdjustmentOp);
        if (size > 0U) {
            std::memcpy(
                bytes + program->operation_offset_bytes,
                program->program.operations.data(),
                size
            );
        }
    }
}

} // namespace shadow::image::detail
