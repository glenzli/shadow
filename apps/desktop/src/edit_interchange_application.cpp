#include "edit_controller.hpp"

#include "edit_stack.hpp"

#include <initializer_list>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage interchange_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

} // namespace

const BackendGradeStack& EditController::gradeStackForInterchange() const noexcept {
    return grade_stack_;
}

bool EditController::applyShadowRecipeGradeNodes(
    const BackendGradeStack& portable_grade_stack,
    QString* const error_text
) {
    const auto fail = [error_text](const QString& error) {
        if (error_text != nullptr) {
            *error_text = error;
        }
        return false;
    };
    if (!active_) {
        return fail(tr("Open a photo before importing a Shadow Recipe."));
    }
    if (interactionLocked()) {
        return fail(tr("Wait for the current edit operation to finish."));
    }
    if (portable_grade_stack.grade_nodes.isEmpty()) {
        return fail(tr("This Shadow Recipe does not contain an editable Grade Node."));
    }
    if (portable_grade_stack.grade_nodes.size() > GradeNodeStack::maximum_grade_node_count) {
        return fail(tr("This Shadow Recipe exceeds the 16-Grade-Node desktop limit."));
    }

    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack imported = grade_stack_;
    imported.grade_nodes = portable_grade_stack.grade_nodes;
    const QString preferred_id = imported.grade_nodes.constFirst().grade_node_id;
    const BackendGradeStack expected = imported;
    setGradeStack(std::move(imported), preferred_id);
    if (grade_stack_ != expected) {
        return fail(tr("Shadow could not represent this Recipe safely."));
    }

    recordWorkingTransition(QStringLiteral("recipe/import"), before);
    schedulePreview(0);
    setStatusMessage(interchange_message(
        QT_TRANSLATE_NOOP("EditController", "Imported Shadow Recipe · %1 Grade Nodes"),
        {portable_grade_stack.grade_nodes.size()}
    ));
    if (error_text != nullptr) {
        error_text->clear();
    }
    return true;
}

void EditController::reportShadowRecipeExported(const QString& file_name) {
    setStatusMessage(interchange_message(
        QT_TRANSLATE_NOOP("EditController", "Exported Shadow Recipe · %1"),
        {file_name}
    ));
}
