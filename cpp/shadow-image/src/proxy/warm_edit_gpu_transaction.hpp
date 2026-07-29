#pragma once

#include "../edit/metal_adjustment_program.hpp"
#include "warm_edit_gpu.hpp"
#include "warm_edit_gpu_render_plan.hpp"
#include "warm_edit_gpu_resident_resources.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace shadow::image::detail {

// One prepared transaction keeps the portable render plan, its Metal programs, and every
// side-table lease together. The dispatcher owns command submission and readback; layer rendering
// can later compose several of these transactions without rebuilding or duplicating lowering.
struct PreparedWarmProgram final {
    PreparedMetalAdjustment program;
    WarmProgramBuffers buffers;
    std::size_t operation_offset_bytes = 0U;
};

struct PreparedWarmPass final {
    std::size_t plan_index = 0U;
    PreparedWarmProgram before;
    std::optional<PreparedWarmProgram> post;
};

struct PreparedWarmTransaction final {
    WarmGpuRenderPlan render_plan;
    std::vector<PreparedWarmPass> passes;
    PreparedWarmProgram final_program;
    bool had_active_adjustments = false;
};

struct WarmTransactionPreparation final {
    std::optional<PreparedWarmTransaction> transaction;
    bool cancelled = false;
    std::string diagnostic;
};

[[nodiscard]] WarmTransactionPreparation prepare_warm_gpu_transaction(
    WarmGpuResidentResources& resident,
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    WarmEditGpuRenderContext render_context,
    std::uint32_t first_input_row_floats,
    std::size_t& operation_count,
    std::stop_token cancellation
);

[[nodiscard]] std::string ensure_warm_gpu_transaction_resources(
    WarmGpuSlotLease& slot,
    const PreparedWarmTransaction& transaction
);

void copy_warm_gpu_transaction_operations(
    id<MTLBuffer> destination,
    const PreparedWarmTransaction& transaction
);

[[nodiscard]] std::vector<PreparedWarmProgram*>
warm_gpu_transaction_programs(PreparedWarmTransaction& transaction);

[[nodiscard]] std::vector<const PreparedWarmProgram*>
warm_gpu_transaction_programs(const PreparedWarmTransaction& transaction);

} // namespace shadow::image::detail
