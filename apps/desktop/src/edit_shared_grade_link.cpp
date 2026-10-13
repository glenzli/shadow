#include "shared_grade_link_controller.hpp"

#include "edit_controller.hpp"
#include "edit_paint_controller.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {
LocalizedUiMessage shared_link_message(const char* source) {
    return {"EditController", source, {}};
}
} // namespace

SharedGradeLinkEditor sharedGradeLinkEditor(EditController& editor) {
    return {
        [&editor] { return editor.captureSharedGradePublication(); },
        [&editor] {
            return editor.active() ? BackendBatchPhotoTarget{editor.photoId(), editor.sourcePath()}
                                   : BackendBatchPhotoTarget{};
        },
        [&editor](const auto& source) { return editor.reserveSharedGradePublication(source); },
        [&editor](const auto& before) { return editor.reserveAdjustmentTransfer(before); },
        [&editor](const auto& saved, const auto& before, const auto& selected, const auto& key) {
            editor.acceptSharedGradeLink(saved, before, selected, key);
        },
        [&editor] { editor.releaseAdjustmentTransfer(); },
        [&editor] { return editor.subjectMaskExecutionAllowed(); },
        [&editor] { return editor.busy(); },
    };
}

SharedGradeLinkSource EditController::captureSharedGradePublication() {
    if (!active_ || interactionLocked())
        throw std::runtime_error(
            tr("Open an editable photo before sharing adjustments.").toStdString()
        );
    finishActiveGesture();
    const auto* selected = selectedGradeNode();
    if (!selected)
        throw std::runtime_error(tr("Select an adjustment node first.").toStdString());
    SharedGradeLinkSource source;
    source.state.photo_id = photo_id_;
    source.state.source_path = source_path_;
    source.state.base_commit_id = base_commit_id_;
    source.state.active_variant_id = active_variant_id_;
    source.state.grade_stack = grade_stack_;
    source.state.is_version_draft = version_draft_;
    source.expected_working_commit_id = durable_working_commit_id_;
    source.representation_id = representation_id_;
    source.grade_node_id = selected->grade_node_id;
    source.name = title_;
    const auto shared = std::find_if(
        shared_grade_nodes_.cbegin(),
        shared_grade_nodes_.cend(),
        [&](const auto& node) { return node.layer_id == selected->shared_layer_id; }
    );
    if (shared != shared_grade_nodes_.cend())
        source.shared_name = shared->label;
    return source;
}

AdjustmentTransferAdmission
EditController::reserveSharedGradePublication(const SharedGradeLinkSource& source) {
    if (stateTaskRunning() || interactionLocked())
        return AdjustmentTransferAdmission::Busy;
    if (!active_ || photo_id_ != source.state.photo_id || source_path_ != source.state.source_path
        || representation_id_ != source.representation_id
        || active_variant_id_ != source.state.active_variant_id
        || base_commit_id_ != source.state.base_commit_id
        || durable_working_commit_id_ != source.expected_working_commit_id
        || version_draft_ != source.state.is_version_draft
        || grade_stack_ != source.state.grade_stack)
        return AdjustmentTransferAdmission::Changed;
    if (!active_parameter_gestures_.isEmpty() || liquify_live_before_.has_value()
        || (paint_controller_ && paint_controller_->strokeActive()))
        return AdjustmentTransferAdmission::Busy;
    // This is a frozen visible draft, not an assumption that autosave already
    // finished. The publisher commits the draft and self-link in one CAS.
    adjustment_transfer_affects_editor_ = true;
    adjustment_transfer_reserved_ = true;
    persistence_state_.stopAutosaveDebounce();
    emit stateBusyChanged();
    emit busyChanged();
    emit historyChanged();
    return AdjustmentTransferAdmission::Ready;
}

void EditController::acceptSharedGradeLink(
    const BackendPhotoEditState& saved,
    const BackendPhotoEditState& before,
    const QString& target_grade_node_id,
    const QString& transaction_key
) {
    if (!active_ || photo_id_ != saved.photo_id || source_path_ != saved.source_path)
        return;
    const QString preserved_selection =
        target_grade_node_id.isEmpty() ? selectedGradeNodeId() : target_grade_node_id;
    applyCommittedGradeStack(
        saved.grade_stack,
        saved.base_commit_id,
        before.grade_stack,
        preserved_selection,
        transaction_key
    );
    setPhotoVariants(saved.active_variant_id, saved.variants);
    versions_.replace(saved.versions);
    emit editBaseCommitIdChanged();
    emit gradeNodesChanged();
    notifyParametersChanged();
}

QVariantMap EditController::sharedGradeNodeState(const BackendGradeNode& node) const {
    const bool shared = !node.shared_layer_id.isEmpty() && !node.shared_revision_id.isEmpty();
    const auto head = std::find_if(
        shared_grade_nodes_.cbegin(),
        shared_grade_nodes_.cend(),
        [&](const auto& candidate) { return candidate.layer_id == node.shared_layer_id; }
    );
    const bool can_compare_pinned = shared && head != shared_grade_nodes_.cend()
                                    && head->revision_id == node.shared_revision_id;
    bool local_changed =
        shared
        && (can_compare_pinned ? sharedGradeParametersDiffer(node, head->grade_node)
                               : node.shared_locally_modified);
    const auto saved = std::find_if(
        committed_grade_stack_.grade_nodes.cbegin(),
        committed_grade_stack_.grade_nodes.cend(),
        [&](const auto& candidate) {
            return candidate.grade_node_id == node.grade_node_id
                   && candidate.shared_revision_id == node.shared_revision_id;
        }
    );
    local_changed =
        local_changed
        || (!can_compare_pinned && shared && saved != committed_grade_stack_.grade_nodes.cend()
            && sharedGradeParametersDiffer(node, *saved));
    return {
        {QStringLiteral("sharedFollowHead"), shared && node.shared_follow_head},
        {QStringLiteral("sharedLocallyModified"), local_changed},
        {QStringLiteral("sharedUpdateAvailable"),
         shared && head != shared_grade_nodes_.cend()
             && head->revision_id != node.shared_revision_id},
        {QStringLiteral("sharedRevisionNumber"), static_cast<int>(node.shared_revision_number)},
        {QStringLiteral("sharedHeadRevisionNumber"),
         head == shared_grade_nodes_.cend() ? 0 : static_cast<int>(head->revision_number)},
    };
}
QVariantMap EditController::selectedSharedGradeNodeState() const {
    const auto* node = selectedGradeNode();
    return node ? sharedGradeNodeState(*node) : QVariantMap{};
}

void EditController::unlinkSelectedGradeNode() {
    const auto* selected = selectedGradeNode();
    if (!active_ || interactionLocked() || !selected || selected->shared_layer_id.isEmpty())
        return;
    finishActiveGesture();
    const auto before = grade_stack_;
    auto updated = grade_stack_;
    auto& node = updated.grade_nodes[selected_grade_node_index_];
    const auto id = node.grade_node_id;
    node.shared_layer_id.clear();
    node.shared_revision_id.clear();
    node.shared_follow_head = false;
    node.shared_revision_number = 0;
    node.shared_locally_modified = false;
    setGradeStack(std::move(updated), id);
    recordWorkingTransition(QStringLiteral("grade_node/%1/shared_unlink").arg(id), before);
    emit gradeNodesChanged();
    notifyParametersChanged();
    // The materialized graph, mask, strength and bypass are byte-for-byte
    // unchanged. Metadata-only unlinking adds no pixel recomputation or transfer.
    setStatusMessage(shared_link_message(
        QT_TRANSLATE_NOOP("EditController", "Shared node unlinked · current adjustments kept")
    ));
}
void EditController::setSelectedGradeNodeSharedFollowHead(bool follow) {
    const auto* selected = selectedGradeNode();
    if (!active_ || interactionLocked() || !selected || selected->shared_layer_id.isEmpty()
        || selected->shared_follow_head == follow)
        return;
    finishActiveGesture();
    const auto before = grade_stack_;
    auto updated = grade_stack_;
    auto& node = updated.grade_nodes[selected_grade_node_index_];
    const auto id = node.grade_node_id;
    node.shared_follow_head = follow;
    setGradeStack(std::move(updated), id);
    recordWorkingTransition(
        QStringLiteral("grade_node/%1/shared_follow/%2").arg(id).arg(follow),
        before
    );
    emit gradeNodesChanged();
    notifyParametersChanged();
    setStatusMessage(shared_link_message(
        follow ? QT_TRANSLATE_NOOP(
                     "EditController",
                     "Following shared updates · update to the latest version after saving"
                 )
               : QT_TRANSLATE_NOOP(
                     "EditController",
                     "Shared revision fixed · current adjustments kept"
                 )
    ));
}
