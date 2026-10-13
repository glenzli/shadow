#include "adjustment_transfer_operations.hpp"
#include "backend/desktop_backend_private.hpp"
#include "backend/edit_settings_projection.hpp"
#include "backend/rust_qt_projection.hpp"

#include <QCoreApplication>
#include <stdexcept>
#include <utility>

namespace {
using desktop_backend_projection::checked_qt_vector_size;
using desktop_backend_projection::edit_state;
using desktop_backend_projection::ffi_grade_stack;
using desktop_backend_projection::qstring;
using desktop_backend_projection::shared_grade_node;

BackendSharedGradeLinkedTarget
project_target(const shadow::desktop::FfiSharedGradeLinkedTarget& source) {
    BackendSharedGradeLinkedTarget target;
    target.photo_id = qstring(source.photo_id);
    target.source_path = qstring(source.source_path);
    target.representation_id = qstring(source.representation_id);
    target.source_byte_len = source.source_byte_len;
    target.has_source_modified_at_ms = source.has_source_modified_at_ms;
    target.source_modified_at_ms = source.source_modified_at_ms;
    target.variant_id = qstring(source.variant_id);
    target.working_commit_id = qstring(source.working_commit_id);
    for (const auto& instance : source.instances) {
        target.grade_node_ids.push_back(qstring(instance.grade_node_id));
        target.shared_revision_ids.push_back(qstring(instance.shared_revision_id));
    }
    return target;
}

shadow::desktop::FfiSharedGradeLinkedTarget
ffi_target(const BackendSharedGradeLinkedTarget& source) {
    if (source.grade_node_ids.isEmpty() || source.grade_node_ids.size() > 16
        || source.grade_node_ids.size() != source.shared_revision_ids.size())
        throw std::invalid_argument(
            QCoreApplication::translate(
                "DesktopBackendSharedGradeLink",
                "Shared node instance identities are incomplete"
            )
                .toStdString()
        );
    shadow::desktop::FfiSharedGradeLinkedTarget result;
    result.photo_id = source.photo_id.toStdString();
    result.source_path = source.source_path.toStdString();
    result.representation_id = source.representation_id.toStdString();
    result.source_byte_len = source.source_byte_len;
    result.has_source_modified_at_ms = source.has_source_modified_at_ms;
    result.source_modified_at_ms = source.source_modified_at_ms;
    result.variant_id = source.variant_id.toStdString();
    result.working_commit_id = source.working_commit_id.toStdString();
    result.instances.reserve(static_cast<std::size_t>(source.grade_node_ids.size()));
    for (qsizetype index = 0; index < source.grade_node_ids.size(); ++index) {
        shadow::desktop::FfiSharedGradeLinkedInstance instance;
        instance.grade_node_id = source.grade_node_ids[index].toStdString();
        instance.shared_revision_id = source.shared_revision_ids[index].toStdString();
        result.instances.push_back(std::move(instance));
    }
    return result;
}

BackendSharedGradeLinkUpdateResult
project_result(const shadow::desktop::FfiSharedGradeLinkUpdateReceipt& receipt, bool preparing) {
    using Status = shadow::desktop::FfiSharedGradeUpdateStatus;
    BackendSharedGradeLinkUpdateResult result;
    switch (receipt.status) {
    case Status::Unchanged:
        result.status = BackendSharedGradeLinkStatus::Unchanged;
        break;
    case Status::Updated:
        result.status =
            preparing ? BackendSharedGradeLinkStatus::Ready : BackendSharedGradeLinkStatus::Updated;
        break;
    case Status::LocalChanged:
        result.status = BackendSharedGradeLinkStatus::LocalChanged;
        break;
    case Status::HeadChanged:
        result.status = BackendSharedGradeLinkStatus::HeadChanged;
        break;
    case Status::Unavailable:
        result.status = BackendSharedGradeLinkStatus::Unavailable;
        break;
    default:
        throw std::invalid_argument(
            QCoreApplication::translate(
                "DesktopBackendSharedGradeLink",
                "Unknown shared node update status"
            )
                .toStdString()
        );
    }
    result.detail = qstring(receipt.diagnostic);
    if (receipt.has_state)
        result.state = edit_state(receipt.state);
    else if (
        result.status == BackendSharedGradeLinkStatus::Ready
        || result.status == BackendSharedGradeLinkStatus::Updated
    )
        throw std::invalid_argument(
            QCoreApplication::translate(
                "DesktopBackendSharedGradeLink",
                "Shared node update has no saved state receipt"
            )
                .toStdString()
        );
    return result;
}
} // namespace

BackendSharedGradePublicationReceipt DesktopBackend::publishSharedGradeNodeFromPhoto(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const QString& expected_variant_id,
    const QString& expected_representation_id,
    const BackendGradeStack& settings,
    const QString& grade_node_id,
    const QString& label
) const {
    const auto receipt = impl_->session->publish_shared_grade_node_from_photo(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        expected_working_commit_id.toStdString(),
        expected_variant_id.toStdString(),
        expected_representation_id.toStdString(),
        ffi_grade_stack(settings),
        grade_node_id.toStdString(),
        label.toStdString()
    );
    return {shared_grade_node(receipt.shared_node), edit_state(receipt.state)};
}

BackendSharedGradeLinkedTargetsPage DesktopBackend::sharedGradeLinkedTargets(
    const QString& layer_id,
    const QString& revision_id,
    const QString& after_photo_id,
    std::uint32_t limit
) const {
    const auto page = impl_->session->shared_grade_linked_targets(
        layer_id.toStdString(),
        revision_id.toStdString(),
        after_photo_id.toStdString(),
        limit
    );
    BackendSharedGradeLinkedTargetsPage result;
    result.has_more = page.has_more;
    result.next_photo_id = qstring(page.next_photo_id);
    result.targets.reserve(checked_qt_vector_size(page.targets.size(), "shared_linked_targets"));
    for (const auto& source : page.targets) {
        result.targets.push_back(project_target(source));
    }
    return result;
}

BackendSharedGradeLinkedTarget DesktopBackend::sharedGradeLinkedTarget(
    const QString& layer_id,
    const QString& photo_id,
    const QString& source_path
) const {
    return project_target(impl_->session->shared_grade_linked_target(
        layer_id.toStdString(),
        photo_id.toStdString(),
        source_path.toStdString()
    ));
}

BackendSharedGradeLinkUpdateResult DesktopBackend::prepareSharedGradeLinkUpdate(
    const QString& layer_id,
    const QString& revision_id,
    const BackendSharedGradeLinkedTarget& target
) const {
    return project_result(
        impl_->session->prepare_shared_grade_link_update(
            layer_id.toStdString(),
            revision_id.toStdString(),
            ffi_target(target)
        ),
        true
    );
}

BackendSharedGradeLinkUpdateResult DesktopBackend::commitSharedGradeLinkUpdate(
    const QString& layer_id,
    const QString& revision_id,
    const BackendSharedGradeLinkedTarget& target
) const {
    return project_result(
        impl_->session->commit_shared_grade_link_update(
            layer_id.toStdString(),
            revision_id.toStdString(),
            ffi_target(target)
        ),
        false
    );
}

BackendPhotoEditState DesktopBackend::commitSharedGradeNodeLink(
    const AdjustmentTransferPrepared& prepared,
    const QString& layer_id,
    const QString& revision_id
) const {
    const auto& before = prepared.before;
    return edit_state(impl_->session->commit_shared_grade_node_link(
        prepared.plan_token,
        layer_id.toStdString(),
        revision_id.toStdString(),
        before.photo_id.toStdString(),
        before.source_path.toStdString(),
        prepared.content_anchor.toStdString(),
        before.base_commit_id.toStdString(),
        before.active_variant_id.toStdString(),
        ffi_grade_stack(before.grade_stack)
    ));
}
