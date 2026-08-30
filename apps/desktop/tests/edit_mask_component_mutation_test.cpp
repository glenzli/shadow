#include "edit_mask_component_mutation.hpp"

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] BackendMaskComponent component(const QString& id, const std::uint8_t operation) {
    return BackendMaskComponent{
        .component_id = id,
        .operation = operation,
        .enabled = true,
        .kind = 3,
    };
}

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Edit mask-component mutation contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    BackendGradeNode node;
    node.local_mask_components = {
        component(QStringLiteral("base"), 0),
        component(QStringLiteral("subtract"), 2),
        component(QStringLiteral("intersect"), 3),
    };
    node.local_mask_invert = true;
    int selected = 1;
    if (!require(
            EditMaskComponentMutation::removeSelected(node, selected)
                    == EditMaskComponentRemovalResult::RemovedComponent
                && node.local_mask_components.size() == 2
                && node.local_mask_components[0].component_id == QStringLiteral("base")
                && node.local_mask_components[1].component_id == QStringLiteral("intersect")
                && node.local_mask_components[0].operation == 0 && selected == 1
                && node.local_mask_invert,
            "removing a middle component preserves order, identity, selection and final invert"
        )) {
        return EXIT_FAILURE;
    }

    selected = 0;
    if (!require(
            EditMaskComponentMutation::removeSelected(node, selected)
                    == EditMaskComponentRemovalResult::RemovedComponent
                && node.local_mask_components.size() == 1
                && node.local_mask_components[0].component_id == QStringLiteral("intersect")
                && node.local_mask_components[0].operation == 0 && selected == 0,
            "removing Base promotes the next stable component to Base"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            EditMaskComponentMutation::removeSelected(node, selected)
                    == EditMaskComponentRemovalResult::RemovedNodeMask
                && node.local_mask_components.isEmpty() && selected == 0 && !node.local_mask_invert,
            "removing the final component clears only the node mask and final inversion"
        )) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
