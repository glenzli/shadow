#include "edit_history_restore_projection.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Edit-history restore projection contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] BackendGradeNode grade_node(const QString& id) {
    return {
        .grade_node_id = id,
        .label = id,
    };
}

[[nodiscard]] BackendMaskComponent mask_component() {
    return {
        .component_id = QStringLiteral("11111111-1111-4111-8111-111111111111"),
        .operation = 0,
        .enabled = true,
        .kind = 3,
    };
}

void inserted_node_undo_restores_its_predecessor() {
    BackendGradeStack restored{
        .grade_nodes = {
            grade_node(QStringLiteral("first")),
            grade_node(QStringLiteral("selected")),
            grade_node(QStringLiteral("next")),
        },
    };
    require(
        EditHistoryRestoreProjection::preferredGradeNodeForRestore(
            "grade_node/masked/add",
            restored,
            2
        ) == QStringLiteral("selected"),
        "undoing a middle insertion must restore the node selected before creation"
    );
    require(
        EditHistoryRestoreProjection::preferredGradeNodeForRestore(
            "grade_node/selected/local_mask/invert",
            restored,
            1
        ) == QStringLiteral("selected"),
        "a parameter undo must preserve its surviving node identity"
    );
}

void list_projection_detects_mask_type_changes() {
    BackendGradeStack before{
        .grade_nodes = {grade_node(QStringLiteral("node"))},
    };
    BackendGradeStack after = before;
    after.grade_nodes[0].local_mask_components = {mask_component()};
    require(
        EditHistoryRestoreProjection::gradeNodeListChanged(before, after),
        "adding or removing a mask must refresh the node-row icon"
    );
    before = after;
    after.grade_nodes[0].local_mask_components[0].kind = 2;
    require(
        EditHistoryRestoreProjection::gradeNodeListChanged(before, after),
        "changing the sole component kind must refresh the node-row icon"
    );
    after = before;
    after.grade_nodes[0].basic.exposure_stops = 1.0;
    require(
        !EditHistoryRestoreProjection::gradeNodeListChanged(before, after),
        "ordinary parameters do not change the node-list presentation"
    );
}

void parameter_projection_detects_every_mask_field() {
    const BackendGradeNode before = grade_node(QStringLiteral("node"));
    BackendGradeNode after = before;
    after.local_mask_components = {mask_component()};
    require(
        EditHistoryRestoreProjection::localMaskChanged(before, after),
        "mask kind changes must refresh selected-mask parameters"
    );
    const BackendGradeNode masked = after;
    after = masked;
    after.local_mask_components[0].x0 = 0.1;
    require(
        EditHistoryRestoreProjection::localMaskChanged(masked, after),
        "mask x0 must refresh selected-mask parameters"
    );
    after = masked;
    after.local_mask_components[0].y0 = 0.1;
    require(
        EditHistoryRestoreProjection::localMaskChanged(masked, after),
        "mask y0 must refresh selected-mask parameters"
    );
    after = masked;
    after.local_mask_components[0].x1 = 0.8;
    require(
        EditHistoryRestoreProjection::localMaskChanged(masked, after),
        "mask x1 must refresh selected-mask parameters"
    );
    after = masked;
    after.local_mask_components[0].y1 = 0.8;
    require(
        EditHistoryRestoreProjection::localMaskChanged(masked, after),
        "mask y1 must refresh selected-mask parameters"
    );
    after = masked;
    after.local_mask_components[0].radius_x = 0.2;
    require(
        EditHistoryRestoreProjection::localMaskChanged(masked, after),
        "mask radius x must refresh selected-mask parameters"
    );
    after = masked;
    after.local_mask_components[0].radius_y = 0.2;
    require(
        EditHistoryRestoreProjection::localMaskChanged(masked, after),
        "mask radius y must refresh selected-mask parameters"
    );
    after = masked;
    after.local_mask_components[0].feather = 0.4;
    require(
        EditHistoryRestoreProjection::localMaskChanged(masked, after),
        "mask feather must refresh selected-mask parameters"
    );
    after = masked;
    after.local_mask_invert = true;
    require(
        EditHistoryRestoreProjection::localMaskChanged(masked, after),
        "mask inversion must refresh selected-mask parameters"
    );
    after = masked;
    after.local_mask_components[0].brush_points = {0.2, 0.3, 1.0};
    require(
        EditHistoryRestoreProjection::localMaskChanged(masked, after),
        "brush coverage must refresh selected-mask parameters"
    );
    require(
        !EditHistoryRestoreProjection::localMaskChanged(masked, masked),
        "an identical mask must not emit a redundant parameter refresh"
    );
}

} // namespace

int main() {
    inserted_node_undo_restores_its_predecessor();
    list_projection_detects_mask_type_changes();
    parameter_projection_detects_every_mask_field();
    return EXIT_SUCCESS;
}
