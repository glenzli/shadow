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

bool EditController::applyXmpDevelopImport(
    const XmpDevelopImport& imported,
    const QString& label,
    QString* const error_text
) {
    const auto fail = [error_text](const QString& error) {
        if (error_text != nullptr)
            *error_text = error;
        return false;
    };
    if (!active_ || interactionLocked() || !canAddGradeNode() || !imported.canApply()) {
        return fail(tr("The XMP adjustments cannot be applied to the current photo."));
    }
    BackendGradeNode node;
    try {
        node = backend_->newBasicGradeNode(tr("Imported XMP · %1").arg(label));
    } catch (const std::exception& error) {
        return fail(QString::fromUtf8(error.what()));
    }
    for (const auto& adjustment : imported.adjustments) {
        const double value = adjustment.target_value;
        switch (adjustment.target) {
        case XmpDevelopTarget::ExposureStops:
            node.basic.exposure_stops = value;
            break;
        case XmpDevelopTarget::ContrastFactor:
            node.basic.contrast_factor = value;
            break;
        case XmpDevelopTarget::SaturationFactor:
            node.basic.saturation_factor = value;
            break;
        case XmpDevelopTarget::Highlights:
            node.fine.highlights = value;
            break;
        case XmpDevelopTarget::Shadows:
            node.fine.shadows = value;
            break;
        case XmpDevelopTarget::Whites:
            node.fine.whites = value;
            break;
        case XmpDevelopTarget::Blacks:
            node.fine.blacks = value;
            break;
        case XmpDevelopTarget::Texture:
            node.fine.texture = value;
            break;
        case XmpDevelopTarget::Clarity:
            node.fine.clarity = value;
            break;
        case XmpDevelopTarget::Dehaze:
            node.fine.dehaze = value;
            break;
        case XmpDevelopTarget::Vibrance:
            node.fine.vibrance = value;
            break;
        }
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = before;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::insertAfterSelection(updated, node, selection)) {
        return fail(tr("The XMP adjustments cannot be applied to the current photo."));
    }
    const BackendGradeStack expected = updated;
    setGradeStack(std::move(updated), node.grade_node_id);
    if (grade_stack_ != expected) {
        setGradeStack(before);
        return fail(tr("The XMP adjustments cannot be applied to the current photo."));
    }
    selectGradeNode(selection);
    recordWorkingTransition(QStringLiteral("xmp/import/%1").arg(node.grade_node_id), before);
    schedulePreview(0);
    if (error_text != nullptr)
        error_text->clear();
    return true;
}

void EditController::reportShadowRecipeExported(const QString& file_name) {
    setStatusMessage(interchange_message(
        QT_TRANSLATE_NOOP("EditController", "Exported Shadow Recipe · %1"),
        {file_name}
    ));
}
