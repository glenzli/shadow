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
        const std::uint8_t left_single_mask_kind =
            left.local_mask_components.size() == 1 ? left.local_mask_components.front().kind : 0U;
        const std::uint8_t right_single_mask_kind =
            right.local_mask_components.size() == 1 ? right.local_mask_components.front().kind : 0U;
        if (left.grade_node_id != right.grade_node_id || left.label != right.label
            || left.enabled != right.enabled
            || left.local_mask_components.size() != right.local_mask_components.size()
            || left_single_mask_kind != right_single_mask_kind) {
            return true;
        }
    }
    return false;
}

bool EditHistoryRestoreProjection::localMaskChanged(
    const BackendGradeNode& before,
    const BackendGradeNode& after
) {
    return before.local_mask_components != after.local_mask_components
           || before.local_mask_invert != after.local_mask_invert;
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
