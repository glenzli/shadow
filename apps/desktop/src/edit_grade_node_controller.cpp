#include "edit_controller.hpp"

#include "edit_stack.hpp"

#include <QCoreApplication>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <utility>

namespace {

[[nodiscard]] LocalizedUiMessage grade_node_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

[[nodiscard]] QString display_grade_node_label(const QString& stored_label) {
    constexpr auto context = "EditController";
    const QString numbered_copy_marker = QStringLiteral(" Copy ");
    const qsizetype numbered_copy_index = stored_label.lastIndexOf(numbered_copy_marker);
    if (numbered_copy_index > 0) {
        const QString suffix = stored_label.mid(numbered_copy_index + numbered_copy_marker.size());
        bool valid_number = false;
        const int number = suffix.toInt(&valid_number);
        if (valid_number && number >= 2 && QString::number(number) == suffix) {
            const QString base = stored_label.left(numbered_copy_index);
            return QCoreApplication::translate(
                       context,
                       QT_TRANSLATE_NOOP("EditController", "%1 Copy %2")
            )
                .arg(display_grade_node_label(base))
                .arg(number);
        }
    }
    const QString copy_suffix = QStringLiteral(" Copy");
    if (stored_label.endsWith(copy_suffix) && stored_label.size() > copy_suffix.size()) {
        const QString base = stored_label.left(stored_label.size() - copy_suffix.size());
        return QCoreApplication::translate(context, QT_TRANSLATE_NOOP("EditController", "%1 Copy"))
            .arg(display_grade_node_label(base));
    }
    if (stored_label.compare(QStringLiteral("Adjustment Node"), Qt::CaseInsensitive) == 0
        || stored_label.compare(QStringLiteral("Adjustments"), Qt::CaseInsensitive) == 0) {
        return QCoreApplication::translate(
            context,
            QT_TRANSLATE_NOOP("EditController", "Adjustment Node")
        );
    }
    const QString current_numbered_prefix = QStringLiteral("Adjustment Node ");
    const QString legacy_numbered_prefix = QStringLiteral("Adjustments ");
    const QString numbered_prefix =
        stored_label.startsWith(current_numbered_prefix, Qt::CaseInsensitive)
            ? current_numbered_prefix
            : legacy_numbered_prefix;
    if (stored_label.startsWith(numbered_prefix, Qt::CaseInsensitive)) {
        const QString suffix = stored_label.mid(numbered_prefix.size());
        bool valid_number = false;
        const int number = suffix.toInt(&valid_number);
        if (valid_number && number >= 2 && QString::number(number) == suffix) {
            return QCoreApplication::translate(
                       context,
                       QT_TRANSLATE_NOOP("EditController", "Adjustment Node %1")
            )
                .arg(number);
        }
    }
    return stored_label;
}

} // namespace

QVariantList EditController::gradeNodes() const {
    QVariantList result;
    result.reserve(grade_stack_.grade_nodes.size());
    for (qsizetype index = 0; index < grade_stack_.grade_nodes.size(); ++index) {
        const auto& grade_node = grade_stack_.grade_nodes.at(index);
        QVariantMap item;
        item.insert(QStringLiteral("gradeNodeId"), grade_node.grade_node_id);
        item.insert(QStringLiteral("sharedLayerId"), grade_node.shared_layer_id);
        item.insert(QStringLiteral("sharedRevisionId"), grade_node.shared_revision_id);
        item.insert(
            QStringLiteral("shared"),
            !grade_node.shared_layer_id.isEmpty() && !grade_node.shared_revision_id.isEmpty()
        );
        const auto shared = std::find_if(
            shared_grade_nodes_.cbegin(),
            shared_grade_nodes_.cend(),
            [&grade_node](const BackendSharedGradeNode& candidate) {
                return candidate.layer_id == grade_node.shared_layer_id
                       && candidate.revision_id == grade_node.shared_revision_id;
            }
        );
        item.insert(
            QStringLiteral("sharedRevisionNumber"),
            shared == shared_grade_nodes_.cend() ? 0 : static_cast<int>(shared->revision_number)
        );
        item.insert(QStringLiteral("label"), display_grade_node_label(grade_node.label));
        item.insert(QStringLiteral("rawLabel"), grade_node.label);
        item.insert(QStringLiteral("strengthPercent"), qRound(grade_node.opacity * 100.0));
        item.insert(QStringLiteral("enabled"), grade_node.enabled);
        item.insert(QStringLiteral("hasLocalMask"), !grade_node.local_mask_components.isEmpty());
        item.insert(
            QStringLiteral("localMaskKind"),
            grade_node.local_mask_components.size() == 1
                ? static_cast<int>(grade_node.local_mask_components.front().kind)
                : 0
        );
        item.insert(
            QStringLiteral("localMaskComponentCount"),
            grade_node.local_mask_components.size()
        );
        item.insert(QStringLiteral("index"), static_cast<int>(index));
        result.push_back(item);
    }
    return result;
}

QVariantList EditController::sharedGradeNodes() const {
    QVariantList result;
    result.reserve(shared_grade_nodes_.size());
    for (const auto& shared : shared_grade_nodes_) {
        result.push_back(
            QVariantMap{
                {QStringLiteral("layerId"), shared.layer_id},
                {QStringLiteral("revisionId"), shared.revision_id},
                {QStringLiteral("revisionNumber"), shared.revision_number},
                {QStringLiteral("label"), shared.label},
            }
        );
    }
    return result;
}

int EditController::selectedGradeNodeIndex() const noexcept {
    return selected_grade_node_index_;
}

QString EditController::selectedGradeNodeId() const {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? QString{} : grade_node->grade_node_id;
}

bool EditController::hasSelectedGradeNode() const noexcept {
    return selectedGradeNode() != nullptr;
}

bool EditController::foundationSelected() const noexcept {
    return selected_recipe_node_kind_ == QStringLiteral("foundation");
}

bool EditController::rawDenoiseSelected() const noexcept {
    return grade_stack_.raw_ai_denoise.present
           && selected_recipe_node_kind_ == QStringLiteral("raw_denoise");
}

QString EditController::selectedRecipeNodeKind() const {
    return selected_recipe_node_kind_;
}

bool EditController::retouchNodeMaterialized() const noexcept {
    return !grade_stack_.retouch_spots.isEmpty() || !grade_stack_.retouch_strokes.isEmpty();
}

bool EditController::liquifyNodeMaterialized() const noexcept {
    return !grade_stack_.liquify_strokes.isEmpty();
}

bool EditController::canAddGradeNode() const noexcept {
    return active_ && !interactionLocked()
           && grade_stack_.grade_nodes.size() < GradeNodeStack::maximum_grade_node_count;
}

bool EditController::canDeleteGradeNode() const noexcept {
    return active_ && !interactionLocked() && hasSelectedGradeNode()
           && grade_stack_.grade_nodes.size() > GradeNodeStack::minimum_grade_node_count;
}

bool EditController::canMoveGradeNodeUp() const noexcept {
    return active_ && !interactionLocked() && hasSelectedGradeNode()
           && selected_grade_node_index_ > 0;
}

bool EditController::canMoveGradeNodeDown() const noexcept {
    const int count = static_cast<int>(grade_stack_.grade_nodes.size());
    return active_ && !interactionLocked() && hasSelectedGradeNode()
           && selected_grade_node_index_ >= 0 && selected_grade_node_index_ + 1 < count;
}

bool EditController::gradeNodeEnabled() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node != nullptr && grade_node->enabled;
}

double EditController::gradeNodeStrength() const noexcept {
    const auto* const grade_node = selectedGradeNode();
    return grade_node == nullptr ? 1.0 : grade_node->opacity;
}

void EditController::setGradeNodeStrength(const double strength) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr || !grade_node->enabled
        || !std::isfinite(strength) || strength < 0.0 || strength > 1.0
        || grade_node->opacity == strength) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_].opacity = strength;
    parameterEdited(QStringLiteral("node/strength"), before);
    emit gradeNodesChanged();
}

void EditController::setGradeNodeEnabled(const bool enabled) {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr
        || grade_node->enabled == enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    const QString grade_node_id = grade_node->grade_node_id;
    grade_stack_.grade_nodes[selected_grade_node_index_].enabled = enabled;
    recordWorkingTransition(QStringLiteral("grade_node/%1/enabled").arg(grade_node_id), before);
    emit gradeNodesChanged();
    emit gradeNodeEnabledChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(0);
    setStatusMessage(grade_node_message(
        enabled ? QT_TRANSLATE_NOOP("EditController", "Grade Node enabled")
                : QT_TRANSLATE_NOOP("EditController", "Grade Node bypassed · settings preserved")
    ));
}

void EditController::selectGradeNode(const int index) {
    const int count = static_cast<int>(grade_stack_.grade_nodes.size());
    if (!active_ || interactionLocked() || index < 0 || index >= count
        || (selected_recipe_node_kind_ == QStringLiteral("grade")
            && index == selected_grade_node_index_)) {
        return;
    }
    finishActiveGesture();
    setPointColorPickerActive(false);
    setRetouchPickerActive(false);
    setWhiteBalancePickerActive(false);
    const bool left_structural = selected_recipe_node_kind_ != QStringLiteral("grade");
    selected_recipe_node_kind_ = QStringLiteral("grade");
    setGradeStack(grade_stack_, grade_stack_.grade_nodes.at(index).grade_node_id);
    if (left_structural) {
        emit selectedGradeNodeChanged();
        emit gradeNodeActionsChanged();
        emit gradeNodeEnabledChanged();
        notifyParametersChanged();
        emit toneCurveChanged();
    }
}

void EditController::selectFoundationNode() {
    if (!active_ || interactionLocked()
        || selected_recipe_node_kind_ == QStringLiteral("foundation")) {
        return;
    }
    finishActiveGesture();
    setPointColorPickerActive(false);
    setRetouchPickerActive(false);
    setWhiteBalancePickerActive(false);
    selected_recipe_node_kind_ = QStringLiteral("foundation");
    selected_point_color_index_ = -1;
    clearPointColorScopeReference();
    const QVector<ToneCurvePoint> neutral_curve{{0.0, 0.0}, {1.0, 1.0}};
    if (tone_curve_points_.points() != neutral_curve) {
        static_cast<void>(tone_curve_points_.replace(neutral_curve));
    }
    emit selectedGradeNodeChanged();
    emit gradeNodeActionsChanged();
    emit gradeNodeEnabledChanged();
    notifyParametersChanged();
    emit toneCurveChanged();
}

void EditController::selectRawDenoiseNode() {
    if (!active_ || interactionLocked() || !grade_stack_.raw_ai_denoise.present
        || selected_recipe_node_kind_ == QStringLiteral("raw_denoise")) {
        return;
    }
    finishActiveGesture();
    setPointColorPickerActive(false);
    setRetouchPickerActive(false);
    setWhiteBalancePickerActive(false);
    selected_recipe_node_kind_ = QStringLiteral("raw_denoise");
    selected_point_color_index_ = -1;
    clearPointColorScopeReference();
    const QVector<ToneCurvePoint> neutral_curve{{0.0, 0.0}, {1.0, 1.0}};
    if (tone_curve_points_.points() != neutral_curve) {
        static_cast<void>(tone_curve_points_.replace(neutral_curve));
    }
    emit selectedGradeNodeChanged();
    emit gradeNodeActionsChanged();
    emit gradeNodeEnabledChanged();
    notifyParametersChanged();
    emit toneCurveChanged();
}

void EditController::selectRetouchNode() {
    if (!active_ || interactionLocked()
        || selected_recipe_node_kind_ == QStringLiteral("retouch")) {
        return;
    }
    finishActiveGesture();
    setPointColorPickerActive(false);
    setRetouchPickerActive(false);
    setWhiteBalancePickerActive(false);
    selected_recipe_node_kind_ = QStringLiteral("retouch");
    selected_point_color_index_ = -1;
    clearPointColorScopeReference();
    emit selectedGradeNodeChanged();
    emit gradeNodeActionsChanged();
    emit gradeNodeEnabledChanged();
    notifyParametersChanged();
}

void EditController::selectImageCompletionNode() {
    if (!active_ || (interactionLocked() && !imageCompletionActive())
        || selected_recipe_node_kind_ == QStringLiteral("completion")) {
        return;
    }
    finishActiveGesture();
    setPointColorPickerActive(false);
    setRetouchPickerActive(false);
    setWhiteBalancePickerActive(false);
    selected_recipe_node_kind_ = QStringLiteral("completion");
    selected_point_color_index_ = -1;
    clearPointColorScopeReference();
    emit selectedGradeNodeChanged();
    emit gradeNodeActionsChanged();
    emit gradeNodeEnabledChanged();
    notifyParametersChanged();
}

void EditController::selectLiquifyNode() {
    if (!active_ || interactionLocked()
        || selected_recipe_node_kind_ == QStringLiteral("liquify")) {
        return;
    }
    finishActiveGesture();
    setPointColorPickerActive(false);
    setRetouchPickerActive(false);
    setWhiteBalancePickerActive(false);
    selected_recipe_node_kind_ = QStringLiteral("liquify");
    selected_point_color_index_ = -1;
    clearPointColorScopeReference();
    emit selectedGradeNodeChanged();
    emit gradeNodeActionsChanged();
    emit gradeNodeEnabledChanged();
    notifyParametersChanged();
}

void EditController::selectCanvasNode() {
    if (!active_ || interactionLocked() || !grade_stack_.geometry.present
        || selected_recipe_node_kind_ == QStringLiteral("canvas")) {
        return;
    }
    finishActiveGesture();
    setPointColorPickerActive(false);
    setRetouchPickerActive(false);
    setWhiteBalancePickerActive(false);
    selected_recipe_node_kind_ = QStringLiteral("canvas");
    selected_point_color_index_ = -1;
    clearPointColorScopeReference();
    emit selectedGradeNodeChanged();
    emit gradeNodeActionsChanged();
    emit gradeNodeEnabledChanged();
    notifyParametersChanged();
}

void EditController::addGradeNode() {
    if (!canAddGradeNode()) {
        setStatusMessage(grade_node_message(
            QT_TRANSLATE_NOOP("EditController", "An edit can contain at most 16 Grade Nodes")
        ));
        return;
    }
    finishActiveGesture();
    BackendGradeNode grade_node;
    try {
        grade_node =
            backend_->newBasicGradeNode(uniqueGradeNodeLabel(QStringLiteral("Adjustment Node")));
    } catch (const std::exception& error) {
        setStatusMessage(grade_node_message(
            QT_TRANSLATE_NOOP("EditController", "Could not create Grade Node · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return;
    }
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::insertAfterSelection(updated, grade_node, selection)) {
        setStatusMessage(grade_node_message(
            QT_TRANSLATE_NOOP("EditController", "The Grade Node could not be inserted safely")
        ));
        return;
    }
    const bool left_structural = selected_recipe_node_kind_ != QStringLiteral("grade");
    selected_recipe_node_kind_ = QStringLiteral("grade");
    setGradeStack(std::move(updated), grade_node.grade_node_id);
    if (left_structural) {
        emit selectedGradeNodeChanged();
        emit gradeNodeActionsChanged();
        emit gradeNodeEnabledChanged();
        notifyParametersChanged();
        emit toneCurveChanged();
    }
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/add").arg(grade_node.grade_node_id),
        before
    );
    schedulePreview(0);
    setStatusMessage(grade_node_message(
        QT_TRANSLATE_NOOP("EditController", "Added Grade Node · %1"),
        {display_grade_node_label(grade_node.label)}
    ));
}

void EditController::duplicateSelectedGradeNode() {
    const auto* const source = selectedGradeNode();
    if (!canAddGradeNode() || source == nullptr) {
        return;
    }
    finishActiveGesture();
    BackendGradeNode duplicate;
    try {
        duplicate = backend_->newBasicGradeNode(
            uniqueGradeNodeLabel(source->label + QStringLiteral(" Copy"))
        );
    } catch (const std::exception& error) {
        setStatusMessage(grade_node_message(
            QT_TRANSLATE_NOOP("EditController", "Could not duplicate Grade Node · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return;
    }
    duplicate.basic = source->basic;
    duplicate.fine = source->fine;
    duplicate.enabled = source->enabled;
    duplicate.local_mask_components = source->local_mask_components;
    for (auto& component : duplicate.local_mask_components) {
        component.component_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    duplicate.local_mask_invert = source->local_mask_invert;

    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::insertAfterSelection(updated, duplicate, selection)) {
        setStatusMessage(grade_node_message(QT_TRANSLATE_NOOP(
            "EditController",
            "The duplicate Grade Node could not be inserted safely"
        )));
        return;
    }
    setGradeStack(std::move(updated), duplicate.grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/duplicate").arg(duplicate.grade_node_id),
        before
    );
    schedulePreview(0);
    setStatusMessage(grade_node_message(
        QT_TRANSLATE_NOOP("EditController", "Duplicated Grade Node · %1"),
        {display_grade_node_label(duplicate.label)}
    ));
}

void EditController::refreshSharedGradeNodes() {
    try {
        const auto refreshed = backend_->sharedGradeNodes();
        if (refreshed != shared_grade_nodes_) {
            shared_grade_nodes_ = refreshed;
            emit sharedGradeNodesChanged();
            emit gradeNodesChanged();
        }
    } catch (const std::exception& error) {
        setStatusMessage(grade_node_message(
            QT_TRANSLATE_NOOP("EditController", "Could not load shared Grade Nodes · %1"),
            {QString::fromUtf8(error.what())}
        ));
    }
}

void EditController::publishSelectedGradeNode(const QString& label) {
    const auto* const selected = selectedGradeNode();
    const QString normalized_label = label.trimmed();
    if (selected == nullptr || interactionLocked()) {
        return;
    }
    if (normalized_label.isEmpty()) {
        setStatusMessage(grade_node_message(
            QT_TRANSLATE_NOOP("EditController", "Give the shared Grade Node a name")
        ));
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    const bool enabled = selected->enabled;
    BackendSharedGradeNode published;
    try {
        published = backend_->publishSharedGradeNode(normalized_label, *selected);
    } catch (const std::exception& error) {
        setStatusMessage(grade_node_message(
            QT_TRANSLATE_NOOP("EditController", "Could not share Grade Node · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return;
    }
    published.grade_node.enabled = enabled;
    // Sharing publishes only the adjustment graph. The current photo keeps
    // its own spatial placement for that Grade Node.
    published.grade_node.local_mask_components = selected->local_mask_components;
    published.grade_node.local_mask_invert = selected->local_mask_invert;
    BackendGradeStack updated = grade_stack_;
    updated.grade_nodes[selected_grade_node_index_] = published.grade_node;
    setGradeStack(std::move(updated), published.grade_node.grade_node_id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/share/%2")
            .arg(published.grade_node.grade_node_id, published.revision_id),
        before
    );
    refreshSharedGradeNodes();
    schedulePreview(0);
    setStatusMessage(grade_node_message(
        QT_TRANSLATE_NOOP("EditController", "Shared Grade Node · %1"),
        {published.label}
    ));
}

void EditController::insertSharedGradeNode(const QString& layer_id) {
    const auto iterator = std::find_if(
        shared_grade_nodes_.cbegin(),
        shared_grade_nodes_.cend(),
        [&layer_id](const BackendSharedGradeNode& shared) { return shared.layer_id == layer_id; }
    );
    if (iterator == shared_grade_nodes_.cend() || !active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    const auto existing = std::find_if(
        updated.grade_nodes.begin(),
        updated.grade_nodes.end(),
        [&iterator](const BackendGradeNode& node) {
            return node.shared_layer_id == iterator->layer_id
                   || node.grade_node_id == iterator->grade_node.grade_node_id;
        }
    );
    BackendGradeNode inserted = iterator->grade_node;
    if (existing != updated.grade_nodes.end()) {
        inserted.enabled = existing->enabled;
        inserted.local_mask_components = existing->local_mask_components;
        inserted.local_mask_invert = existing->local_mask_invert;
        *existing = inserted;
    } else {
        if (!canAddGradeNode()) {
            setStatusMessage(grade_node_message(
                QT_TRANSLATE_NOOP("EditController", "An edit can contain at most 16 Grade Nodes")
            ));
            return;
        }
        int selection = selected_grade_node_index_;
        if (!GradeNodeStack::insertAfterSelection(updated, inserted, selection)) {
            setStatusMessage(grade_node_message(QT_TRANSLATE_NOOP(
                "EditController",
                "The shared Grade Node could not be inserted safely"
            )));
            return;
        }
    }
    const bool left_structural = selected_recipe_node_kind_ != QStringLiteral("grade");
    selected_recipe_node_kind_ = QStringLiteral("grade");
    setGradeStack(std::move(updated), inserted.grade_node_id);
    if (left_structural) {
        emit selectedGradeNodeChanged();
        emit gradeNodeActionsChanged();
        emit gradeNodeEnabledChanged();
        notifyParametersChanged();
        emit toneCurveChanged();
    }
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/shared/%2")
            .arg(inserted.grade_node_id, iterator->revision_id),
        before
    );
    schedulePreview(0);
    setStatusMessage(grade_node_message(
        QT_TRANSLATE_NOOP("EditController", "Applied shared Grade Node · %1"),
        {iterator->label}
    ));
}

void EditController::deleteSelectedGradeNode() {
    const auto* const selected = selectedGradeNode();
    if (!canDeleteGradeNode() || selected == nullptr) {
        return;
    }
    finishActiveGesture();
    const QString deleted_id = selected->grade_node_id;
    const QString deleted_label = selected->label;
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::deleteSelection(updated, selection)) {
        return;
    }
    const QString next_id = updated.grade_nodes.at(selection).grade_node_id;
    setGradeStack(std::move(updated), next_id);
    recordWorkingTransition(QStringLiteral("grade_node/%1/delete").arg(deleted_id), before);
    schedulePreview(0);
    setStatusMessage(grade_node_message(
        QT_TRANSLATE_NOOP("EditController", "Deleted Grade Node · %1"),
        {display_grade_node_label(deleted_label)}
    ));
}

void EditController::moveSelectedGradeNode(const int destination_index) {
    const auto* const selected = selectedGradeNode();
    if (!active_ || interactionLocked() || selected == nullptr) {
        return;
    }
    finishActiveGesture();
    const QString moved_id = selected->grade_node_id;
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack updated = grade_stack_;
    int selection = selected_grade_node_index_;
    if (!GradeNodeStack::moveSelection(updated, selection, destination_index)) {
        return;
    }
    setGradeStack(std::move(updated), moved_id);
    recordWorkingTransition(QStringLiteral("grade_node/%1/move").arg(moved_id), before);
    schedulePreview(0);
    setStatusMessage(
        grade_node_message(QT_TRANSLATE_NOOP("EditController", "Reordered Grade Node"))
    );
}

void EditController::resetSelectedGradeNode() {
    const auto* const grade_node = selectedGradeNode();
    if (!active_ || interactionLocked() || grade_node == nullptr) {
        return;
    }
    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    BackendGradeStack reset = grade_stack_;
    if (!GradeNodeStack::resetSelection(reset, selected_grade_node_index_)) {
        return;
    }
    const QString grade_node_id = grade_node->grade_node_id;
    setGradeStack(std::move(reset), grade_node_id);
    recordWorkingTransition(QStringLiteral("grade_node/%1/reset").arg(grade_node_id), before);
    schedulePreview(0);
    setStatusMessage(
        grade_node_message(QT_TRANSLATE_NOOP("EditController", "Reset the selected Grade Node"))
    );
}

void EditController::resetAllGradeNodes() {
    if (!active_ || interactionLocked()) {
        return;
    }
    finishActiveGesture();

    BackendGradeNode neutral;
    try {
        neutral = backend_->newBasicGradeNode(QStringLiteral("Adjustment Node"));
    } catch (const std::exception& error) {
        setStatusMessage(grade_node_message(
            QT_TRANSLATE_NOOP("EditController", "Could not clear Grade Nodes · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return;
    }

    const BackendGradeStack before = grade_stack_;
    BackendGradeStack reset = grade_stack_;
    reset.grade_nodes = {neutral};
    setGradeStack(std::move(reset), neutral.grade_node_id);
    recordWorkingTransition(QStringLiteral("grade_nodes/reset_all"), before);
    schedulePreview(0);
    setStatusMessage(
        grade_node_message(QT_TRANSLATE_NOOP("EditController", "Cleared all Grade Nodes"))
    );
}

const BackendGradeNode* EditController::selectedGradeNode() const noexcept {
    if (selected_recipe_node_kind_ != QStringLiteral("grade")) {
        return nullptr;
    }
    const int count = static_cast<int>(grade_stack_.grade_nodes.size());
    if (selected_grade_node_index_ < 0 || selected_grade_node_index_ >= count) {
        return nullptr;
    }
    return &grade_stack_.grade_nodes.at(selected_grade_node_index_);
}

QString EditController::uniqueGradeNodeLabel(const QString& base) const {
    const QString clean_base =
        base.trimmed().isEmpty() ? QStringLiteral("Adjustment Node") : base.trimmed();
    const auto exists = [this](const QString& candidate) {
        return std::any_of(
            grade_stack_.grade_nodes.cbegin(),
            grade_stack_.grade_nodes.cend(),
            [&candidate](const BackendGradeNode& grade_node) {
                return grade_node.label == candidate;
            }
        );
    };
    if (!exists(clean_base)) {
        return clean_base;
    }
    for (int suffix = 2; suffix <= GradeNodeStack::maximum_grade_node_count + 1; ++suffix) {
        const QString candidate = QStringLiteral("%1 %2").arg(clean_base).arg(suffix);
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return clean_base + QStringLiteral(" Copy");
}
