#include "shared_grade_link_controller.hpp"

#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTranslator>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace {
class ProductTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        return QByteArray(context) == "SharedGradeLinkController"
                   ? QStringLiteral("translated: ") + QString::fromUtf8(source)
                   : QString{};
    }
};
struct Fixture final {
    std::mutex mutex;
    QHash<QString, BackendPhotoEditState> states;
    BackendSharedGradeNode shared;
    QVector<BackendSharedGradeNode> extra_heads;
    QHash<QString, BackendSharedGradeLinkStatus> failures;
    QHash<QString, int> writes_by_photo;
    BackendSharedGradeLinkedTarget prepared_subscription;
    std::atomic_int publications{0}, writes{0}, prepares{0}, pages{0}, copies{0}, previews{0};
    std::atomic_bool hold_prepare{false}, entered_prepare{false};
    std::atomic_bool hold_write{false}, entered_write{false};
    std::atomic_bool hold_preview{false}, entered_preview{false};
    std::atomic_bool foreground_busy{false};
    bool changed_source = false;
    bool changed_target_draft = false;
    bool reserved = false;
    bool released = false;
    bool empty_first_page = false;
    bool bad_cursor = false;
    bool unsaved_source = false;
    bool masks_allowed = true;
    bool offline_a = false;
    bool source_extra_follow = false;
    bool semantic_clipboard = false;
    std::uint64_t byte_len = 111;
    bool has_modified_at = true;
    std::int64_t modified_at = 333;
    QHash<QString, QString> representation_overrides;
    int page_limit = 64;
    QString current_photo;

    Fixture() {
        shared.layer_id = QStringLiteral("shared-layer");
        shared.revision_id = QStringLiteral("revision-1");
        shared.revision_number = 1;
        shared.label = QStringLiteral("Shared tone");
        shared.grade_node.grade_node_id = QStringLiteral("library-node");
        shared.grade_node.label = shared.label;
        for (const auto* id : {"source", "a", "b", "new"}) {
            BackendPhotoEditState state;
            state.photo_id = id;
            state.source_path = QStringLiteral("/isolated/%1.dng").arg(id);
            state.base_commit_id = QStringLiteral("head-%1").arg(id);
            state.active_variant_id = QStringLiteral("variant-%1").arg(id);
            BackendGradeNode node;
            node.grade_node_id = QStringLiteral("instance-%1").arg(id);
            node.label = shared.label;
            node.shared_layer_id = shared.layer_id;
            node.shared_revision_id = shared.revision_id;
            node.shared_revision_number = 1;
            node.shared_follow_head = true;
            node.opacity = 0.35;
            node.enabled = false;
            BackendMaskComponent mask;
            mask.component_id = QStringLiteral("private-mask-%1").arg(id);
            mask.kind = 2;
            node.local_mask_components = {mask};
            node.local_mask_invert = true;
            if (state.photo_id == QStringLiteral("new")) {
                node.shared_layer_id.clear();
                node.shared_revision_id.clear();
                node.shared_follow_head = false;
                node.shared_revision_number = 0;
            }
            state.grade_stack.grade_nodes = {node};
            state.grade_stack.geometry.crop_left = 0.2;
            state.grade_stack.foundation.temperature_kelvin = 7200;
            states.insert(id, state);
        }
    }
    BackendPhotoEditState read(const QString& id) {
        const std::scoped_lock lock(mutex);
        return states.value(id);
    }
    QVariantList targets(std::initializer_list<const char*> ids) {
        QVariantList values;
        for (const auto* id : ids)
            values.push_back(
                QVariantMap{
                    {"photoId", id},
                    {"sourcePath", QStringLiteral("/isolated/%1.dng").arg(id)}
                }
            );
        return values;
    }
    BackendSharedGradeLinkedTarget
    subscription(const BackendPhotoEditState& state, QString layer = {}) {
        if (layer.isEmpty())
            layer = shared.layer_id;
        BackendSharedGradeLinkedTarget target;
        target.photo_id = state.photo_id;
        target.source_path = state.source_path;
        target.representation_id = representation_overrides.value(
            state.photo_id,
            QStringLiteral("representation-%1").arg(state.photo_id)
        );
        target.variant_id = state.active_variant_id;
        target.working_commit_id = state.base_commit_id;
        target.source_byte_len = byte_len;
        target.has_source_modified_at_ms = has_modified_at;
        target.source_modified_at_ms = modified_at;
        for (const auto& node : state.grade_stack.grade_nodes) {
            if (node.shared_follow_head && node.shared_layer_id == layer) {
                target.grade_node_ids.push_back(node.grade_node_id);
                target.shared_revision_ids.push_back(node.shared_revision_id);
            }
        }
        return target;
    }
    SharedGradeLinkOperations operations() {
        return {
            .nodes =
                [this] {
                    auto nodes = extra_heads;
                    nodes.prepend(shared);
                    return nodes;
                },
            .publish =
                [this](const SharedGradeLinkSource& source, const QString& label) {
                    const std::scoped_lock lock(mutex);
                    auto& state = states[source.state.photo_id];
                    if (state.base_commit_id != source.expected_working_commit_id
                        || state.active_variant_id != source.state.active_variant_id)
                        throw std::runtime_error("source head changed");
                    ++publications;
                    shared.label = label;
                    shared.revision_id = QStringLiteral("revision-%1").arg(publications + 1);
                    shared.revision_number = static_cast<std::uint32_t>(publications + 1);
                    auto stack = source.state.grade_stack;
                    for (auto& node : stack.grade_nodes) {
                        if (node.grade_node_id != source.grade_node_id)
                            continue;
                        shared.grade_node.basic = node.basic;
                        shared.grade_node.fine = node.fine;
                        node.shared_layer_id = shared.layer_id;
                        node.shared_revision_id = shared.revision_id;
                        node.shared_revision_number = shared.revision_number;
                        node.shared_follow_head = true;
                    }
                    state.grade_stack = std::move(stack);
                    state.base_commit_id =
                        QStringLiteral("publication-%1").arg(publications.load());
                    return BackendSharedGradePublicationReceipt{shared, state};
                },
            .linked_targets =
                [this](
                    const auto&,
                    const auto& revision,
                    const QString& after,
                    std::uint32_t limit
                ) {
                    ++pages;
                    BackendSharedGradeLinkedTargetsPage page;
                    if (empty_first_page && after.isEmpty()) {
                        page.has_more = true;
                        page.next_photo_id = QStringLiteral("0");
                        return page;
                    }
                    const std::scoped_lock lock(mutex);
                    auto ids = states.keys();
                    ids.sort();
                    for (const auto& id : ids) {
                        if (id <= after || (id == QStringLiteral("source") && !source_extra_follow)
                            || id == QStringLiteral("new"))
                            continue;
                        const auto& state = states[id];
                        auto linked = subscription(state);
                        for (qsizetype index = linked.grade_node_ids.size(); index-- > 0;) {
                            if (linked.shared_revision_ids[index] == revision) {
                                linked.grade_node_ids.removeAt(index);
                                linked.shared_revision_ids.removeAt(index);
                            }
                        }
                        if (linked.grade_node_ids.isEmpty())
                            continue;
                        page.targets.push_back(linked);
                        if (offline_a && id == QStringLiteral("a"))
                            page.targets.last().source_path.clear();
                        page.next_photo_id = id;
                        if (page.targets.size()
                            >= std::min<int>(page_limit, static_cast<int>(limit))) {
                            page.has_more = true;
                            break;
                        }
                    }
                    if (bad_cursor) {
                        page.has_more = true;
                        page.next_photo_id = after;
                    }
                    return page;
                },
            .target =
                [this](const QString& layer, const QString& id, const auto&) {
                    return subscription(read(id), layer);
                },
            .copy =
                [this](const AdjustmentTransferSource& source) {
                    ++copies;
                    return AdjustmentTransferClipboard{
                        QByteArray("independent-transfer-plan"),
                        source.photo_id,
                        source.name,
                        1,
                        source.allow_spatial_masks,
                        semantic_clipboard
                    };
                },
            .prepare =
                [this](
                    const BackendSharedGradeNode& published,
                    const auto&,
                    const SharedGradeLinkTarget& target,
                    const auto& cancellation
                ) {
                    ++prepares;
                    entered_prepare = true;
                    while (hold_prepare && !cancellation->cancelled())
                        QThread::msleep(1);
                    SharedGradeLinkPrepared result;
                    result.before = read(target.photo.photo_id);
                    auto intent = target.subscription;
                    if (!intent) {
                        const auto current = subscription(result.before, published.layer_id);
                        result.frozen_before = target.frozen_before
                                                   ? target.frozen_before
                                                   : sharedGradeLinkTargetIdentity(current);
                        result.status =
                            validateSharedGradeLinkTargetIdentity(*result.frozen_before, current);
                        if (result.status != BackendSharedGradeLinkStatus::Ready)
                            return result;
                    }
                    if (intent && target.refresh_subscription) {
                        BackendSharedGradeLinkedTarget refreshed;
                        const auto status = refreshSharedGradeSubscriptionIntent(
                            *intent,
                            subscription(result.before, published.layer_id),
                            published.revision_id,
                            refreshed
                        );
                        if (status != BackendSharedGradeLinkStatus::Ready) {
                            result.status = status;
                            return result;
                        }
                        intent = refreshed;
                    }
                    if (intent) {
                        prepared_subscription = *intent;
                        if (intent->source_path.isEmpty()) {
                            result.status = BackendSharedGradeLinkStatus::Unavailable;
                            return result;
                        }
                        if (intent->working_commit_id != result.before.base_commit_id) {
                            result.status = BackendSharedGradeLinkStatus::HeadChanged;
                            return result;
                        }
                        for (const auto& id : intent->grade_node_ids) {
                            const auto& nodes = result.before.grade_stack.grade_nodes;
                            const auto found =
                                std::find_if(nodes.cbegin(), nodes.cend(), [&](const auto& node) {
                                    return node.grade_node_id == id;
                                });
                            if (found != nodes.cend() && found->basic.exposure_stops == 9) {
                                result.status = BackendSharedGradeLinkStatus::LocalChanged;
                                return result;
                            }
                        }
                    }
                    const auto failure =
                        failures.value(target.photo.photo_id, BackendSharedGradeLinkStatus::Ready);
                    if (failure != BackendSharedGradeLinkStatus::Ready) {
                        result.status = failure;
                        return result;
                    }
                    const auto& nodes = result.before.grade_stack.grade_nodes;
                    const auto linked =
                        std::find_if(nodes.cbegin(), nodes.cend(), [&](const auto& node) {
                            return node.shared_layer_id == published.layer_id;
                        });
                    bool all_current = linked != nodes.cend();
                    if (intent) {
                        for (const auto& id : intent->grade_node_ids) {
                            all_current =
                                all_current
                                && std::any_of(nodes.cbegin(), nodes.cend(), [&](const auto& node) {
                                       return node.grade_node_id == id
                                              && node.shared_revision_id == published.revision_id;
                                   });
                        }
                    } else
                        all_current =
                            all_current && linked->shared_revision_id == published.revision_id;
                    if (all_current) {
                        result.status = BackendSharedGradeLinkStatus::Unchanged;
                        return result;
                    }
                    result.status = BackendSharedGradeLinkStatus::Ready;
                    result.new_link = linked == nodes.cend();
                    result.subscription =
                        intent ? *intent : subscription(result.before, published.layer_id);
                    return result;
                },
            .write =
                [this](
                    const BackendSharedGradeNode& published,
                    const SharedGradeLinkPrepared& prepared
                ) {
                    entered_write = true;
                    while (hold_write)
                        QThread::msleep(1);
                    const std::scoped_lock lock(mutex);
                    auto& state = states[prepared.before.photo_id];
                    if (state.base_commit_id != prepared.before.base_commit_id
                        || state.active_variant_id != prepared.before.active_variant_id)
                        throw std::runtime_error("destination head changed");
                    ++writes;
                    ++writes_by_photo[state.photo_id];
                    if (prepared.new_link) {
                        auto added = published.grade_node;
                        added.grade_node_id = QStringLiteral("linked-%1").arg(state.photo_id);
                        state.grade_stack.grade_nodes.push_back(added);
                    }
                    for (auto& node : state.grade_stack.grade_nodes) {
                        if (prepared.new_link
                                ? node.grade_node_id
                                      != state.grade_stack.grade_nodes.last().grade_node_id
                                : !prepared.subscription.grade_node_ids.contains(
                                      node.grade_node_id
                                  ))
                            continue;
                        node.basic = published.grade_node.basic;
                        node.fine = published.grade_node.fine;
                        node.shared_layer_id = published.layer_id;
                        node.shared_revision_id = published.revision_id;
                        node.shared_revision_number = published.revision_number;
                        node.shared_follow_head = true;
                    }
                    state.base_commit_id = QStringLiteral("saved-%1").arg(writes.load());
                    return BackendSharedGradeLinkUpdateResult{
                        BackendSharedGradeLinkStatus::Updated,
                        state,
                        {}
                    };
                },
            .preview =
                [this](const auto&, const auto& cancellation) {
                    ++previews;
                    entered_preview = true;
                    while (hold_preview && !cancellation->cancelled())
                        QThread::msleep(1);
                    return !cancellation->cancelled();
                },
        };
    }
    SharedGradeLinkEditor editor() {
        return {
            [this] {
                auto state = read(QStringLiteral("source"));
                auto source = SharedGradeLinkSource{
                    state,
                    state.base_commit_id,
                    QStringLiteral("representation-source"),
                    state.grade_stack.grade_nodes.first().grade_node_id,
                    QStringLiteral("Source photo"),
                    shared.label
                };
                if (unsaved_source)
                    source.state.grade_stack.geometry.crop_left = 0.4;
                return source;
            },
            [this] { return BackendBatchPhotoTarget{current_photo, {}}; },
            [this](const SharedGradeLinkSource& source) {
                if (changed_source
                    || read(source.state.photo_id).base_commit_id
                           != source.expected_working_commit_id)
                    return AdjustmentTransferAdmission::Changed;
                reserved = true;
                return AdjustmentTransferAdmission::Ready;
            },
            [this](const auto&) {
                if (changed_target_draft)
                    return AdjustmentTransferAdmission::Changed;
                reserved = true;
                return AdjustmentTransferAdmission::Ready;
            },
            [](const auto&, const auto&, const auto&, const auto&) {},
            [this] {
                reserved = false;
                released = true;
            },
            [this] { return masks_allowed; },
            [this] { return foreground_busy.load(); },
        };
    }
};
} // namespace

class SharedGradeLinkControllerTest final : public QObject {
    Q_OBJECT
  private slots:
    void explicitPublicationCommitsDraftAndPreservesPrivateState() {
        Fixture fixture;
        fixture.unsaved_source = true;
        const auto target_before = fixture.read(QStringLiteral("a"));
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.requestPublication();
        controller.publish(
            QStringLiteral("New shared tone"),
            fixture.targets({"a", "b", "source", "a"})
        );
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.publications.load(), 1);
        QCOMPARE(fixture.writes.load(), 2);
        QCOMPARE(fixture.copies.load(), 0);
        QCOMPARE(controller.completed(), 2);
        QCOMPARE(controller.failed(), 0);
        QCOMPARE(fixture.read(QStringLiteral("source")).grade_stack.geometry.crop_left, 0.4);
        const auto target_after = fixture.read(QStringLiteral("a"));
        const auto& node_before = target_before.grade_stack.grade_nodes.first();
        const auto& node_after = target_after.grade_stack.grade_nodes.first();
        QCOMPARE(node_after.grade_node_id, node_before.grade_node_id);
        QCOMPARE(node_after.opacity, node_before.opacity);
        QCOMPARE(node_after.enabled, node_before.enabled);
        QVERIFY(node_after.local_mask_components == node_before.local_mask_components);
        QCOMPARE(node_after.local_mask_invert, node_before.local_mask_invert);
        QVERIFY(target_after.grade_stack.foundation == target_before.grade_stack.foundation);
        QVERIFY(target_after.grade_stack.geometry == target_before.grade_stack.geometry);
        QVERIFY(!fixture.reserved);
    }
    void sharingDialogFreezesSourceAndRejectsChangedDraft() {
        Fixture fixture;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.requestPublication();
        fixture.states[QStringLiteral("source")].grade_stack.geometry.crop_left = 0.6;
        fixture.changed_source = true;
        controller.publish(QStringLiteral("Tone"), fixture.targets({"a"}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.publications.load(), 0);
        QCOMPARE(fixture.writes.load(), 0);
        QVERIFY(controller.statusText().contains(QStringLiteral("changed")));
    }
    void ownAutosaveHeadAdvanceWithIdenticalContentCanPublishFrozenRequest() {
        Fixture fixture;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.requestPublication();
        fixture.states[QStringLiteral("source")].base_commit_id =
            QStringLiteral("own-autosave-ack");
        controller.publish(QStringLiteral("Tone"), {});
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.publications.load(), 1);
        QCOMPARE(fixture.writes.load(), 2);
    }
    void currentSyncPreservesSourceFingerprintAndSelectedInstanceSubset() {
        Fixture fixture;
        auto& stack = fixture.states[QStringLiteral("source")].grade_stack;
        auto additional = stack.grade_nodes.first();
        additional.grade_node_id = QStringLiteral("second-instance");
        stack.grade_nodes.push_back(additional);
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.syncSelected();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.prepared_subscription.source_byte_len, std::uint64_t(111));
        QVERIFY(fixture.prepared_subscription.has_source_modified_at_ms);
        QCOMPARE(fixture.prepared_subscription.source_modified_at_ms, std::int64_t(333));
        QCOMPARE(fixture.prepared_subscription.grade_node_ids.size(), 1);
        QCOMPARE(
            fixture.prepared_subscription.grade_node_ids.first(),
            QStringLiteral("instance-source")
        );
        controller.refreshCurrentLinks();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.prepared_subscription.grade_node_ids.size(), 2);
        QCOMPARE(fixture.prepared_subscription.source_byte_len, std::uint64_t(111));
    }
    void localChangesRemainVisibleAndRetryDoesNotRepublishSuccesses() {
        Fixture fixture;
        fixture.failures.insert(QStringLiteral("b"), BackendSharedGradeLinkStatus::LocalChanged);
        const auto before = fixture.read(QStringLiteral("b"));
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.publish(QStringLiteral("Tone"), {});
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.failed(), 1);
        QVERIFY(controller.canRetry());
        QVERIFY(fixture.read(QStringLiteral("b")).grade_stack == before.grade_stack);
        fixture.failures.clear();
        controller.retryFailed();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.failed(), 0);
        QCOMPARE(fixture.publications.load(), 1);
        QCOMPARE(fixture.writes_by_photo.value(QStringLiteral("a")), 1);
        QCOMPARE(fixture.writes_by_photo.value(QStringLiteral("b")), 1);
    }
    void cancellationDuringAcknowledgedWriteKeepsReceiptAndRetriesOnlyPending() {
        Fixture fixture;
        fixture.hold_write = true;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.publish(QStringLiteral("Tone"), {});
        QTRY_VERIFY_WITH_TIMEOUT(fixture.entered_write.load(), 5000);
        controller.cancel();
        fixture.hold_write = false;
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 1);
        QCOMPARE(controller.completed(), 1);
        QCOMPARE(controller.failed(), 1);
        controller.retryFailed();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.publications.load(), 1);
        QCOMPARE(fixture.writes.load(), 2);
        QCOMPARE(controller.failed(), 0);
    }
    void paginationStreamsWorkBeforeFetchingNextPageAndAcceptsEmptyPages() {
        Fixture fixture;
        fixture.empty_first_page = true;
        fixture.page_limit = 1;
        fixture.hold_prepare = true;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.publish(QStringLiteral("Tone"), {});
        QTRY_VERIFY_WITH_TIMEOUT(fixture.entered_prepare.load(), 5000);
        QCOMPARE(fixture.pages.load(), 2); // filtered empty page, then first actual target
        fixture.hold_prepare = false;
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 2);
        QCOMPARE(controller.completed(), 2);
        QVERIFY(fixture.pages.load() >= 3);
    }
    void nonAdvancingCursorFailsWithoutTargetMutationAndRetryKeepsPublishedRevision() {
        Fixture fixture;
        fixture.bad_cursor = true;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.publish(QStringLiteral("Tone"), {});
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.publications.load(), 1);
        QCOMPARE(fixture.writes.load(), 0);
        QVERIFY(controller.canRetry());
        fixture.bad_cursor = false;
        controller.retryFailed();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.publications.load(), 1);
        QCOMPARE(fixture.writes.load(), 2);
    }
    void foregroundPreviewPreemptsBackgroundWithoutRewritingMetadata() {
        Fixture fixture;
        fixture.hold_preview = true;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.publish(QStringLiteral("Tone"), {});
        QTRY_VERIFY_WITH_TIMEOUT(fixture.entered_preview.load(), 5000);
        QVERIFY(!fixture.reserved);
        fixture.foreground_busy = true;
        controller.foregroundWorkChanged();
        QTest::qWait(100);
        QCOMPARE(fixture.previews.load(), 1);
        fixture.hold_preview = false;
        fixture.foreground_busy = false;
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 2);
        QVERIFY(fixture.previews.load() >= 3);
    }
    void changedTargetDraftBlocksWriteAndProfileSwitchCancelsAdmission() {
        Fixture fixture;
        fixture.changed_target_draft = true;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.publish(QStringLiteral("Tone"), {});
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 0);
        QCOMPARE(controller.failed(), 2);
        controller.setProfileSwitchPending(true);
        QVERIFY(!controller.admissionEnabled());
        QVERIFY(!controller.canRetry());
        controller.publish(QStringLiteral("Another"), {});
        QCOMPARE(fixture.publications.load(), 1);
    }
    void newLinksUseIndependentTransferWithoutRepublishing() {
        Fixture fixture;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.requestLinks();
        controller.linkToPhotos(fixture.targets({"new"}), true);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.publications.load(), 0);
        QCOMPARE(fixture.copies.load(), 1);
        QCOMPARE(fixture.writes.load(), 1);
        QCOMPARE(fixture.read(QStringLiteral("new")).grade_stack.grade_nodes.size(), 2);
        controller.requestLinks();
        controller.linkToPhotos(fixture.targets({"new"}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 1);
    }
    void newLinkRetryPreservesVariantAndSourceIntent() {
        Fixture fixture;
        fixture.changed_target_draft = true;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.linkToPhotos(fixture.targets({"new"}), true);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.failed(), 1);
        QCOMPARE(fixture.writes.load(), 0);
        fixture.changed_target_draft = false;
        auto& state = fixture.states[QStringLiteral("new")];
        const auto original_variant = state.active_variant_id;
        const auto original_head = state.base_commit_id;
        auto retry_without_write = [&] {
            controller.retryFailed();
            QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
            QCOMPARE(fixture.writes.load(), 0);
            QCOMPARE(controller.failed(), 1);
            QCOMPARE(fixture.publications.load(), 0);
        };
        state.active_variant_id = QStringLiteral("another-variant-with-same-head");
        retry_without_write();
        QCOMPARE(state.base_commit_id, original_head);
        state.active_variant_id = original_variant;
        fixture.representation_overrides.insert(QStringLiteral("new"), QStringLiteral("rebound"));
        retry_without_write();
        fixture.representation_overrides.clear();
        fixture.byte_len = 112;
        retry_without_write();
        fixture.byte_len = 111;
        fixture.has_modified_at = false;
        retry_without_write();
        fixture.has_modified_at = true;
        fixture.modified_at = 334;
        retry_without_write();
        fixture.modified_at = 333;
        state.source_path = QStringLiteral("/isolated/rebound.dng");
        retry_without_write();
        state.source_path = QStringLiteral("/isolated/new.dng");
        state.base_commit_id = QStringLiteral("same-variant-new-working-head");
        controller.retryFailed();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.failed(), 0);
        QCOMPARE(fixture.writes.load(), 1);
        QCOMPARE(fixture.publications.load(), 0);
        QCOMPARE(fixture.copies.load(), 1);
        QCOMPARE(fixture.read(QStringLiteral("new")).grade_stack.grade_nodes.size(), 2);
    }
    void newLinkCancellationRetainsAnAcquiredTargetIdentity() {
        Fixture fixture;
        auto operations = fixture.operations();
        const auto prepare = operations.prepare;
        std::atomic_bool acquired{false};
        bool first = true;
        operations.prepare = [&](const auto& shared,
                                 const auto& clipboard,
                                 const auto& target,
                                 const auto& cancellation) {
            auto result = prepare(shared, clipboard, target, cancellation);
            if (first) {
                first = false;
                acquired = result.frozen_before.has_value();
                while (!cancellation->cancelled())
                    QThread::msleep(1);
            }
            return result;
        };
        SharedGradeLinkController controller(operations, fixture.editor());
        controller.linkToPhotos(fixture.targets({"new"}));
        QTRY_VERIFY_WITH_TIMEOUT(acquired.load(), 5000);
        controller.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        fixture.states[QStringLiteral("new")].active_variant_id = QStringLiteral("another-variant");
        controller.retryFailed();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 0);
        QCOMPARE(controller.failed(), 1);
    }
    void newLinkRetryCapturesIdentityOnlyWhenMetadataWasAcquired() {
        Fixture fixture;
        auto operations = fixture.operations();
        const auto prepare = operations.prepare;
        bool first = true;
        operations.prepare = [&](const auto& shared,
                                 const auto& clipboard,
                                 const auto& target,
                                 const auto& cancellation) {
            if (std::exchange(first, false))
                return SharedGradeLinkPrepared{};
            return prepare(shared, clipboard, target, cancellation);
        };
        SharedGradeLinkController controller(operations, fixture.editor());
        controller.linkToPhotos(fixture.targets({"new"}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.failed(), 1);
        fixture.states[QStringLiteral("new")].active_variant_id =
            QStringLiteral("first-loaded-variant");
        fixture.representation_overrides.insert(
            QStringLiteral("new"),
            QStringLiteral("first-loaded-source")
        );
        controller.retryFailed();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 1);
        QCOMPARE(controller.failed(), 0);
        QCOMPARE(fixture.publications.load(), 0);
    }
    void currentPhotoMultipleLayersContinueOnlyFromThisOperationsAcknowledgedHead() {
        Fixture fixture;
        auto& stack = fixture.states[QStringLiteral("source")].grade_stack;
        stack.grade_nodes.first().shared_revision_id = QStringLiteral("revision-0");
        auto second = fixture.shared;
        second.layer_id = QStringLiteral("second-layer");
        second.revision_id = QStringLiteral("second-revision-2");
        second.revision_number = 2;
        fixture.extra_heads = {second};
        auto node = stack.grade_nodes.first();
        node.grade_node_id = QStringLiteral("second-instance");
        node.shared_layer_id = second.layer_id;
        node.shared_revision_id = QStringLiteral("second-revision-1");
        stack.grade_nodes.push_back(node);
        fixture.current_photo = QStringLiteral("source");
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.refreshCurrentLinks();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 2);
        QCOMPARE(controller.failed(), 0);
        const auto saved = fixture.read(QStringLiteral("source"));
        QCOMPARE(saved.grade_stack.grade_nodes[0].shared_revision_id, fixture.shared.revision_id);
        QCOMPARE(saved.grade_stack.grade_nodes[1].shared_revision_id, second.revision_id);
    }
    void offlineSubscriptionDoesNotBlockOtherPhotosInSamePage() {
        Fixture fixture;
        fixture.offline_a = true;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.publish(QStringLiteral("Tone"), {});
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.failed(), 1);
        QCOMPARE(fixture.writes_by_photo.value(QStringLiteral("a")), 0);
        QCOMPARE(fixture.writes_by_photo.value(QStringLiteral("b")), 1);
    }
    void failedExistingSubscriptionCannotBecomeNewLinkAfterUnlink() {
        Fixture fixture;
        fixture.failures.insert(QStringLiteral("b"), BackendSharedGradeLinkStatus::Failed);
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.publish(QStringLiteral("Tone"), fixture.targets({"new"}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        auto& node = fixture.states[QStringLiteral("b")].grade_stack.grade_nodes.first();
        node.shared_layer_id.clear();
        node.shared_revision_id.clear();
        node.shared_follow_head = false;
        fixture.failures.clear();
        controller.retryFailed();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes_by_photo.value(QStringLiteral("b")), 0);
        QCOMPARE(fixture.read(QStringLiteral("b")).grade_stack.grade_nodes.size(), 1);
        QCOMPARE(controller.failed(), 1);
    }
    void selectedRetryRetainsSubsetAndRejectsVariantOrFingerprintRedirect() {
        Fixture fixture;
        auto& stack = fixture.states[QStringLiteral("source")].grade_stack;
        stack.grade_nodes.first().shared_revision_id = QStringLiteral("revision-0");
        auto unselected = stack.grade_nodes.first();
        unselected.grade_node_id = QStringLiteral("unselected-instance");
        unselected.basic.exposure_stops = 9;
        stack.grade_nodes.push_back(unselected);
        fixture.failures.insert(QStringLiteral("source"), BackendSharedGradeLinkStatus::Failed);
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.syncSelected();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        fixture.failures.clear();
        controller.retryFailed();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 1);
        QCOMPARE(fixture.prepared_subscription.grade_node_ids.size(), 1);
        QCOMPARE(
            fixture.read(QStringLiteral("source")).grade_stack.grade_nodes[1].basic.exposure_stops,
            9.0
        );

        fixture.states[QStringLiteral("source")]
            .grade_stack.grade_nodes.first()
            .shared_revision_id = QStringLiteral("revision-0");
        fixture.failures.insert(QStringLiteral("source"), BackendSharedGradeLinkStatus::Failed);
        controller.syncSelected();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        fixture.failures.clear();
        fixture.states[QStringLiteral("source")].active_variant_id =
            QStringLiteral("another-variant");
        controller.retryFailed();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 1);
        QCOMPARE(controller.failed(), 1);
        fixture.states[QStringLiteral("source")].active_variant_id =
            QStringLiteral("variant-source");
        fixture.byte_len = 112;
        controller.retryFailed();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes.load(), 1);
        QVERIFY(
            controller.results().last().toMap().value("message").toString().contains("unavailable")
        );
    }
    void cachedFailureMessageRetranslatesAndSharedNameUsesLibraryLabel() {
        Fixture fixture;
        fixture.states[QStringLiteral("source")].grade_stack.grade_nodes.first().label =
            QStringLiteral("Basic node");
        fixture.shared.label = QStringLiteral("Authored shared name");
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.requestPublication();
        QCOMPARE(controller.nodeName(), fixture.shared.label);
        controller.publish({}, {});
        const auto before = controller.statusText();
        ProductTranslator translator;
        QVERIFY(QCoreApplication::installTranslator(&translator));
        controller.retranslateUi();
        QCOMPARE(controller.statusText(), QStringLiteral("translated: ") + before);
        QCoreApplication::removeTranslator(&translator);
    }
    void nativeSharedParameterHintIgnoresInstanceAndLutPresentationMetadata() {
        BackendGradeNode left, right;
        left.opacity = 0.2;
        left.enabled = false;
        left.fine.lut_managed_path = QStringLiteral("/old-profile/lut.cube");
        right.fine.lut_managed_path = QStringLiteral("/restored-profile/lut.cube");
        left.fine.lut_title = QStringLiteral("Old display name");
        right.fine.lut_title = QStringLiteral("New display name");
        QVERIFY(!sharedGradeParametersDiffer(left, right));
        right.fine.lut_intensity = 0.8;
        QVERIFY(sharedGradeParametersDiffer(left, right));
    }
    void sourceOtherFollowingInstancesParticipateAfterSelectedPublication() {
        Fixture fixture;
        fixture.source_extra_follow = true;
        auto& stack = fixture.states[QStringLiteral("source")].grade_stack;
        auto other = stack.grade_nodes.first();
        other.grade_node_id = QStringLiteral("source-other-instance");
        stack.grade_nodes.push_back(other);
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.publish(QStringLiteral("Tone"), {});
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(fixture.writes_by_photo.value(QStringLiteral("source")), 1);
        const auto saved = fixture.read(QStringLiteral("source"));
        QCOMPARE(saved.grade_stack.grade_nodes[0].shared_revision_id, fixture.shared.revision_id);
        QCOMPARE(saved.grade_stack.grade_nodes[1].shared_revision_id, fixture.shared.revision_id);
        QCOMPARE(controller.completed(), 3); // publication is a separate event
    }
    void alreadyCurrentMaskedLinkDoesNotNeedAnInferenceRuntime() {
        Fixture fixture;
        fixture.semantic_clipboard = true;
        fixture.masks_allowed = false;
        SharedGradeLinkController controller(fixture.operations(), fixture.editor());
        controller.linkToPhotos(fixture.targets({"a"}));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
        QCOMPARE(controller.failed(), 0);
        QCOMPARE(fixture.writes.load(), 0);
        QCOMPARE(controller.completed(), 1);
    }
};

QTEST_GUILESS_MAIN(SharedGradeLinkControllerTest)
#include "shared_grade_link_controller_test.moc"
