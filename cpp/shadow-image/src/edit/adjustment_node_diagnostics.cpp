#include "adjustment_node_diagnostics.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/edit_error.hpp>

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

} // namespace shadow::image::detail
