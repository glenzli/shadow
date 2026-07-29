#pragma once

#include "warm_edit_gpu_kernel_contract.hpp"

#include <shadow/image/adjustment_layers.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shadow::image::detail {

inline constexpr std::uint32_t maximum_warm_brush_grid_side = 32U;
inline constexpr std::uint32_t maximum_warm_brush_references = 4U * 1'024U * 1'024U;

// One exact, bounded CSR grid for a continuous brush mask. Storage is already packed into
// 32-bit words so one immutable Metal buffer can be rebound at the three typed section offsets.
struct WarmGpuBrushIndex final {
    std::uint32_t grid_columns = 0U;
    std::uint32_t grid_rows = 0U;
    std::uint32_t capsule_count = 0U;
    std::uint32_t reference_count = 0U;
    std::size_t capsule_offset_bytes = 0U;
    std::size_t cell_range_offset_bytes = 0U;
    std::size_t reference_offset_bytes = 0U;
    std::vector<std::uint32_t> words;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] WarmBrushCapsule capsule(std::uint32_t index) const;
    [[nodiscard]] WarmBrushCellRange cell(std::uint32_t index) const;
    [[nodiscard]] std::uint32_t reference(std::uint32_t index) const;
};

struct WarmGpuBrushIndexPreparation final {
    std::optional<WarmGpuBrushIndex> index;
    std::string diagnostic;
};

[[nodiscard]] WarmGpuBrushIndexPreparation
prepare_warm_gpu_brush_index(const LocalMask& mask, Dimensions full_dimensions);

} // namespace shadow::image::detail
