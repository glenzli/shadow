#include "shared_grade_link_operations.hpp"

#include "desktop_backend.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {
BackendSharedGradeLinkStatus transfer_status(AdjustmentTransferIssue issue) {
    switch (issue) {
    case AdjustmentTransferIssue::None:
        return BackendSharedGradeLinkStatus::Ready;
    case AdjustmentTransferIssue::Changed:
        return BackendSharedGradeLinkStatus::HeadChanged;
    case AdjustmentTransferIssue::NodeLimit:
        return BackendSharedGradeLinkStatus::NodeLimit;
    case AdjustmentTransferIssue::Unavailable:
        return BackendSharedGradeLinkStatus::Unavailable;
    default:
        return BackendSharedGradeLinkStatus::Failed;
    }
}

bool matches_edit_identity(
    const BackendPhotoEditState& state,
    const BackendSharedGradeLinkedTarget& target
) {
    return state.photo_id == target.photo_id && state.source_path == target.source_path
           && state.active_variant_id == target.variant_id
           && state.base_commit_id == target.working_commit_id;
}
} // namespace

SharedGradeLinkOperations sharedGradeLinkOperations(DesktopBackend& backend) {
    const auto transfer = adjustmentTransferOperations(backend);
    return {
        .nodes = [&backend] { return backend.sharedGradeNodes(); },
        .publish =
            [&backend](const SharedGradeLinkSource& source, const QString& label) {
                return backend.publishSharedGradeNodeFromPhoto(
                    source.state.photo_id,
                    source.state.source_path,
                    source.state.base_commit_id,
                    source.expected_working_commit_id,
                    source.state.active_variant_id,
                    source.representation_id,
                    source.state.grade_stack,
                    source.grade_node_id,
                    label
                );
            },
        .linked_targets =
            [&backend](
                const QString& layer,
                const QString& revision,
                const QString& after,
                std::uint32_t limit
            ) { return backend.sharedGradeLinkedTargets(layer, revision, after, limit); },
        .target =
            [&backend](const QString& layer, const QString& photo, const QString& path) {
                return backend.sharedGradeLinkedTarget(layer, photo, path);
            },
        .copy = transfer.copy,
        .prepare =
            [&backend, transfer](
                const BackendSharedGradeNode& shared,
                const AdjustmentTransferClipboard& clipboard,
                const SharedGradeLinkTarget& target,
                const std::shared_ptr<AdjustmentTransferCancellation>& cancellation
            ) {
                SharedGradeLinkPrepared result;
                try {
                    auto subscription = target.subscription;
                    if (subscription && target.refresh_subscription) {
                        const auto intent = *subscription;
                        const auto current = backend.sharedGradeLinkedTarget(
                            shared.layer_id,
                            intent.photo_id,
                            intent.source_path
                        );
                        BackendSharedGradeLinkedTarget refreshed;
                        result.status = refreshSharedGradeSubscriptionIntent(
                            intent,
                            current,
                            shared.revision_id,
                            refreshed
                        );
                        if (result.status != BackendSharedGradeLinkStatus::Ready)
                            return result;
                        subscription = std::move(refreshed);
                    }
                    if (!subscription) {
                        const auto current = backend.sharedGradeLinkedTarget(
                            shared.layer_id,
                            target.photo.photo_id,
                            target.photo.source_path
                        );
                        result.frozen_before = target.frozen_before
                                                   ? target.frozen_before
                                                   : sharedGradeLinkTargetIdentity(current);
                        result.status =
                            validateSharedGradeLinkTargetIdentity(*result.frozen_before, current);
                        if (result.status != BackendSharedGradeLinkStatus::Ready)
                            return result;
                        result.before = transfer.load(target.photo);
                        if (!matches_edit_identity(result.before, current)) {
                            result.status = BackendSharedGradeLinkStatus::HeadChanged;
                            return result;
                        }
                        QVector<QString> existing_revisions;
                        for (const auto& node : result.before.grade_stack.grade_nodes) {
                            if (node.shared_layer_id != shared.layer_id)
                                continue;
                            if (!node.shared_follow_head) {
                                result.status = BackendSharedGradeLinkStatus::HeadChanged;
                                result.message = {
                                    "SharedGradeLinkController",
                                    QT_TRANSLATE_NOOP(
                                        "SharedGradeLinkController",
                                        "This photo has a fixed shared revision. Follow updates or "
                                        "unlink it first."
                                    )
                                };
                                return result;
                            }
                            existing_revisions.push_back(node.shared_revision_id);
                        }
                        if (!existing_revisions.isEmpty()) {
                            if (std::all_of(
                                    existing_revisions.cbegin(),
                                    existing_revisions.cend(),
                                    [&](const auto& revision) {
                                        return revision == shared.revision_id;
                                    }
                                )) {
                                // Linking an already-current instance never replaces
                                // local parameters, masks or strength.
                                result.status = BackendSharedGradeLinkStatus::Unchanged;
                                return result;
                            }
                            subscription = current;
                        }
                    }
                    if (subscription) {
                        result.subscription = *subscription;
                        const auto prepared = backend.prepareSharedGradeLinkUpdate(
                            shared.layer_id,
                            shared.revision_id,
                            *subscription
                        );
                        result.status = prepared.status;
                        result.before = prepared.state;
                        result.detail = prepared.detail;
                    } else {
                        result.new_link = true;
                        if (clipboard.requires_inference && !target.subject_masks_allowed) {
                            result.status = BackendSharedGradeLinkStatus::Unavailable;
                            result.message = {
                                "SharedGradeLinkController",
                                QT_TRANSLATE_NOOP(
                                    "SharedGradeLinkController",
                                    "Smart masks are unavailable. Check AI settings and retry."
                                )
                            };
                            return result;
                        }
                        const auto before = result.before;
                        result.transfer = transfer.prepare(clipboard, target.photo, cancellation);
                        result.status = transfer_status(result.transfer.failure.issue);
                        result.detail = result.transfer.failure.detail;
                        if (!result.transfer.before.photo_id.isEmpty())
                            result.before = result.transfer.before;
                        if (result.status == BackendSharedGradeLinkStatus::Ready) {
                            const auto current = backend.sharedGradeLinkedTarget(
                                shared.layer_id,
                                target.photo.photo_id,
                                target.photo.source_path
                            );
                            result.status = validateSharedGradeLinkTargetIdentity(
                                *result.frozen_before,
                                current
                            );
                            if (result.status == BackendSharedGradeLinkStatus::Ready
                                && (!matches_edit_identity(result.before, current)
                                    || result.before.base_commit_id != before.base_commit_id
                                    || result.before.active_variant_id != before.active_variant_id))
                                result.status = BackendSharedGradeLinkStatus::HeadChanged;
                        }
                    }
                } catch (const std::exception& error) {
                    // Keep any exact identity acquired before a later failure.
                    result.status = BackendSharedGradeLinkStatus::Failed;
                    result.detail = QString::fromUtf8(error.what());
                }
                return result;
            },
        .write =
            [&backend](
                const BackendSharedGradeNode& shared,
                const SharedGradeLinkPrepared& prepared
            ) {
                if (prepared.new_link) {
                    return BackendSharedGradeLinkUpdateResult{
                        BackendSharedGradeLinkStatus::Updated,
                        backend.commitSharedGradeNodeLink(
                            prepared.transfer,
                            shared.layer_id,
                            shared.revision_id
                        ),
                        {}
                    };
                }
                return backend.commitSharedGradeLinkUpdate(
                    shared.layer_id,
                    shared.revision_id,
                    prepared.subscription
                );
            },
        .preview = transfer.preview,
    };
}
