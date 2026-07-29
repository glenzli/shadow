#include "edit_controller.hpp"

#include <cmath>
#include <initializer_list>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage local_mask_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

} // namespace

QVariantMap EditController::selectedLocalMask() const {
    const auto* const grade_node = selectedGradeNode();
    if (grade_node == nullptr) {
        return {{QStringLiteral("kind"), 0}};
    }
    QVariantList brush_points;
    brush_points.reserve(grade_node->local_mask_brush_points.size() / 3);
    for (qsizetype offset = 0; offset + 2 < grade_node->local_mask_brush_points.size();
         offset += 3) {
        brush_points.push_back(
            QVariantMap{
                {QStringLiteral("x"), grade_node->local_mask_brush_points.at(offset)},
                {QStringLiteral("y"), grade_node->local_mask_brush_points.at(offset + 1)},
                {QStringLiteral("beginsStroke"),
                 grade_node->local_mask_brush_points.at(offset + 2) == 1.0},
            }
        );
    }
    return {
        {QStringLiteral("kind"), static_cast<int>(grade_node->local_mask_kind)},
        {QStringLiteral("x0"), grade_node->local_mask_x0},
        {QStringLiteral("y0"), grade_node->local_mask_y0},
        {QStringLiteral("x1"), grade_node->local_mask_x1},
        {QStringLiteral("y1"), grade_node->local_mask_y1},
        {QStringLiteral("radiusX"), grade_node->local_mask_radius_x},
        {QStringLiteral("radiusY"), grade_node->local_mask_radius_y},
        {QStringLiteral("feather"), grade_node->local_mask_feather},
        {QStringLiteral("inverted"), grade_node->local_mask_invert},
        {QStringLiteral("brushPoints"), brush_points},
    };
}

bool EditController::hasCopiedNodeMask() const noexcept {
    return node_mask_clipboard_.has_value();
}

void EditController::setSelectedLocalMask(const int kind) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled || kind < 0
        || kind > 3 || grade_node->local_mask_kind == kind) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    initializeLocalMask(*grade_node, kind);
    parameterEdited(QStringLiteral("local_mask/kind"), before);
    emit gradeNodesChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP(
        "EditController",
        kind == 0   ? "Removed local mask"
        : kind == 1 ? "Added linear gradient mask"
        : kind == 2 ? "Added radial gradient mask"
                    : "Added brush mask"
    )));
}

void EditController::copySelectedLocalMask() {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->local_mask_kind == 0U) {
        setStatusMessage(
            local_mask_message(QT_TRANSLATE_NOOP("EditController", "Select a node mask to copy"))
        );
        return;
    }
    finishActiveGesture();
    node_mask_clipboard_ = NodeMaskClipboard{
        .kind = grade_node->local_mask_kind,
        .x0 = grade_node->local_mask_x0,
        .y0 = grade_node->local_mask_y0,
        .x1 = grade_node->local_mask_x1,
        .y1 = grade_node->local_mask_y1,
        .radius_x = grade_node->local_mask_radius_x,
        .radius_y = grade_node->local_mask_radius_y,
        .feather = grade_node->local_mask_feather,
        .inverted = grade_node->local_mask_invert,
        .brush_points = grade_node->local_mask_brush_points,
    };
    emit nodeMaskClipboardChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP("EditController", "Copied node mask")));
}

void EditController::pasteSelectedLocalMask() {
    if (!node_mask_clipboard_.has_value()) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "Copy a node mask before pasting")
        ));
        return;
    }
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "Select an enabled Grade Node to paste the mask")
        ));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeNode candidate = *grade_node;
    const NodeMaskClipboard& source = *node_mask_clipboard_;
    candidate.local_mask_kind = source.kind;
    candidate.local_mask_x0 = source.x0;
    candidate.local_mask_y0 = source.y0;
    candidate.local_mask_x1 = source.x1;
    candidate.local_mask_y1 = source.y1;
    candidate.local_mask_radius_x = source.radius_x;
    candidate.local_mask_radius_y = source.radius_y;
    candidate.local_mask_feather = source.feather;
    candidate.local_mask_invert = source.inverted;
    candidate.local_mask_brush_points = source.brush_points;
    if (candidate == *grade_node) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "Selected Grade Node already uses the copied mask")
        ));
        return;
    }
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/paste"), before);
    emit gradeNodesChanged();
    setStatusMessage(local_mask_message(QT_TRANSLATE_NOOP("EditController", "Pasted node mask")));
}

void EditController::setSelectedLocalMaskValue(const QString& key, const double value) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->local_mask_kind == 0U
        || !acceptParameter(value, 0.0, 1.0, QT_TRANSLATE_NOOP("EditController", "Local mask"))) {
        return;
    }

    double* target = nullptr;
    if (key == QStringLiteral("x0"))
        target = &grade_node->local_mask_x0;
    else if (key == QStringLiteral("y0"))
        target = &grade_node->local_mask_y0;
    else if (key == QStringLiteral("x1"))
        target = &grade_node->local_mask_x1;
    else if (key == QStringLiteral("y1"))
        target = &grade_node->local_mask_y1;
    else if (key == QStringLiteral("radiusX"))
        target = &grade_node->local_mask_radius_x;
    else if (key == QStringLiteral("radiusY"))
        target = &grade_node->local_mask_radius_y;
    else if (key == QStringLiteral("feather"))
        target = &grade_node->local_mask_feather;
    if (target == nullptr || *target == value) {
        return;
    }

    BackendGradeNode candidate = *grade_node;
    if (key == QStringLiteral("x0"))
        candidate.local_mask_x0 = value;
    else if (key == QStringLiteral("y0"))
        candidate.local_mask_y0 = value;
    else if (key == QStringLiteral("x1"))
        candidate.local_mask_x1 = value;
    else if (key == QStringLiteral("y1"))
        candidate.local_mask_y1 = value;
    else if (key == QStringLiteral("radiusX"))
        candidate.local_mask_radius_x = value;
    else if (key == QStringLiteral("radiusY"))
        candidate.local_mask_radius_y = value;
    else
        candidate.local_mask_feather = value;
    if (candidate.local_mask_kind == 1U
        && std::hypot(
               candidate.local_mask_x1 - candidate.local_mask_x0,
               candidate.local_mask_y1 - candidate.local_mask_y0
           ) < 0.01) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A gradient mask needs two distinct points")
        ));
        return;
    }
    if (candidate.local_mask_kind == 2U
        && (candidate.local_mask_radius_x < 0.01 || candidate.local_mask_radius_y < 0.01)) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A radial mask needs a non-zero radius")
        ));
        return;
    }
    if (candidate.local_mask_kind == 3U && candidate.local_mask_radius_x < 0.005) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A brush mask needs a non-zero size")
        ));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/%1").arg(key), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskPoint(
    const QString& point,
    const double normalized_x,
    const double normalized_y
) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->local_mask_kind == 0U
        || !acceptParameter(
            normalized_x,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Local mask")
        )
        || !acceptParameter(
            normalized_y,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Local mask")
        )) {
        return;
    }

    BackendGradeNode candidate = *grade_node;
    if (point == QStringLiteral("start") && candidate.local_mask_kind == 1U) {
        candidate.local_mask_x0 = normalized_x;
        candidate.local_mask_y0 = normalized_y;
    } else if (point == QStringLiteral("end") && candidate.local_mask_kind == 1U) {
        candidate.local_mask_x1 = normalized_x;
        candidate.local_mask_y1 = normalized_y;
    } else if (point == QStringLiteral("center") && candidate.local_mask_kind == 2U) {
        candidate.local_mask_x0 = normalized_x;
        candidate.local_mask_y0 = normalized_y;
    } else {
        return;
    }

    if (candidate == *grade_node) {
        return;
    }
    if (candidate.local_mask_kind == 1U
        && std::hypot(
               candidate.local_mask_x1 - candidate.local_mask_x0,
               candidate.local_mask_y1 - candidate.local_mask_y0
           ) < 0.01) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "A gradient mask needs two distinct points")
        ));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    *grade_node = std::move(candidate);
    parameterEdited(QStringLiteral("local_mask/%1").arg(point), before);
    emit gradeNodesChanged();
}

void EditController::setSelectedLocalMaskInverted(const bool inverted) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || grade_node->local_mask_kind == 0U || grade_node->local_mask_invert == inverted) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_invert = inverted;
    parameterEdited(QStringLiteral("local_mask/invert"), before);
    emit gradeNodesChanged();
}

void EditController::appendSelectedLocalMaskBrushPoint(
    const double normalized_x,
    const double normalized_y,
    const bool begins_stroke
) {
    constexpr qsizetype maximum_brush_points = 4'096;
    constexpr double minimum_point_distance = 0.0015;
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (grade_node == nullptr || grade_node->local_mask_kind != 3U
        || !acceptParameter(
            normalized_x,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Brush mask")
        )
        || !acceptParameter(
            normalized_y,
            0.0,
            1.0,
            QT_TRANSLATE_NOOP("EditController", "Brush mask")
        )) {
        return;
    }
    const qsizetype point_count = grade_node->local_mask_brush_points.size() / 3;
    if (point_count >= maximum_brush_points) {
        setStatusMessage(local_mask_message(
            QT_TRANSLATE_NOOP("EditController", "This brush mask has reached its point limit")
        ));
        return;
    }
    if (!begins_stroke && point_count > 0) {
        const qsizetype previous = grade_node->local_mask_brush_points.size() - 3;
        if (std::hypot(
                normalized_x - grade_node->local_mask_brush_points.at(previous),
                normalized_y - grade_node->local_mask_brush_points.at(previous + 1)
            )
            < minimum_point_distance) {
            return;
        }
    }
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_brush_points.push_back(normalized_x);
    grade_node->local_mask_brush_points.push_back(normalized_y);
    grade_node->local_mask_brush_points.push_back(begins_stroke ? 1.0 : 0.0);
    parameterEdited(QStringLiteral("local_mask/brush"), before);
    emit gradeNodesChanged();
}

void EditController::clearSelectedLocalMaskBrush() {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || grade_node->local_mask_kind != 3U || grade_node->local_mask_brush_points.isEmpty()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_node->local_mask_brush_points.clear();
    parameterEdited(QStringLiteral("local_mask/brush/clear"), before);
    emit gradeNodesChanged();
}
