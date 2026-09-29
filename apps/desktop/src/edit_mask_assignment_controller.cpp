#include "edit_condition_mask_controller.hpp"
#include "edit_controller.hpp"

#include "edit_stack.hpp"

#include <QUuid>

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

void EditController::initializeLocalMaskComponent(
    BackendMaskComponent& component,
    const int kind,
    const int operation
) {
    component = BackendMaskComponent{};
    component.component_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    component.operation = static_cast<std::uint8_t>(operation);
    component.kind = static_cast<std::uint8_t>(kind);
    if (kind == 1) {
        component.x0 = 0.25;
        component.y0 = 0.5;
        component.x1 = 0.75;
        component.y1 = 0.5;
    } else if (kind == 2) {
        component.x0 = 0.5;
        component.y0 = 0.5;
        component.radius_x = 0.28;
        component.radius_y = 0.28;
        component.feather = 0.35;
    } else if (kind == 3) {
        component.radius_x = 0.035;
        component.feather = 0.6;
    } else if (kind == 4) {
        component.x0 = 0.2;
        component.x1 = 0.8;
        component.feather = 0.08;
    } else if (kind == 5) {
        component.x0 = 30.0 / 360.0;
        component.x1 = 30.0 / 180.0;
        component.feather = 0.45;
    } else if (kind == 7) {
        component.condition_expression = defaultConditionMaskExpression();
    }
}

bool EditController::createLocalMask(const int kind, const int destination) {
    constexpr int current_node_destination = 0;
    constexpr int new_node_destination = 1;
    if (!active_ || interactionLocked() || kind < 1 || (kind > 5 && kind != 7)
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
        if (!grade_node->local_mask_components.isEmpty()) {
            setStatusMessage(mask_assignment_message(QT_TRANSLATE_NOOP(
                "EditController",
                "This Grade Node already has a mask · edit it or create a new node"
            )));
            return false;
        }
        const BackendGradeStack before = grade_stack_;
        BackendMaskComponent component;
        initializeLocalMaskComponent(component, kind, 0);
        grade_node->local_mask_components.push_back(std::move(component));
        grade_node->local_mask_invert = false;
        selected_local_mask_component_index_ = 0;
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
    BackendMaskComponent component;
    initializeLocalMaskComponent(component, kind, 0);
    grade_node.local_mask_components.push_back(std::move(component));
    grade_node.local_mask_invert = false;
    selected_local_mask_component_index_ = 0;

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

bool EditController::addLocalMaskComponent(const int kind, const int operation) {
    auto* const grade_node = selected_grade_node_index_ < 0
                                 ? nullptr
                                 : &grade_stack_.grade_nodes[selected_grade_node_index_];
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled || kind < 1
        || (kind > 5 && kind != 7) || operation < 1 || operation > 3
        || grade_node->local_mask_components.isEmpty()) {
        return false;
    }
    if (grade_node->local_mask_components.size() >= BACKEND_MAX_MASK_COMPONENTS) {
        setStatusMessage(mask_assignment_message(
            QT_TRANSLATE_NOOP("EditController", "A node mask can contain at most 8 components")
        ));
        return false;
    }

    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendMaskComponent component;
    initializeLocalMaskComponent(component, kind, operation);
    grade_node->local_mask_components.push_back(std::move(component));
    selected_local_mask_component_index_ =
        static_cast<int>(grade_node->local_mask_components.size() - 1);
    parameterEdited(QStringLiteral("local_mask/component/add"), before);
    emit gradeNodesChanged();
    setStatusMessage(mask_assignment_message(
        operation == 1   ? QT_TRANSLATE_NOOP("EditController", "Added mask component")
        : operation == 2 ? QT_TRANSLATE_NOOP("EditController", "Added subtracting mask component")
                         : QT_TRANSLATE_NOOP("EditController", "Added intersecting mask component")
    ));
    return true;
}
