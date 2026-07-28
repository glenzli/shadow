#include "adjustment_node_diagnostics.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/edit_error.hpp>

#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace shadow::image::detail {

std::string adjustment_node_prefix(const std::size_t index, const AdjustmentNode& node) {
    std::ostringstream message;
    message << "edit node " << index;
    if (!node.node_id.empty()) {
        message << " (" << node.node_id << ')';
    }
    message << ": ";
    return message.str();
}

[[noreturn]] void throw_node_error(const EditErrorCode code, const std::size_t index,
                                   const AdjustmentNode& node, const std::string_view detail) {
    throw EditError(code, index, adjustment_node_prefix(index, node) + std::string(detail));
}

float checked_edit_pixel_float(const double value, const std::size_t node_index,
                               const AdjustmentNode& node) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maximum || value > maximum) {
        throw_node_error(EditErrorCode::numeric_overflow, node_index, node,
                         "pixel result exceeded finite float32 range");
    }
    return static_cast<float>(value);
}

} // namespace shadow::image::detail
