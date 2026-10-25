#include "adjustment_transfer_operations.hpp"

#include "desktop_backend.hpp"
#include "edit_stack.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <exception>
#include <stdexcept>
#include <utility>

namespace {

// The in-process clipboard uses the same bounded document as Recipe export.
constexpr qsizetype maximum_document_bytes = 64 * 1024 * 1024;

class TransferError final : public std::runtime_error {
  public:
    TransferError(const AdjustmentTransferIssue issue, const char* detail) :
        std::runtime_error(
            QCoreApplication::translate("AdjustmentTransferOperations", detail).toStdString()
        ),
        issue(issue) {}

    const AdjustmentTransferIssue issue;
};

[[noreturn]] void fail(const AdjustmentTransferIssue issue, const char* detail) {
    throw TransferError(issue, detail);
}

void check_cancelled(const std::shared_ptr<AdjustmentTransferCancellation>& cancellation) {
    if (cancellation->cancelled())
        fail(
            AdjustmentTransferIssue::Cancelled,
            QT_TRANSLATE_NOOP("AdjustmentTransferOperations", "adjustment transfer cancelled")
        );
}

struct MaskCounts final {
    qsizetype leaves = 0;
    qsizetype semantic = 0;
};

void validate_source_masks(const BackendGradeStack& stack, const bool allow_spatial) {
    for (const auto& node : stack.grade_nodes) {
        for (const auto& component : node.local_mask_components) {
            switch (component.kind) {
            case 1:
            case 2:
            case 3:
                if (!allow_spatial)
                    fail(
                        AdjustmentTransferIssue::UnsupportedMask,
                        QT_TRANSLATE_NOOP(
                            "AdjustmentTransferOperations",
                            "spatial masks require explicit normalized-placement consent"
                        )
                    );
                break;
            case 4:
            case 5:
            case 7:
                break;
            case 6:
                if (component.semantic_query.trimmed().isEmpty())
                    fail(
                        AdjustmentTransferIssue::UnsupportedMask,
                        QT_TRANSLATE_NOOP(
                            "AdjustmentTransferOperations",
                            "a managed mask has no transferable semantic intent"
                        )
                    );
                break;
            default:
                fail(
                    AdjustmentTransferIssue::UnsupportedMask,
                    QT_TRANSLATE_NOOP(
                        "AdjustmentTransferOperations",
                        "the adjustment contains an unsupported mask component"
                    )
                );
            }
        }
    }
}

void inspect_mask(
    const QJsonObject& definition,
    const bool allow_spatial,
    const bool allow_composite,
    MaskCounts& counts
) {
    const auto kind = definition.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("composite")) {
        const auto components = definition.value(QStringLiteral("components"));
        if (!allow_composite || !components.isArray() || components.toArray().isEmpty()
            || components.toArray().size() > BACKEND_MAX_MASK_COMPONENTS)
            fail(
                AdjustmentTransferIssue::UnsupportedMask,
                QT_TRANSLATE_NOOP("AdjustmentTransferOperations", "invalid composite mask topology")
            );
        for (const auto& component : components.toArray()) {
            inspect_mask(
                component.toObject().value(QStringLiteral("definition")).toObject(),
                allow_spatial,
                false,
                counts
            );
        }
        return;
    }
    ++counts.leaves;
    if (kind == QStringLiteral("linear_gradient") || kind == QStringLiteral("radial_gradient")
        || kind == QStringLiteral("brush")) {
        if (!allow_spatial)
            fail(
                AdjustmentTransferIssue::UnsupportedMask,
                QT_TRANSLATE_NOOP(
                    "AdjustmentTransferOperations",
                    "spatial masks require explicit normalized-placement consent"
                )
            );
    } else if (kind == QStringLiteral("managed_raster")) {
        const auto intent = definition.value(QStringLiteral("semantic_intent")).toObject();
        if (intent.value(QStringLiteral("query")).toString().trimmed().isEmpty())
            fail(
                AdjustmentTransferIssue::UnsupportedMask,
                QT_TRANSLATE_NOOP(
                    "AdjustmentTransferOperations",
                    "a managed mask has no transferable semantic intent"
                )
            );
        ++counts.semantic;
    } else if (
        kind != QStringLiteral("luminance_range") && kind != QStringLiteral("color_range")
        && kind != QStringLiteral("condition_expression")
    ) {
        fail(
            AdjustmentTransferIssue::UnsupportedMask,
            QT_TRANSLATE_NOOP("AdjustmentTransferOperations", "unsupported Recipe mask definition")
        );
    }
}

[[nodiscard]] MaskCounts
inspect_document(const QByteArray& bytes, const int expected_nodes, const bool allow_spatial) {
    if (bytes.isEmpty() || bytes.size() > maximum_document_bytes)
        fail(
            AdjustmentTransferIssue::Failed,
            QT_TRANSLATE_NOOP(
                "AdjustmentTransferOperations",
                "adjustment clipboard exceeds its document bound"
            )
        );
    if (expected_nodes < 1 || expected_nodes > GradeNodeStack::maximum_grade_node_count)
        fail(
            AdjustmentTransferIssue::NodeLimit,
            QT_TRANSLATE_NOOP(
                "AdjustmentTransferOperations",
                "adjustment clipboard has an invalid node count"
            )
        );
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail(
            AdjustmentTransferIssue::Failed,
            QT_TRANSLATE_NOOP(
                "AdjustmentTransferOperations",
                "adjustment clipboard is not a Recipe document"
            )
        );
    const auto snapshot = document.object().value(QStringLiteral("snapshot")).toObject();
    const auto layers = snapshot.value(QStringLiteral("layers"));
    if (!layers.isArray() || layers.toArray().size() != expected_nodes)
        fail(
            AdjustmentTransferIssue::Failed,
            QT_TRANSLATE_NOOP(
                "AdjustmentTransferOperations",
                "adjustment clipboard node snapshot is incomplete"
            )
        );
    MaskCounts counts;
    const auto masks = snapshot.value(QStringLiteral("masks"));
    if (!masks.isUndefined() && !masks.isArray())
        fail(
            AdjustmentTransferIssue::UnsupportedMask,
            QT_TRANSLATE_NOOP("AdjustmentTransferOperations", "invalid Recipe mask collection")
        );
    for (const auto& mask : masks.toArray()) {
        inspect_mask(
            mask.toObject().value(QStringLiteral("definition")).toObject(),
            allow_spatial,
            true,
            counts
        );
    }
    // Domain decoding below validates values, identities, references, and the
    // document digest. This pass only enforces this workflow's mask policy.
    return counts;
}

void validate_preview(const BackendShadowRecipeImportPreview& preview, const int expected_nodes) {
    if (preview.grade_node_count != static_cast<std::uint32_t>(expected_nodes))
        fail(
            AdjustmentTransferIssue::Failed,
            QT_TRANSLATE_NOOP(
                "AdjustmentTransferOperations",
                "Recipe projection lost adjustment nodes"
            )
        );
    if (preview.removed_lut_count != 0)
        fail(
            AdjustmentTransferIssue::Failed,
            QT_TRANSLATE_NOOP(
                "AdjustmentTransferOperations",
                "a complete LUT resource could not be transferred"
            )
        );
    // Never apply portable_grade_stack: that convenience projection can bypass
    // a whole node when one managed mask is unresolved. The plan must resolve
    // every leaf before any part of this photo is published.
}

class RecipePlanLease final {
  public:
    RecipePlanLease(
        DesktopBackend& backend,
        const std::uint64_t token,
        std::shared_ptr<AdjustmentTransferCancellation> cancellation
    ) : backend_(backend), token_(token), cancellation_(std::move(cancellation)) {}
    RecipePlanLease(RecipePlanLease&& other) noexcept :
        backend_(other.backend_), token_(std::exchange(other.token_, 0)),
        cancellation_(std::move(other.cancellation_)) {}
    RecipePlanLease(const RecipePlanLease&) = delete;
    RecipePlanLease& operator=(const RecipePlanLease&) = delete;
    ~RecipePlanLease() {
        close();
    }

    void close() noexcept {
        const auto token = std::exchange(token_, 0);
        if (token == 0)
            return;
        try {
            backend_.closeSemanticRecipeImport(token);
        } catch (...) {}
    }

    [[nodiscard]] bool matches(const DesktopBackend& backend, const std::uint64_t token) const {
        return &backend_ == &backend && token != 0 && token_ == token;
    }

    void checkCancelled() const {
        check_cancelled(cancellation_);
    }

  private:
    DesktopBackend& backend_;
    std::uint64_t token_;
    std::shared_ptr<AdjustmentTransferCancellation> cancellation_;
};

struct ClearActiveOnExit final {
    std::shared_ptr<AdjustmentTransferCancellation> cancellation;
    ~ClearActiveOnExit() {
        cancellation->clearActive();
    }
};

struct ClosePlanOnExit final {
    std::shared_ptr<RecipePlanLease> plan;
    ~ClosePlanOnExit() {
        if (plan)
            plan->close();
    }
};

[[nodiscard]] AdjustmentTransferIssue item_issue(const BackendRecipeImportItemTerminal terminal) {
    switch (terminal) {
    case BackendRecipeImportItemTerminal::NotFound:
        return AdjustmentTransferIssue::NotFound;
    case BackendRecipeImportItemTerminal::Unavailable:
        return AdjustmentTransferIssue::Unavailable;
    case BackendRecipeImportItemTerminal::Cancelled:
        return AdjustmentTransferIssue::Cancelled;
    default:
        return AdjustmentTransferIssue::Failed;
    }
}

[[nodiscard]] AdjustmentTransferPrepared prepare_transfer(
    DesktopBackend& backend,
    const AdjustmentTransferClipboard& clipboard,
    const BackendBatchPhotoTarget& target,
    const std::shared_ptr<AdjustmentTransferCancellation>& requested_cancellation
) {
    AdjustmentTransferPrepared result;
    const auto cancellation = requested_cancellation
                                  ? requested_cancellation
                                  : std::make_shared<AdjustmentTransferCancellation>();
    std::shared_ptr<RecipePlanLease> lease;
    // Unregister before destroying a failed plan; the same batch cancellation
    // object may subsequently belong to another target's active inference.
    const ClearActiveOnExit clear_active{cancellation};
    try {
        check_cancelled(cancellation);
        result.before = backend.photoEditState(target.photo_id, target.source_path);
        result.content_anchor = result.before.base_commit_id;
        check_cancelled(cancellation);
        if (result.before.photo_id != target.photo_id
            || result.before.source_path != target.source_path
            || result.before.active_variant_id.isEmpty())
            fail(
                AdjustmentTransferIssue::Changed,
                QT_TRANSLATE_NOOP(
                    "AdjustmentTransferOperations",
                    "destination edit identity is incomplete or changed"
                )
            );
        const auto masks = inspect_document(
            clipboard.document,
            clipboard.node_count,
            clipboard.allow_spatial_masks
        );
        if (result.before.grade_stack.grade_nodes.size() + clipboard.node_count
            > GradeNodeStack::maximum_grade_node_count)
            fail(
                AdjustmentTransferIssue::NodeLimit,
                QT_TRANSLATE_NOOP(
                    "AdjustmentTransferOperations",
                    "destination would exceed the 16-node limit"
                )
            );
        check_cancelled(cancellation);
        validate_preview(backend.previewShadowRecipe(clipboard.document), clipboard.node_count);
        check_cancelled(cancellation);
        const auto plan = backend.prepareSemanticRecipeImport(
            target.photo_id,
            target.source_path,
            result.before.base_commit_id,
            result.before.base_commit_id,
            result.before.grade_stack,
            clipboard.document
        );
        RecipePlanLease pending_lease(backend, plan.plan_token, cancellation);
        lease = std::make_shared<RecipePlanLease>(std::move(pending_lease));
        check_cancelled(cancellation);
        if (plan.plan_token == 0 || plan.nodes.size() != clipboard.node_count
            || plan.items.size() != masks.semantic)
            fail(
                AdjustmentTransferIssue::Failed,
                QT_TRANSLATE_NOOP(
                    "AdjustmentTransferOperations",
                    "semantic Recipe plan is incomplete"
                )
            );
        for (const auto& node : plan.nodes) {
            if (node.excluded || node.unsupported_managed_leaf_count != 0)
                fail(
                    AdjustmentTransferIssue::UnsupportedMask,
                    QT_TRANSLATE_NOOP(
                        "AdjustmentTransferOperations",
                        "a complete adjustment mask cannot be transferred"
                    )
                );
        }
        for (const auto& item : plan.items) {
            check_cancelled(cancellation);
            const auto job = backend.beginSemanticRecipeImportItem(plan.plan_token, item.item_id);
            cancellation->setActive([&backend, token = plan.plan_token, id = item.item_id, job] {
                try {
                    static_cast<void>(backend.cancelSemanticRecipeImportItem(token, id, job));
                } catch (...) {}
            });
            // setActive observes cancellation that raced with begin/registration.
            check_cancelled(cancellation);
            const auto executed =
                backend.executeSemanticRecipeImportItem(plan.plan_token, item.item_id, job);
            cancellation->clearActive();
            check_cancelled(cancellation);
            if (executed.terminal != BackendRecipeImportItemTerminal::Staged) {
                result.failure = {item_issue(executed.terminal), executed.detail};
                return result;
            }
            const auto accepted = backend.acceptSemanticRecipeImportItem(
                plan.plan_token,
                item.item_id,
                executed.proposal_token,
                executed.generation
            );
            check_cancelled(cancellation);
            if (accepted.terminal != BackendRecipeImportItemTerminal::Completed) {
                result.failure = {item_issue(accepted.terminal), accepted.detail};
                return result;
            }
        }
        check_cancelled(cancellation);
        const auto imported = backend.finalizeSemanticRecipeImport(
            plan.plan_token,
            target.photo_id,
            target.source_path,
            result.before.base_commit_id,
            result.before.base_commit_id,
            result.before.grade_stack
        );
        check_cancelled(cancellation);
        if (imported.grade_nodes.size() != clipboard.node_count)
            fail(
                AdjustmentTransferIssue::Failed,
                QT_TRANSLATE_NOOP(
                    "AdjustmentTransferOperations",
                    "finalized Recipe lost adjustment nodes"
                )
            );
        validate_source_masks(imported, clipboard.allow_spatial_masks);
        MaskCounts imported_masks;
        result.after = result.before.grade_stack;
        for (const auto& node : imported.grade_nodes) {
            if (!node.shared_layer_id.isEmpty() || !node.shared_revision_id.isEmpty()
                || !GradeNodeStack::canInsert(result.after, node))
                fail(
                    AdjustmentTransferIssue::Failed,
                    QT_TRANSLATE_NOOP(
                        "AdjustmentTransferOperations",
                        "imported adjustment identity is not independent"
                    )
                );
            imported_masks.leaves += node.local_mask_components.size();
            for (const auto& component : node.local_mask_components)
                imported_masks.semantic += component.kind == 6;
            result.after.grade_nodes.push_back(node);
        }
        if (imported_masks.leaves != masks.leaves || imported_masks.semantic != masks.semantic)
            fail(
                AdjustmentTransferIssue::UnsupportedMask,
                QT_TRANSLATE_NOOP(
                    "AdjustmentTransferOperations",
                    "finalized Recipe lost mask components"
                )
            );
        check_cancelled(cancellation);
        result.plan_token = plan.plan_token;
        result.retained_plan = lease;
    } catch (const TransferError& error) {
        result.failure = {error.issue, QString::fromUtf8(error.what())};
    } catch (const std::exception& error) {
        result.failure = {
            cancellation->cancelled() ? AdjustmentTransferIssue::Cancelled
                                      : AdjustmentTransferIssue::Failed,
            QString::fromUtf8(error.what())
        };
    } catch (...) {
        result.failure.issue = cancellation->cancelled() ? AdjustmentTransferIssue::Cancelled
                                                         : AdjustmentTransferIssue::Failed;
    }
    return result;
}

} // namespace

AdjustmentTransferOperations adjustmentTransferOperations(DesktopBackend& backend) {
    return {
        .copy =
            [&backend](const AdjustmentTransferSource& source) {
                if (source.stack.grade_nodes.isEmpty()
                    || source.stack.grade_nodes.size() > GradeNodeStack::maximum_grade_node_count)
                    fail(
                        AdjustmentTransferIssue::NodeLimit,
                        QT_TRANSLATE_NOOP(
                            "AdjustmentTransferOperations",
                            "copy requires 1 through 16 adjustment nodes"
                        )
                    );
                validate_source_masks(source.stack, source.allow_spatial_masks);
                AdjustmentTransferClipboard clipboard{
                    .document = backend.exportShadowRecipe(
                        source.photo_id,
                        source.source_path,
                        source.base_commit_id,
                        source.stack,
                        {}
                    ),
                    .source_photo_id = source.photo_id,
                    .source_name = source.name,
                    .node_count = static_cast<int>(source.stack.grade_nodes.size()),
                    .allow_spatial_masks = source.allow_spatial_masks,
                };
                const auto masks = inspect_document(
                    clipboard.document,
                    clipboard.node_count,
                    clipboard.allow_spatial_masks
                );
                clipboard.requires_inference = masks.semantic > 0;
                MaskCounts source_masks;
                for (const auto& node : source.stack.grade_nodes) {
                    source_masks.leaves += node.local_mask_components.size();
                    for (const auto& component : node.local_mask_components)
                        source_masks.semantic += component.kind == 6;
                }
                if (source_masks.leaves != masks.leaves || source_masks.semantic != masks.semantic)
                    fail(
                        AdjustmentTransferIssue::UnsupportedMask,
                        QT_TRANSLATE_NOOP(
                            "AdjustmentTransferOperations",
                            "copy could not preserve complete masks"
                        )
                    );
                validate_preview(
                    backend.previewShadowRecipe(clipboard.document),
                    clipboard.node_count
                );
                return clipboard;
            },
        .load = [&backend](
                    const BackendBatchPhotoTarget& target
                ) { return backend.photoEditState(target.photo_id, target.source_path); },
        .prepare = [&backend](
                       const AdjustmentTransferClipboard& clipboard,
                       const BackendBatchPhotoTarget& target,
                       const std::shared_ptr<AdjustmentTransferCancellation>& cancellation
                   ) { return prepare_transfer(backend, clipboard, target, cancellation); },
        .write =
            [&backend](const AdjustmentTransferPrepared& prepared) {
                const auto lease =
                    std::static_pointer_cast<RecipePlanLease>(prepared.retained_plan);
                const ClosePlanOnExit close_plan{lease};
                if (prepared.failure.issue != AdjustmentTransferIssue::None)
                    fail(
                        prepared.failure.issue,
                        QT_TRANSLATE_NOOP(
                            "AdjustmentTransferOperations",
                            "cannot write a failed adjustment transfer"
                        )
                    );
                const auto& before = prepared.before;
                if (before.photo_id.isEmpty() || before.source_path.isEmpty()
                    || before.active_variant_id.isEmpty())
                    fail(
                        AdjustmentTransferIssue::Changed,
                        QT_TRANSLATE_NOOP(
                            "AdjustmentTransferOperations",
                            "destination edit identity is incomplete"
                        )
                    );
                if (prepared.after.grade_nodes.isEmpty()
                    || prepared.after.grade_nodes.size() > GradeNodeStack::maximum_grade_node_count)
                    fail(
                        AdjustmentTransferIssue::NodeLimit,
                        QT_TRANSLATE_NOOP(
                            "AdjustmentTransferOperations",
                            "destination has an invalid node count"
                        )
                    );
                if (prepared.plan_token != 0) {
                    if (!lease || !lease->matches(backend, prepared.plan_token))
                        fail(
                            AdjustmentTransferIssue::Changed,
                            QT_TRANSLATE_NOOP(
                                "AdjustmentTransferOperations",
                                "prepared Recipe plan is no longer available"
                            )
                        );
                    lease->checkCancelled();
                    return backend.commitSemanticRecipeAppend(
                        prepared.plan_token,
                        before.photo_id,
                        before.source_path,
                        prepared.content_anchor,
                        before.base_commit_id,
                        before.active_variant_id,
                        before.grade_stack
                    );
                }
                // Undo supplies the old content anchor and stack independently of
                // the current expected head, preserving opaque old mask identities.
                return backend.autosaveWorkingEdit(
                    before.photo_id,
                    before.source_path,
                    prepared.content_anchor,
                    before.base_commit_id,
                    before.active_variant_id,
                    prepared.after
                );
            },
        .preview =
            [&backend](
                const BackendPhotoEditState& saved,
                const std::shared_ptr<AdjustmentTransferCancellation>& cancellation
            ) {
                if (cancellation->cancelled())
                    return false;
                const auto token = backend.beginEditPreviewRequest();
                const ClearActiveOnExit clear_active{cancellation};
                cancellation->setActive([&backend, token] {
                    (void)backend.cancelEditPreviewRequest(token);
                });
                // Publish the same saved Recipe through the existing warm render
                // and durable Gallery boundary; no independent grading executor.
                const auto preview = backend.renderEditPreview(
                    saved.photo_id,
                    saved.source_path,
                    saved.base_commit_id,
                    saved.grade_stack,
                    token,
                    1024,
                    88,
                    EditPreviewPolicy::PresentationCommit,
                    std::nullopt
                );
                return preview.terminal == EditPreviewTerminal::Completed;
            },
    };
}
