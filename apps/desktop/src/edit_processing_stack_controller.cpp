#include "edit_controller.hpp"

#include "ai_preferences.hpp"

#include <algorithm>
#include <initializer_list>

namespace {

[[nodiscard]] LocalizedUiMessage processing_stack_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

} // namespace

// Optional fixed-node collection mutations live here. Model execution remains
// in EditRawFoundationController and authored crop parameters remain in
// edit_geometry_controller.cpp.
bool EditController::rawDenoiseNodeMaterialized() const noexcept {
    return grade_stack_.raw_ai_denoise.present;
}

bool EditController::rawDenoiseNodeVisible() const noexcept {
    return grade_stack_.raw_ai_denoise.present && !grade_stack_.raw_ai_denoise.bypassed;
}

bool EditController::canvasNodeMaterialized() const noexcept {
    return grade_stack_.geometry.present;
}

bool EditController::canvasNodeEnabled() const noexcept {
    return grade_stack_.geometry.present && grade_stack_.geometry.enabled;
}

void EditController::addRawDenoiseNode() {
    if (!active_ || interactionLocked() || grade_stack_.raw_ai_denoise.present) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.raw_ai_denoise = {
        .present = true,
        .enabled = false,
        .bypassed = false,
        .model = 0,
        .amount_percent = static_cast<std::uint8_t>(
            ai_preferences_ == nullptr ? 100 : ai_preferences_->rawDenoiseDefaultAmount()
        ),
    };
    rawDenoiseEdited(QStringLiteral("add"), before);
    selectRawDenoiseNode();
    setStatusMessage(processing_stack_message(
        QT_TRANSLATE_NOOP("EditController", "AI RAW Denoise added · generate when needed")
    ));
}

void EditController::setRawDenoiseNodeVisible(const bool visible) {
    if (!active_ || interactionLocked() || !grade_stack_.raw_ai_denoise.present
        || grade_stack_.raw_ai_denoise.bypassed == !visible) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.raw_ai_denoise.bypassed = !visible;
    rawDenoiseEdited(QStringLiteral("visibility"), before);
    setStatusMessage(processing_stack_message(
        visible ? QT_TRANSLATE_NOOP("EditController", "AI RAW Denoise node is visible")
                : QT_TRANSLATE_NOOP("EditController", "AI RAW Denoise node is hidden")
    ));
}

void EditController::removeRawDenoiseNode() {
    if (!active_ || interactionLocked() || !grade_stack_.raw_ai_denoise.present) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.raw_ai_denoise = {};
    rawDenoiseEdited(QStringLiteral("remove"), before);
    if (selected_recipe_node_kind_ == QStringLiteral("raw_denoise")) {
        selectFoundationNode();
    }
    setStatusMessage(
        processing_stack_message(QT_TRANSLATE_NOOP("EditController", "AI RAW Denoise removed"))
    );
}

void EditController::addCanvasNode() {
    if (!active_ || interactionLocked() || grade_stack_.geometry.present) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry.present = true;
    grade_stack_.geometry.enabled = true;
    parameterEdited(QStringLiteral("geometry/add"), before);
    selectCanvasNode();
    setStatusMessage(
        processing_stack_message(QT_TRANSLATE_NOOP("EditController", "Crop & Geometry added"))
    );
}

void EditController::removeCanvasNode() {
    if (!active_ || interactionLocked() || !grade_stack_.geometry.present) {
        return;
    }
    finishActiveGesture();
    if (crop_tool_active_) {
        setCropToolActive(false);
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry = {};
    parameterEdited(QStringLiteral("geometry/remove"), before);
    if (selected_recipe_node_kind_ == QStringLiteral("canvas")) {
        if (grade_stack_.grade_nodes.empty()) {
            selectFoundationNode();
        } else {
            const int final_index = static_cast<int>(grade_stack_.grade_nodes.size()) - 1;
            selectGradeNode(std::clamp(selected_grade_node_index_, 0, final_index));
        }
    }
    setStatusMessage(
        processing_stack_message(QT_TRANSLATE_NOOP("EditController", "Crop & Geometry removed"))
    );
}

void EditController::setCanvasNodeEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || !grade_stack_.geometry.present
        || grade_stack_.geometry.enabled == enabled) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.geometry.enabled = enabled;
    parameterEdited(QStringLiteral("geometry/enabled"), before);
    setStatusMessage(processing_stack_message(
        enabled
            ? QT_TRANSLATE_NOOP("EditController", "Crop & Geometry enabled")
            : QT_TRANSLATE_NOOP("EditController", "Crop & Geometry bypassed · settings preserved")
    ));
}
