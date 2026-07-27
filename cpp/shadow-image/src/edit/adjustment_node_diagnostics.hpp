#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace shadow::image {

struct AdjustmentNode;
enum class EditErrorCode : std::uint8_t;

namespace detail {

[[nodiscard]] std::string adjustment_node_prefix(std::size_t index, const AdjustmentNode& node);

[[noreturn]] void throw_node_error(EditErrorCode code, std::size_t index,
                                   const AdjustmentNode& node, std::string_view detail);

} // namespace detail

} // namespace shadow::image
