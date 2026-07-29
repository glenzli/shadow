#include "edit_history_restore_projection.hpp"

#include "edit_stack.hpp"

#include <algorithm>

namespace {

[[nodiscard]] QString history_grade_node_id(const std::string& key) {
    const QString value = QString::fromStdString(key);
    constexpr QLatin1StringView prefix("grade_node/");
    if (!value.startsWith(prefix)) {
        return {};
    }
    const qsizetype prefix_size = prefix.size();
    const qsizetype end = value.indexOf(QLatin1Char('/'), prefix_size);
    return end < 0 ? value.mid(prefix_size) : value.mid(prefix_size, end - prefix_size);
}

[[nodiscard]] bool undoes_insert(const std::string& history_key) {
    return history_key.ends_with("/add") || history_key.ends_with("/duplicate");
}

} // namespace

bool EditHistoryRestoreProjection::gradeNodeListChanged(
    const BackendGradeStack& before,
    const BackendGradeStack& after
) {
    if (before.grade_nodes.size() != after.grade_nodes.size()) {
        return true;
    }
    for (qsizetype index = 0; index < before.grade_nodes.size(); ++index) {
        const auto& left = before.grade_nodes.at(index);
        const auto& right = after.grade_nodes.at(index);
        if (left.grade_node_id != right.grade_node_id || left.label != right.label
            || left.enabled != right.enabled || left.local_mask_kind != right.local_mask_kind) {
            return true;
        }
    }
    return false;
}

bool EditHistoryRestoreProjection::localMaskChanged(
    const BackendGradeNode& before,
    const BackendGradeNode& after
) {
    return before.local_mask_kind != after.local_mask_kind
           || before.local_mask_x0 != after.local_mask_x0
           || before.local_mask_y0 != after.local_mask_y0
           || before.local_mask_x1 != after.local_mask_x1
           || before.local_mask_y1 != after.local_mask_y1
           || before.local_mask_radius_x != after.local_mask_radius_x
           || before.local_mask_radius_y != after.local_mask_radius_y
           || before.local_mask_feather != after.local_mask_feather
           || before.local_mask_invert != after.local_mask_invert
           || before.local_mask_brush_points != after.local_mask_brush_points;
}

QString EditHistoryRestoreProjection::preferredGradeNodeForRestore(
    const std::string& history_key,
    const BackendGradeStack& restored,
    const int previous_selected_index
) {
    QString preferred_id = history_grade_node_id(history_key);
    if (!undoes_insert(history_key) || GradeNodeStack::gradeNodeIndex(restored, preferred_id) >= 0
        || restored.grade_nodes.isEmpty()) {
        return preferred_id;
    }
    const int previous_index = std::clamp(
        previous_selected_index - 1,
        0,
        static_cast<int>(restored.grade_nodes.size() - 1)
    );
    return restored.grade_nodes.at(previous_index).grade_node_id;
}
