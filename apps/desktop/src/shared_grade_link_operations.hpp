#pragma once

#include "adjustment_transfer_operations.hpp"
#include "backend/shared_grade_link_types.hpp"
#include "localized_ui_message.hpp"

#include <optional>
#include <utility>

class DesktopBackend;

// Native display/admission hint only. Rust compares canonical revision graphs
// for every durable write; resource location and presentation title are not
// authored LUT semantics.
[[nodiscard]] inline bool
sharedGradeParametersDiffer(const BackendGradeNode& left, const BackendGradeNode& right) {
    auto left_fine = left.fine;
    auto right_fine = right.fine;
    left_fine.lut_managed_path.clear();
    right_fine.lut_managed_path.clear();
    left_fine.lut_title.clear();
    right_fine.lut_title.clear();
    return left.basic != right.basic || left_fine != right_fine;
}

// A retry may refresh the working head only within the original subscription
// intent. It never creates an instance or redirects a Variant/source.
[[nodiscard]] inline BackendSharedGradeLinkStatus refreshSharedGradeSubscriptionIntent(
    const BackendSharedGradeLinkedTarget& intent,
    const BackendSharedGradeLinkedTarget& current,
    const QString& published_revision,
    BackendSharedGradeLinkedTarget& refreshed
) {
    if (current.photo_id != intent.photo_id || current.source_path != intent.source_path
        || current.variant_id != intent.variant_id)
        return BackendSharedGradeLinkStatus::HeadChanged;
    if (current.representation_id != intent.representation_id
        || current.source_byte_len != intent.source_byte_len
        || current.has_source_modified_at_ms != intent.has_source_modified_at_ms
        || current.source_modified_at_ms != intent.source_modified_at_ms)
        return BackendSharedGradeLinkStatus::Unavailable;
    if (intent.grade_node_ids.isEmpty()
        || intent.grade_node_ids.size() != intent.shared_revision_ids.size()
        || current.grade_node_ids.size() != current.shared_revision_ids.size())
        return BackendSharedGradeLinkStatus::HeadChanged;
    QVector<QString> revisions;
    for (qsizetype index = 0; index < intent.grade_node_ids.size(); ++index) {
        const auto found = current.grade_node_ids.indexOf(intent.grade_node_ids[index]);
        if (found < 0
            || (current.shared_revision_ids[found] != intent.shared_revision_ids[index]
                && current.shared_revision_ids[found] != published_revision))
            return BackendSharedGradeLinkStatus::HeadChanged;
        revisions.push_back(current.shared_revision_ids[found]);
    }
    refreshed = current;
    refreshed.grade_node_ids = intent.grade_node_ids;
    refreshed.shared_revision_ids = std::move(revisions);
    return BackendSharedGradeLinkStatus::Ready;
}

// Publication captures the visible draft independently of its durable CAS head.
// No image bytes are retained by this workflow.
struct SharedGradeLinkSource final {
    BackendPhotoEditState state;
    QString expected_working_commit_id;
    QString representation_id;
    QString grade_node_id;
    QString name;
    QString shared_name;
};

// A selected photo may need a new instance. Its first exact metadata receipt
// freezes the target independently of any following-instance subscription.
struct SharedGradeLinkTargetIdentity final {
    QString photo_id, source_path, representation_id, variant_id;
    std::uint64_t source_byte_len = 0;
    bool has_source_modified_at_ms = false;
    std::int64_t source_modified_at_ms = 0;
};

[[nodiscard]] inline SharedGradeLinkTargetIdentity
sharedGradeLinkTargetIdentity(const BackendSharedGradeLinkedTarget& target) {
    return {
        target.photo_id,
        target.source_path,
        target.representation_id,
        target.variant_id,
        target.source_byte_len,
        target.has_source_modified_at_ms,
        target.source_modified_at_ms
    };
}

[[nodiscard]] inline BackendSharedGradeLinkStatus validateSharedGradeLinkTargetIdentity(
    const SharedGradeLinkTargetIdentity& intent,
    const BackendSharedGradeLinkedTarget& current
) {
    if (current.photo_id != intent.photo_id || current.source_path != intent.source_path
        || current.variant_id != intent.variant_id)
        return BackendSharedGradeLinkStatus::HeadChanged;
    if (current.source_path.isEmpty() || current.representation_id.isEmpty()
        || current.representation_id != intent.representation_id
        || current.source_byte_len != intent.source_byte_len
        || current.has_source_modified_at_ms != intent.has_source_modified_at_ms
        || current.source_modified_at_ms != intent.source_modified_at_ms)
        return BackendSharedGradeLinkStatus::Unavailable;
    return BackendSharedGradeLinkStatus::Ready;
}

struct SharedGradeLinkTarget final {
    BackendBatchPhotoTarget photo;
    QString title;
    std::optional<BackendSharedGradeLinkedTarget> subscription;
    bool refresh_subscription = false;
    bool subject_masks_allowed = true;
    std::optional<SharedGradeLinkTargetIdentity> frozen_before = std::nullopt;
};

struct SharedGradeLinkPrepared final {
    BackendPhotoEditState before;
    AdjustmentTransferPrepared transfer;
    BackendSharedGradeLinkedTarget subscription;
    BackendSharedGradeLinkStatus status = BackendSharedGradeLinkStatus::Failed;
    QString detail;
    LocalizedUiMessage message;
    bool new_link = false;
    std::optional<SharedGradeLinkTargetIdentity> frozen_before = std::nullopt;
};

// Every callback executes on a worker. Source publication and target writes
// acquire the editor's short metadata reservation through the controller.
struct SharedGradeLinkOperations final {
    std::function<QVector<BackendSharedGradeNode>()> nodes;
    std::function<
        BackendSharedGradePublicationReceipt(const SharedGradeLinkSource&, const QString&)>
        publish;
    std::function<BackendSharedGradeLinkedTargetsPage(
        const QString&,
        const QString&,
        const QString&,
        std::uint32_t
    )>
        linked_targets;
    std::function<BackendSharedGradeLinkedTarget(const QString&, const QString&, const QString&)>
        target;
    std::function<AdjustmentTransferClipboard(const AdjustmentTransferSource&)> copy;
    std::function<SharedGradeLinkPrepared(
        const BackendSharedGradeNode&,
        const AdjustmentTransferClipboard&,
        const SharedGradeLinkTarget&,
        const std::shared_ptr<AdjustmentTransferCancellation>&
    )>
        prepare;
    std::function<BackendSharedGradeLinkUpdateResult(
        const BackendSharedGradeNode&,
        const SharedGradeLinkPrepared&
    )>
        write;
    std::function<
        bool(const BackendPhotoEditState&, const std::shared_ptr<AdjustmentTransferCancellation>&)>
        preview;
};

[[nodiscard]] SharedGradeLinkOperations sharedGradeLinkOperations(DesktopBackend& backend);
