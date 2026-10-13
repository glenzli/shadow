#include "adjustment_transfer_controller.hpp"

#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace {
struct Fixture {
    std::mutex mutex;
    QHash<QString, BackendPhotoEditState> states;
    std::atomic_int writes{0}, prepares{0};
    std::atomic_bool hold_prepare{false}, hold_write{false}, entered_write{false};
    std::atomic_bool hold_preview{false}, entered_preview{false};
    std::atomic_int previews{0};
    bool foreground_busy = false;
    bool changed_draft = false;
    bool busy_editor = false;
    bool entered_reserve = false;
    bool semantic_clipboard = false;
    bool masks_allowed = true;
    std::atomic_int retained_plans{0};
    bool reserved = false;
    bool released = false;
    QString fail_photo;
    QString capture_photo = QStringLiteral("source");

    Fixture() {
        for (const auto* id : {"source", "a", "b", "c"}) {
            BackendPhotoEditState state;
            state.photo_id = id;
            state.source_path = QStringLiteral("/isolated/%1.dng").arg(id);
            state.base_commit_id = QStringLiteral("head-%1").arg(id);
            state.active_variant_id = QStringLiteral("variant-%1").arg(id);
            BackendGradeNode node;
            node.grade_node_id = QStringLiteral("node-%1").arg(id);
            node.opacity = 0.35;
            node.enabled = false;
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
    void change(const QString& id) {
        const std::scoped_lock lock(mutex);
        states[id].base_commit_id += QStringLiteral("-newer");
        states[id].grade_stack.geometry.crop_left = 0.3;
    }
    QVariantList targets(std::initializer_list<const char*> ids) {
        QVariantList result;
        for (const auto* id : ids)
            result.push_back(
                QVariantMap{
                    {"photoId", id},
                    {"sourcePath", QStringLiteral("/isolated/%1.dng").arg(id)}
                }
            );
        return result;
    }
    AdjustmentTransferOperations operations() {
        return {
            [this](const AdjustmentTransferSource& source) {
                return AdjustmentTransferClipboard{
                    QByteArray("document"),
                    source.photo_id,
                    source.name,
                    1,
                    source.allow_spatial_masks,
                    semantic_clipboard
                };
            },
            [this](const BackendBatchPhotoTarget& target) { return read(target.photo_id); },
            [this](
                const auto& clipboard,
                const BackendBatchPhotoTarget& target,
                const auto& cancellation
            ) {
                ++prepares;
                while (hold_prepare && !cancellation->cancelled())
                    QThread::msleep(1);
                AdjustmentTransferPrepared result;
                result.before = read(target.photo_id);
                result.after = result.before.grade_stack;
                result.content_anchor = result.before.base_commit_id;
                if (cancellation->cancelled()) {
                    result.failure.issue = AdjustmentTransferIssue::Cancelled;
                } else if (target.photo_id == fail_photo) {
                    result.failure.issue = AdjustmentTransferIssue::NotFound;
                } else {
                    auto copied = read(clipboard.source_photo_id).grade_stack.grade_nodes.first();
                    copied.grade_node_id = QStringLiteral("copy-%1").arg(target.photo_id);
                    result.after.grade_nodes.push_back(copied);
                    ++retained_plans;
                    result.retained_plan = std::shared_ptr<int>(new int(1), [this](int* value) {
                        delete value;
                        --retained_plans;
                    });
                }
                return result;
            },
            [this](const AdjustmentTransferPrepared& prepared) {
                entered_write = true;
                while (hold_write)
                    QThread::msleep(1);
                const std::scoped_lock lock(mutex);
                auto& state = states[prepared.before.photo_id];
                if (state.base_commit_id != prepared.before.base_commit_id
                    || state.active_variant_id != prepared.before.active_variant_id)
                    throw std::runtime_error("newer working head");
                state.grade_stack = prepared.after;
                state.base_commit_id = QStringLiteral("saved-%1").arg(++writes);
                return state;
            },
            [this](const auto&, const auto& cancellation) {
                ++previews;
                entered_preview = true;
                while (hold_preview && !cancellation->cancelled())
                    QThread::msleep(1);
                return !cancellation->cancelled();
            }
        };
    }
    AdjustmentTransferEditor editor() {
        return {
            [this](bool) {
                const auto state = read(capture_photo);
                return AdjustmentTransferSource{
                    state.photo_id,
                    state.source_path,
                    state.base_commit_id,
                    QStringLiteral("Source"),
                    state.grade_stack,
                    false
                };
            },
            [] { return BackendBatchPhotoTarget{}; },
            [this](const auto&) {
                entered_reserve = true;
                if (busy_editor)
                    return AdjustmentTransferAdmission::Busy;
                if (changed_draft)
                    return AdjustmentTransferAdmission::Changed;
                reserved = true;
                return AdjustmentTransferAdmission::Ready;
            },
            [](const auto&, const auto&) {},
            [this] {
                reserved = false;
                released = true;
            },
            [this] { return masks_allowed; },
            [this] { return foreground_busy; }
        };
    }
};
} // namespace

class AdjustmentTransferTest final : public QObject {
    Q_OBJECT
  private slots:
    void append_preserves_private_edits_and_strength_and_undo() {
        Fixture fixture;
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        QVERIFY(controller.hasClipboard());
        controller.applyToPhotos(fixture.targets({"source", "a", "a", "b"}));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.total(), 2);
        QCOMPARE(controller.completed(), 2);
        QCOMPARE(fixture.writes.load(), 2);
        auto state = fixture.read(QStringLiteral("a"));
        QCOMPARE(state.grade_stack.grade_nodes.size(), 2);
        QCOMPARE(state.grade_stack.grade_nodes.last().opacity, 0.35);
        QVERIFY(!state.grade_stack.grade_nodes.last().enabled);
        QCOMPARE(state.grade_stack.geometry.crop_left, 0.2);
        QCOMPARE(state.grade_stack.foundation.temperature_kelvin, 7200U);
        QVERIFY(controller.canUndo());
        controller.undoBatch();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.read(QStringLiteral("a")).grade_stack.grade_nodes.size(), 1);
        QCOMPARE(fixture.read(QStringLiteral("b")).grade_stack.grade_nodes.size(), 1);
        QVERIFY(!controller.canUndo());
        QVERIFY(fixture.released && !fixture.reserved);
    }
    void failed_photo_is_unchanged_and_retry_retains_batch_undo() {
        Fixture fixture;
        fixture.fail_photo = QStringLiteral("b");
        const auto original = fixture.read(QStringLiteral("b"));
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a", "b"}));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.completed(), 1);
        QCOMPARE(controller.failed(), 1);
        QCOMPARE(fixture.read(QStringLiteral("b")).base_commit_id, original.base_commit_id);
        QVERIFY(controller.canRetry());
        fixture.fail_photo.clear();
        controller.retryFailed();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.completed(), 2);
        QCOMPARE(fixture.prepares.load(), 3);
        controller.undoBatch();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.read(QStringLiteral("a")).grade_stack.grade_nodes.size(), 1);
        QCOMPARE(fixture.read(QStringLiteral("b")).grade_stack.grade_nodes.size(), 1);
    }
    void cancel_inference_never_publishes_and_retry_is_explicit() {
        Fixture fixture;
        fixture.hold_prepare = true;
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a", "b"}));
        QTRY_VERIFY(fixture.prepares.load() == 1);
        controller.cancel();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.writes.load(), 0);
        QCOMPARE(controller.failed(), 2);
        fixture.hold_prepare = false;
        controller.retryFailed();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.completed(), 2);
    }
    void repeat_paste_does_not_duplicate_or_overwrite_newer_edits() {
        Fixture fixture;
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a"}));
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a"}));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.writes.load(), 1);
        QCOMPARE(
            controller.results().first().toMap().value("state").toString(),
            QStringLiteral("unchanged")
        );
        QVERIFY(controller.canUndo());
        fixture.change(QStringLiteral("a"));
        controller.applyToPhotos(fixture.targets({"a"}));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.failed(), 1);
        QCOMPARE(fixture.writes.load(), 1);
        QCOMPARE(fixture.read(QStringLiteral("a")).grade_stack.geometry.crop_left, 0.3);
    }
    void newer_open_draft_and_newer_undo_head_are_protected() {
        Fixture fixture;
        fixture.changed_draft = true;
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a"}));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.writes.load(), 0);
        fixture.changed_draft = false;
        controller.retryFailed();
        QTRY_VERIFY(!controller.busy());
        fixture.change(QStringLiteral("a"));
        controller.undoBatch();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.failed(), 1);
        QCOMPARE(fixture.read(QStringLiteral("a")).grade_stack.grade_nodes.size(), 2);
        QCOMPARE(fixture.read(QStringLiteral("a")).grade_stack.geometry.crop_left, 0.3);
    }
    void cancel_during_commit_keeps_acknowledged_receipt() {
        Fixture fixture;
        fixture.hold_write = true;
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a", "b"}));
        QTRY_VERIFY(fixture.entered_write.load());
        controller.cancel();
        fixture.hold_write = false;
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.completed(), 1);
        QCOMPARE(controller.failed(), 1);
        QVERIFY(controller.canUndo());
        controller.undoBatch();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.read(QStringLiteral("a")).grade_stack.grade_nodes.size(), 1);
    }
    void cancellation_before_job_registration_still_cancels() {
        AdjustmentTransferCancellation cancellation;
        int calls = 0;
        cancellation.cancel();
        cancellation.setActive([&] { ++calls; });
        QCOMPARE(calls, 1);
        cancellation.clearActive();
        cancellation.cancel();
        QCOMPARE(calls, 1);
    }
    void cancel_prepared_photo_releases_plan_without_another_prepare() {
        Fixture fixture;
        fixture.busy_editor = true;
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a"}));
        QTRY_VERIFY(fixture.entered_reserve);
        QCOMPARE(fixture.retained_plans.load(), 1);
        controller.cancel();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.retained_plans.load(), 0);
        QCOMPARE(fixture.writes.load(), 0);
    }
    void semantic_application_respects_current_ai_permission() {
        Fixture fixture;
        fixture.semantic_clipboard = true;
        fixture.masks_allowed = false;
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a"}));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.prepares.load(), 0);
        QCOMPARE(fixture.writes.load(), 0);
        QCOMPARE(controller.failed(), 1);
        fixture.masks_allowed = true;
        fixture.busy_editor = true;
        controller.retryFailed();
        QTRY_VERIFY(fixture.entered_reserve);
        fixture.masks_allowed = false;
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.retained_plans.load(), 0);
        QCOMPARE(fixture.writes.load(), 0);
        fixture.busy_editor = false;
        fixture.masks_allowed = true;
        controller.retryFailed();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.completed(), 1);
        QCOMPARE(fixture.writes.load(), 1);
    }
    void profile_quiescence_cancels_jobs_and_blocks_new_admission() {
        Fixture fixture;
        fixture.hold_prepare = true;
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a", "b"}));
        QTRY_COMPARE(fixture.prepares.load(), 1);
        controller.setProfileSwitchPending(true);
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.writes.load(), 0);
        QVERIFY(!controller.admissionEnabled());
        QVERIFY(!controller.canRetry());
        controller.applyToPhotos(fixture.targets({"c"}));
        controller.copyAdjustments();
        QVERIFY(!controller.busy());
        QCOMPARE(fixture.prepares.load(), 1);
        fixture.hold_prepare = false;
        controller.setProfileSwitchPending(false);
        QVERIFY(controller.canRetry());
        controller.retryFailed();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.completed(), 2);
    }
    void failed_batch_retry_keeps_its_recipe_after_another_copy() {
        Fixture fixture;
        fixture.fail_photo = QStringLiteral("b");
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a", "b"}));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.failed(), 1);
        fixture.capture_photo = QStringLiteral("c");
        {
            const std::scoped_lock lock(fixture.mutex);
            fixture.states["c"].grade_stack.grade_nodes.first().opacity = 0.8;
        }
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        fixture.fail_photo.clear();
        QVERIFY(controller.canRetry());
        controller.retryFailed();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.read(QStringLiteral("b")).grade_stack.grade_nodes.last().opacity, 0.35);
        QCOMPARE(fixture.writes.load(), 2);
        controller.applyToPhotos(fixture.targets({"b"}));
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.read(QStringLiteral("b")).grade_stack.grade_nodes.last().opacity, 0.8);
        QCOMPARE(fixture.writes.load(), 3);
    }
    void preview_cancellation_keeps_the_write_receipt_and_batch_undo() {
        Fixture fixture;
        fixture.hold_preview = true;
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a", "b"}));
        QTRY_VERIFY(fixture.entered_preview.load());
        QCOMPARE(fixture.writes.load(), 1);
        QVERIFY(!fixture.reserved);
        controller.cancel();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.completed(), 1);
        QCOMPARE(controller.failed(), 1);
        QVERIFY(controller.canUndo());
        fixture.hold_preview = false;
        controller.undoBatch();
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(fixture.read(QStringLiteral("a")).grade_stack.grade_nodes.size(), 1);
    }
    void foreground_render_preempts_background_preview_without_cancelling_batch() {
        Fixture fixture;
        fixture.hold_preview = true;
        AdjustmentTransferController controller(fixture.operations(), fixture.editor());
        controller.copyAdjustments();
        QTRY_VERIFY(!controller.busy());
        controller.applyToPhotos(fixture.targets({"a", "b"}));
        QTRY_VERIFY(fixture.entered_preview.load());
        fixture.foreground_busy = true;
        controller.foregroundWorkChanged();
        QTest::qWait(80);
        QCOMPARE(fixture.previews.load(), 1);
        QCOMPARE(fixture.writes.load(), 1);
        fixture.hold_preview = false;
        fixture.foreground_busy = false;
        QTRY_VERIFY(!controller.busy());
        QCOMPARE(controller.completed(), 2);
        QCOMPARE(controller.failed(), 0);
        QCOMPARE(fixture.writes.load(), 2);
        QCOMPARE(fixture.previews.load(), 3);
    }
};
QTEST_GUILESS_MAIN(AdjustmentTransferTest)
#include "adjustment_transfer_controller_test.moc"
