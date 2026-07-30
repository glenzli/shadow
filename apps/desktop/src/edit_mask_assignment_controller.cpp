#include "edit_controller.hpp"

#include "edit_stack.hpp"

#include <exception>
#include <initializer_list>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage mask_assignment_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

} // namespace

void EditController::initializeLocalMask(BackendGradeNode& grade_node, const int kind) {
    grade_node.local_mask_kind = static_cast<std::uint8_t>(kind);
    grade_node.local_mask_invert = false;
    grade_node.local_mask_brush_points.clear();
    grade_node.local_mask_x0 = 0.0;
    grade_node.local_mask_y0 = 0.0;
    grade_node.local_mask_x1 = 0.0;
    grade_node.local_mask_y1 = 0.0;
    grade_node.local_mask_radius_x = 0.0;
    grade_node.local_mask_radius_y = 0.0;
    grade_node.local_mask_feather = 0.0;
    if (kind == 1) {
        grade_node.local_mask_x0 = 0.25;
        grade_node.local_mask_y0 = 0.5;
        grade_node.local_mask_x1 = 0.75;
        grade_node.local_mask_y1 = 0.5;
    } else if (kind == 2) {
        grade_node.local_mask_x0 = 0.5;
        grade_node.local_mask_y0 = 0.5;
        grade_node.local_mask_radius_x = 0.28;
        grade_node.local_mask_radius_y = 0.28;
        grade_node.local_mask_feather = 0.35;
    } else if (kind == 3) {
        grade_node.local_mask_radius_x = 0.035;
        grade_node.local_mask_feather = 0.6;
    } else if (kind == 4) {
        grade_node.local_mask_x0 = 0.2;
        grade_node.local_mask_x1 = 0.8;
        grade_node.local_mask_feather = 0.08;
    } else if (kind == 5) {
        grade_node.local_mask_x0 = 30.0 / 360.0;
        grade_node.local_mask_x1 = 30.0 / 180.0;
        grade_node.local_mask_feather = 0.45;
    }
}

bool EditController::createLocalMask(const int kind, const int destination) {
    constexpr int current_node_destination = 0;
    constexpr int new_node_destination = 1;
    if (!active_ || interactionLocked() || kind < 1 || kind > 5
        || (destination != current_node_destination && destination != new_node_destination)) {
        return false;
    }

    finishActiveGesture();
    if (destination == current_node_destination) {
        auto* const grade_node = selected_grade_node_index_ < 0
                                     ? nullptr
                                     : &grade_stack_.grade_nodes[selected_grade_node_index_];
        if (grade_node == nullptr || !grade_node->enabled) {
            setStatusMessage(mask_assignment_message(QT_TRANSLATE_NOOP(
                "EditController",
                "Select an enabled Grade Node before adding a mask"
            )));
            return false;
        }
        if (grade_node->local_mask_kind != 0U) {
            setStatusMessage(mask_assignment_message(QT_TRANSLATE_NOOP(
                "EditController",
                "This Grade Node already has a mask · edit it or create a new node"
            )));
            return false;
        }
        const BackendGradeStack before = grade_stack_;
        initializeLocalMask(*grade_node, kind);
        parameterEdited(QStringLiteral("local_mask/create"), before);
        emit gradeNodesChanged();
        setStatusMessage(mask_assignment_message(
            QT_TRANSLATE_NOOP("EditController", "Added mask to current Grade Node")
        ));
        return true;
    }

    if (!canAddGradeNode()) {
        setStatusMessage(mask_assignment_message(
            QT_TRANSLATE_NOOP("EditController", "An edit can contain at most 16 Grade Nodes")
        ));
        return false;
    }
    BackendGradeNode grade_node;
    try {
        grade_node =
            backend_->newBasicGradeNode(uniqueGradeNodeLabel(QStringLiteral("Adjustment Node")));
    } catch (const std::exception& error) {
        setStatusMessage(mask_assignment_message(
            QT_TRANSLATE_NOOP("EditController", "Could not create masked Grade Node · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return false;
    }
    initializeLocalMask(grade_node, kind);

    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::insertAfterSelection(updated, grade_node, selection)) {
        setStatusMessage(mask_assignment_message(QT_TRANSLATE_NOOP(
            "EditController",
            "The masked Grade Node could not be inserted safely"
        )));
        return false;
    }
    setGradeStack(std::move(updated), grade_node.grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/add").arg(grade_node.grade_node_id),
        before
    );
    schedulePreview(0);
    setStatusMessage(mask_assignment_message(
        QT_TRANSLATE_NOOP("EditController", "Created a new Grade Node with a mask")
    ));
    return true;
}
