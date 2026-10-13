#include "adjustment_transfer_controller.hpp"
#include "edit_controller.hpp"
#include "edit_paint_controller.hpp"

#include <stdexcept>

AdjustmentTransferEditor adjustmentTransferEditor(EditController& editor) {
    return {
        [&editor](bool selected) { return editor.captureAdjustmentTransfer(selected); },
        [&editor] {
            return editor.active() ? BackendBatchPhotoTarget{editor.photoId(), editor.sourcePath()}
                                   : BackendBatchPhotoTarget{};
        },
        [&editor](const auto& before) { return editor.reserveAdjustmentTransfer(before); },
        [&editor](const auto& saved, const auto& before) {
            editor.acceptAdjustmentTransfer(saved, before);
        },
        [&editor] { editor.releaseAdjustmentTransfer(); },
        [&editor] { return editor.subjectMaskExecutionAllowed(); },
        [&editor] { return editor.busy(); }
    };
}

AdjustmentTransferSource EditController::captureAdjustmentTransfer(bool selected_only) {
    if (!active_ || interactionLocked())
        throw std::runtime_error(
            tr("Open an editable photo before copying adjustments.").toStdString()
        );
    finishActiveGesture();
    auto stack = grade_stack_;
    if (selected_only) {
        const auto* selected = selectedGradeNode();
        if (!selected || selected_recipe_node_kind_ != QStringLiteral("grade"))
            throw std::runtime_error(tr("Select an adjustment node to copy.").toStdString());
        stack.grade_nodes = {*selected};
    }
    if (stack.grade_nodes.isEmpty())
        throw std::runtime_error(tr("This photo has no adjustment nodes to copy.").toStdString());
    return {photo_id_, source_path_, base_commit_id_, title_, std::move(stack), false};
}

AdjustmentTransferAdmission
EditController::reserveAdjustmentTransfer(const BackendPhotoEditState& before) {
    if (stateTaskRunning() || interactionLocked())
        return AdjustmentTransferAdmission::Busy;
    if (active_ && photo_id_ == before.photo_id) {
        if (source_path_ != before.source_path || active_variant_id_ != before.active_variant_id
            || dirty_ || version_draft_ || durable_working_commit_id_ != before.base_commit_id
            || grade_stack_ != before.grade_stack)
            return AdjustmentTransferAdmission::Changed;
        if (!active_parameter_gestures_.isEmpty() || liquify_live_before_.has_value()
            || (paint_controller_ && paint_controller_->strokeActive()))
            return AdjustmentTransferAdmission::Busy;
    }
    // Only the final asynchronous metadata commit reserves this state. During
    // mask preparation browsing and ordinary editing remain available.
    adjustment_transfer_affects_editor_ = active_ && photo_id_ == before.photo_id;
    if (adjustment_transfer_affects_editor_)
        finishActiveGesture();
    adjustment_transfer_reserved_ = true;
    persistence_state_.stopAutosaveDebounce();
    emit stateBusyChanged();
    emit busyChanged();
    emit historyChanged();
    return AdjustmentTransferAdmission::Ready;
}

void EditController::acceptAdjustmentTransfer(
    const BackendPhotoEditState& saved,
    const BackendPhotoEditState& before
) {
    if (!active_ || photo_id_ != saved.photo_id || source_path_ != saved.source_path)
        return;
    const auto selected = saved.grade_stack.grade_nodes.isEmpty()
                              ? QString{}
                              : saved.grade_stack.grade_nodes.constLast().grade_node_id;
    applyCommittedGradeStack(
        saved.grade_stack,
        saved.base_commit_id,
        before.grade_stack,
        selected,
        QStringLiteral("adjustments/transfer")
    );
    setPhotoVariants(saved.active_variant_id, saved.variants);
    versions_.replace(saved.versions);
    emit editBaseCommitIdChanged();
}

void EditController::releaseAdjustmentTransfer() {
    adjustment_transfer_reserved_ = false;
    adjustment_transfer_affects_editor_ = false;
    emit stateBusyChanged();
    emit busyChanged();
    emit historyChanged();
    if (persistence_state_.closeAfterAutosave()) {
        persistence_state_.cancelApplicationClose();
        if (prepareToClose())
            emit closeReady();
        return;
    }
    if (openPendingPhoto())
        return;
    if (persistence_state_.closePhotoAfterAutosave()) {
        closePhoto();
        return;
    }
    if (dirty_ && persistence_state_.autosaveRequested())
        scheduleAutosave();
}
