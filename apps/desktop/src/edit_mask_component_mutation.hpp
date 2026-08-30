#pragma once

#include "backend/edit_types.hpp"

#include <algorithm>

enum class EditMaskComponentRemovalResult : std::uint8_t {
    Invalid,
    RemovedComponent,
    RemovedNodeMask,
};

// Owns the topology-preserving part of one node-mask component removal. The
// controller remains responsible for the surrounding gesture, history,
// autosave, preview invalidation, and user-visible status transaction.
struct EditMaskComponentMutation final {
    [[nodiscard]] static EditMaskComponentRemovalResult
    removeSelected(BackendGradeNode& grade_node, int& selected_component_index) {
        if (grade_node.local_mask_components.isEmpty() || selected_component_index < 0
            || selected_component_index >= grade_node.local_mask_components.size()) {
            return EditMaskComponentRemovalResult::Invalid;
        }
        grade_node.local_mask_components.removeAt(selected_component_index);
        if (grade_node.local_mask_components.isEmpty()) {
            grade_node.local_mask_invert = false;
            selected_component_index = 0;
            return EditMaskComponentRemovalResult::RemovedNodeMask;
        }
        grade_node.local_mask_components.front().operation = 0U;
        selected_component_index = std::clamp(
            selected_component_index,
            0,
            static_cast<int>(grade_node.local_mask_components.size() - 1)
        );
        return EditMaskComponentRemovalResult::RemovedComponent;
    }
};
