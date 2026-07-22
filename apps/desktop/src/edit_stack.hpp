#pragma once

#include "desktop_backend.hpp"

#include <algorithm>

namespace GradeNodeStack {

inline constexpr int minimum_grade_node_count = 1;
inline constexpr int maximum_grade_node_count = 16;

[[nodiscard]] inline int gradeNodeIndex(
    const BackendGradeStack& grade_stack,
    const QString& grade_node_id
) noexcept {
    if (grade_node_id.isEmpty()) {
        return -1;
    }
    for (qsizetype index = 0; index < grade_stack.grade_nodes.size(); ++index) {
        if (grade_stack.grade_nodes.at(index).grade_node_id == grade_node_id) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

[[nodiscard]] inline int resolvedSelection(
    const BackendGradeStack& grade_stack,
    const QString& preferred_grade_node_id,
    const int fallback_index
) noexcept {
    const int preferred = gradeNodeIndex(grade_stack, preferred_grade_node_id);
    if (preferred >= 0) {
        return preferred;
    }
    if (grade_stack.grade_nodes.isEmpty()) {
        return -1;
    }
    const int last = static_cast<int>(grade_stack.grade_nodes.size() - 1);
    return std::clamp(fallback_index, 0, last);
}

[[nodiscard]] inline bool canInsert(
    const BackendGradeStack& grade_stack,
    const BackendGradeNode& grade_node
) noexcept {
    return grade_stack.grade_nodes.size() < maximum_grade_node_count
        && !grade_node.grade_node_id.isEmpty()
        && gradeNodeIndex(grade_stack, grade_node.grade_node_id) < 0;
}

inline bool insertAfterSelection(
    BackendGradeStack& grade_stack,
    const BackendGradeNode& grade_node,
    int& selected_index
) {
    if (!canInsert(grade_stack, grade_node)) {
        return false;
    }
    const int count = static_cast<int>(grade_stack.grade_nodes.size());
    const int insertion_index = selected_index >= 0 && selected_index < count
        ? selected_index + 1
        : count;
    grade_stack.grade_nodes.insert(insertion_index, grade_node);
    selected_index = insertion_index;
    return true;
}

inline bool deleteSelection(
    BackendGradeStack& grade_stack,
    int& selected_index
) {
    const int count = static_cast<int>(grade_stack.grade_nodes.size());
    if (count <= minimum_grade_node_count || selected_index < 0
        || selected_index >= count) {
        return false;
    }
    grade_stack.grade_nodes.removeAt(selected_index);
    selected_index = std::min(
        selected_index,
        static_cast<int>(grade_stack.grade_nodes.size() - 1)
    );
    return true;
}

inline bool moveSelection(
    BackendGradeStack& grade_stack,
    int& selected_index,
    const int destination_index
) {
    const int count = static_cast<int>(grade_stack.grade_nodes.size());
    if (selected_index < 0 || selected_index >= count || destination_index < 0
        || destination_index >= count || destination_index == selected_index) {
        return false;
    }
    grade_stack.grade_nodes.move(selected_index, destination_index);
    selected_index = destination_index;
    return true;
}

inline bool resetSelection(
    BackendGradeStack& grade_stack,
    const int selected_index
) {
    const int count = static_cast<int>(grade_stack.grade_nodes.size());
    if (selected_index < 0 || selected_index >= count) {
        return false;
    }
    auto& grade_node = grade_stack.grade_nodes[selected_index];
    const BackendBasicEditParameters neutral;
    const BackendFineEditParameters neutral_fine;
    if (grade_node.basic == neutral
        && grade_node.tone_curve_kind == ToneCurveKind::None
        && grade_node.tone_curve_master_points.isEmpty()
        && grade_node.tone_curve_red_points.isEmpty()
        && grade_node.tone_curve_green_points.isEmpty()
        && grade_node.tone_curve_blue_points.isEmpty()
        && grade_node.fine == neutral_fine) {
        return false;
    }
    grade_node.basic = neutral;
    grade_node.fine = neutral_fine;
    grade_node.tone_curve_kind = ToneCurveKind::None;
    grade_node.tone_curve_master_points.clear();
    grade_node.tone_curve_red_points.clear();
    grade_node.tone_curve_green_points.clear();
    grade_node.tone_curve_blue_points.clear();
    return true;
}

} // namespace GradeNodeStack
