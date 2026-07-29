#include "edit_preview_presentation_context.hpp"
#include "edit_preview_presentation_registry.hpp"
#include "edit_preview_provider.hpp"
#include "edit_preview_texture_item.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQuickWindow>
#include <QRectF>
#include <QString>
#include <QThread>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "edit preview texture item failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] QString preview_source(const quint64 generation) {
    return QStringLiteral("image://shadow-edit/current?generation=%1").arg(generation);
}

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate&& predicate, const qint64 timeout_ms = 5'000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1U);
    }
    return predicate();
}

class TestFrame final : public BackendEditPreviewFrame {
  public:
    TestFrame() : rgb8_(4U * 2U * 3U, 127U) {}

    [[nodiscard]] QSize dimensions() const noexcept override {
        return {4, 2};
    }

    [[nodiscard]] std::size_t rowStrideBytes() const noexcept override {
        return 12U;
    }

    [[nodiscard]] BackendEditPreviewStorage storageKind() const noexcept override {
        return BackendEditPreviewStorage::HostRgb8;
    }

    [[nodiscard]] std::optional<BackendAppleMetalPreviewTexture>
    appleMetalTexture() const noexcept override {
        return std::nullopt;
    }

    [[nodiscard]] std::size_t materializedPixelBytes() const noexcept override {
        return rgb8_.size();
    }

    [[nodiscard]] std::span<const std::uint8_t> materializeRgb8() const override {
        materializations_.fetch_add(1U, std::memory_order_relaxed);
        if (materialization_failure_.load(std::memory_order_relaxed)) {
            return {};
        }
        return rgb8_;
    }

    [[nodiscard]] std::optional<BackendEditMaskCoverageView>
    maskCoverage() const noexcept override {
        return std::nullopt;
    }

    [[nodiscard]] std::string presentationFallbackDiagnostic() const override {
        return {};
    }

    [[nodiscard]] std::uint64_t retainedBytes() const noexcept override {
        return rgb8_.size();
    }

    [[nodiscard]] std::uint32_t materializations() const noexcept {
        return materializations_.load(std::memory_order_relaxed);
    }

    void setMaterializationFailure(const bool fail) noexcept {
        materialization_failure_.store(fail, std::memory_order_relaxed);
    }

  private:
    std::vector<std::uint8_t> rgb8_;
    mutable std::atomic_uint32_t materializations_{0U};
    std::atomic_bool materialization_failure_{false};
};

class TestableEditPreviewTextureItem final : public EditPreviewTextureItem {
  public:
    using EditPreviewTextureItem::EditPreviewTextureItem;

    void releaseSceneGraphResources() {
        releaseResources();
    }
};

class TestSceneGraphWindow final : public QQuickWindow {
  public:
    void invalidateSceneGraphFromRenderThreadHarness() {
        emit sceneGraphInvalidated();
    }
};

void source_admission_preserves_settled_fallback() {
    auto store = std::make_shared<EditPreviewStore>();
    auto context = std::make_shared<EditPreviewPresentationContext>();
    EditPreviewPresentationRegistry registry(store, context);
    EditPreviewTextureItem item;
    item.setPresentationRegistry(&registry);

    store->publish(
        EditPreviewSlot::Current,
        QByteArrayLiteral("encoded-settled"),
        QSize(4, 2),
        0,
        {},
        1U
    );
    item.setSource(preview_source(1U));
    require(
        !item.liveFrameAvailable() && item.fallbackSource() == preview_source(1U),
        "a settled encoded generation must remain on QML Image"
    );

    const auto frame = std::make_shared<TestFrame>();
    store->publish(
        EditPreviewSlot::Current,
        {},
        QSize(4, 2),
        12,
        {},
        2U,
        frame,
        EditPreviewPresentationBinding{
            .epoch = 7U,
            .window_identity = 11U,
            .api = EditPreviewPresentationApi::Metal,
            .device_handle = 13U,
            .initialized = true,
        }
    );
    item.setSource(preview_source(2U));
    require(
        item.liveFrameAvailable() && item.fallbackSource() == preview_source(1U)
            && item.presentedGeneration().isEmpty(),
        "a live generation must retain the last settled Image beneath it"
    );
    require(
        frame->materializations() == 0U,
        "GUI-thread source admission must inspect descriptors only"
    );

    item.setSource(preview_source(99U));
    require(
        !item.liveFrameAvailable() && item.fallbackSource() == preview_source(1U),
        "a stale generation must not replace the last admitted fallback"
    );

    store->publish(
        EditPreviewSlot::Current,
        QByteArrayLiteral("next-settled"),
        QSize(4, 2),
        0,
        {},
        3U
    );
    item.setSource(preview_source(3U));
    require(
        !item.liveFrameAvailable() && item.fallbackSource() == preview_source(3U),
        "the next settled generation must replace the retained fallback"
    );
}

void live_admission_roundtrip_preserves_last_settled_fallback() {
    auto store = std::make_shared<EditPreviewStore>();
    auto context = std::make_shared<EditPreviewPresentationContext>();
    EditPreviewPresentationRegistry registry(store, context);
    QQuickWindow window;
    window.resize(160, 120);
    EditPreviewTextureItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(160.0);
    item.setHeight(120.0);
    item.setPresentationRegistry(&registry);

    store->publish(
        EditPreviewSlot::Current,
        QByteArrayLiteral("encoded-settled"),
        QSize(4, 2),
        0,
        {},
        5U
    );
    item.setSource(preview_source(5U));
    require(
        item.fallbackSource() == preview_source(5U) && !item.liveFrameAvailable(),
        "the roundtrip must start from one settled fallback"
    );

    const auto live_frame = std::make_shared<TestFrame>();
    store->publish(EditPreviewSlot::Current, {}, QSize(4, 2), 12, {}, 6U, live_frame);
    item.setSource(preview_source(6U));
    require(
        item.liveFrameAvailable() && item.fallbackSource() == preview_source(5U),
        "normal mode must admit the live frame above the settled fallback"
    );

    window.show();
    window.requestUpdate();
    require(
        wait_until([&] {
            return live_frame->materializations() == 1U
                   && item.presentedGeneration() == QStringLiteral("6");
        }),
        "the normal canvas must present the live frame before the dual roundtrip"
    );

    item.setVisible(false);
    item.setLiveAdmissionEnabled(false);
    require(
        !item.liveFrameAvailable() && item.fallbackSource() == preview_source(5U)
            && item.presentedGeneration().isEmpty(),
        "entering dual mode must revoke live admission without blanking the settled fallback"
    );
    require(
        live_frame->materializations() == 1U,
        "a suspended hidden item must not repeat its existing materialization"
    );

    item.setLiveAdmissionEnabled(true);
    require(
        item.liveFrameAvailable() && item.fallbackSource() == preview_source(5U)
            && item.presentedGeneration().isEmpty(),
        "leaving dual mode must wait for a fresh import above the retained fallback"
    );
    item.setVisible(true);
    window.requestUpdate();
    require(
        wait_until([&] {
            return live_frame->materializations() == 2U
                   && item.presentedGeneration() == QStringLiteral("6");
        }),
        "a hidden same-generation node must not satisfy the new live-admission cycle"
    );

    item.setVisible(false);
    item.setLiveAdmissionEnabled(false);
    const auto replacement_frame = std::make_shared<TestFrame>();
    store->publish(EditPreviewSlot::Current, {}, QSize(4, 2), 12, {}, 7U, replacement_frame);
    item.setSource(preview_source(7U));
    require(
        !item.liveFrameAvailable() && item.fallbackSource() == preview_source(5U)
            && replacement_frame->materializations() == 0U,
        "a live generation arriving during dual mode must remain unadmitted "
        "and preserve the last settled fallback"
    );

    item.setLiveAdmissionEnabled(true);
    require(
        item.liveFrameAvailable() && item.fallbackSource() == preview_source(5U)
            && item.presentedGeneration().isEmpty(),
        "leaving dual mode must wait for a fresh import above the retained fallback"
    );
    item.setLiveAdmissionEnabled(false);
    require(
        !item.liveFrameAvailable() && item.fallbackSource() == preview_source(5U),
        "a repeated admission failure or suspension must still leave a non-empty fallback"
    );

    item.setParentItem(nullptr);
    window.hide();
    window.releaseResources();
}

void content_rect_preserves_geometry_contract() {
    auto store = std::make_shared<EditPreviewStore>();
    auto context = std::make_shared<EditPreviewPresentationContext>();
    EditPreviewPresentationRegistry registry(store, context);
    const auto frame = std::make_shared<TestFrame>();
    store->publish(EditPreviewSlot::Current, {}, QSize(4, 2), 12, {}, 4U, frame);
    EditPreviewTextureItem item;
    item.setPresentationRegistry(&registry);
    item.setWidth(100.0);
    item.setHeight(100.0);
    item.setFillMode(EditPreviewTextureItem::FillMode::PreserveAspectFit);
    item.setSource(preview_source(4U));
    require(
        item.contentRect() == QRectF(0.0, 25.0, 100.0, 50.0),
        "aspect-fit content geometry must remain explicit for pointer mapping"
    );
    item.setFillMode(EditPreviewTextureItem::FillMode::Stretch);
    require(
        item.contentRect() == QRectF(0.0, 0.0, 100.0, 100.0),
        "the main Precision canvas stretch contract must fill its surface"
    );
}

void registry_is_explicit_reconfigurable_and_lifetime_safe() {
    auto first_store = std::make_shared<EditPreviewStore>();
    auto first_context = std::make_shared<EditPreviewPresentationContext>();
    const std::weak_ptr<EditPreviewStore> first_store_lifetime = first_store;
    const std::weak_ptr<EditPreviewPresentationContext> first_context_lifetime = first_context;
    first_store->publish(
        EditPreviewSlot::Current,
        {},
        QSize(4, 2),
        12,
        {},
        8U,
        std::make_shared<TestFrame>()
    );

    EditPreviewPresentationRegistry registry;
    EditPreviewTextureItem first_item;
    EditPreviewTextureItem second_item;
    first_item.setSource(preview_source(8U));
    second_item.setSource(preview_source(8U));
    require(
        !first_item.liveFrameAvailable() && !second_item.liveFrameAvailable(),
        "items without an explicit registry must fail closed"
    );

    first_item.setPresentationRegistry(&registry);
    second_item.setPresentationRegistry(&registry);
    registry.configure(first_store, first_context);
    require(
        first_item.liveFrameAvailable() && second_item.liveFrameAvailable(),
        "one explicit composition owner must refresh every bound item"
    );

    first_store.reset();
    first_context.reset();
    require(
        !first_store_lifetime.expired() && !first_context_lifetime.expired(),
        "the registry must retain its configured services"
    );
    registry.clear();
    require(
        !first_item.liveFrameAvailable() && !second_item.liveFrameAvailable()
            && first_store_lifetime.expired() && first_context_lifetime.expired(),
        "clearing the registry must synchronously fail closed and release "
        "the old composition"
    );

    auto replacement_store = std::make_shared<EditPreviewStore>();
    auto replacement_context = std::make_shared<EditPreviewPresentationContext>();
    replacement_store->publish(
        EditPreviewSlot::Current,
        {},
        QSize(4, 2),
        12,
        {},
        8U,
        std::make_shared<TestFrame>()
    );
    registry.configure(replacement_store, replacement_context);
    require(
        first_item.liveFrameAvailable() && second_item.liveFrameAvailable(),
        "reconfiguring the same registry must refresh existing items"
    );

    EditPreviewTextureItem destruction_item;
    auto destruction_store = std::make_shared<EditPreviewStore>();
    auto destruction_context = std::make_shared<EditPreviewPresentationContext>();
    const std::weak_ptr<EditPreviewStore> destruction_store_lifetime = destruction_store;
    const std::weak_ptr<EditPreviewPresentationContext> destruction_context_lifetime =
        destruction_context;
    destruction_store->publish(
        EditPreviewSlot::Current,
        {},
        QSize(4, 2),
        12,
        {},
        9U,
        std::make_shared<TestFrame>()
    );
    auto destruction_registry =
        std::make_unique<EditPreviewPresentationRegistry>(destruction_store, destruction_context);
    destruction_item.setPresentationRegistry(destruction_registry.get());
    destruction_item.setSource(preview_source(9U));
    destruction_store.reset();
    destruction_context.reset();
    require(
        destruction_item.liveFrameAvailable(),
        "a live item must accept the explicitly owned composition"
    );
    destruction_registry.reset();
    require(
        destruction_item.presentationRegistry() == nullptr && !destruction_item.liveFrameAvailable()
            && destruction_store_lifetime.expired() && destruction_context_lifetime.expired(),
        "destroying the composition owner before its item must clear the "
        "binding without a dangling service locator"
    );
}

void rendered_node_replaces_same_source_frame_after_registry_reconfiguration() {
    auto first_store = std::make_shared<EditPreviewStore>();
    auto first_context = std::make_shared<EditPreviewPresentationContext>();
    auto first_frame = std::make_shared<TestFrame>();
    constexpr quint64 generation = 12U;
    first_store
        ->publish(EditPreviewSlot::Current, {}, QSize(4, 2), 12, {}, generation, first_frame);
    EditPreviewPresentationRegistry registry(first_store, first_context);

    QQuickWindow window;
    window.resize(160, 120);
    EditPreviewTextureItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(160.0);
    item.setHeight(120.0);
    item.setPresentationRegistry(&registry);
    item.setSource(preview_source(generation));
    window.show();
    window.requestUpdate();

    require(
        wait_until([&] {
            return first_frame->materializations() == 1U
                   && item.presentedGeneration() == QString::number(generation);
        }),
        "the first admitted frame must create a real software QSG texture and publish readiness"
    );

    auto replacement_store = std::make_shared<EditPreviewStore>();
    auto replacement_context = std::make_shared<EditPreviewPresentationContext>();
    auto replacement_frame = std::make_shared<TestFrame>();
    replacement_store
        ->publish(EditPreviewSlot::Current, {}, QSize(4, 2), 12, {}, generation, replacement_frame);

    // Keep the URL and generation unchanged and perform clear/configure before
    // the render thread receives another synchronization point. The node cache
    // must still distinguish the replacement owner/configuration.
    registry.clear();
    registry.configure(replacement_store, replacement_context);
    require(
        item.liveFrameAvailable() && item.presentedGeneration().isEmpty()
            && item.source() == preview_source(generation),
        "same-source reconfiguration must synchronously revoke old readiness"
    );
    window.requestUpdate();
    require(
        wait_until([&] {
            return replacement_frame->materializations() == 1U
                   && item.presentedGeneration() == QString::number(generation);
        }),
        "the next render sync must destroy the stale node texture, materialize "
        "the replacement frame, and republish readiness"
    );
    require(
        first_frame->materializations() == 1U,
        "same-source replacement must not recreate or reuse the old frame texture"
    );

    item.setParentItem(nullptr);
    window.hide();
    window.releaseResources();
}

void pending_readiness_rejects_same_generation_replacement() {
    auto first_store = std::make_shared<EditPreviewStore>();
    auto first_context = std::make_shared<EditPreviewPresentationContext>();
    auto first_frame = std::make_shared<TestFrame>();
    constexpr quint64 generation = 13U;
    first_store
        ->publish(EditPreviewSlot::Current, {}, QSize(4, 2), 12, {}, generation, first_frame);
    EditPreviewPresentationRegistry registry(first_store, first_context);

    auto replacement_store = std::make_shared<EditPreviewStore>();
    auto replacement_context = std::make_shared<EditPreviewPresentationContext>();
    auto replacement_frame = std::make_shared<TestFrame>();
    replacement_store
        ->publish(EditPreviewSlot::Current, {}, QSize(4, 2), 12, {}, generation, replacement_frame);

    QQuickWindow window;
    window.resize(160, 120);
    EditPreviewTextureItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(160.0);
    item.setHeight(120.0);
    item.setPresentationRegistry(&registry);
    item.setSource(preview_source(generation));

    std::atomic_bool replacement_queued{false};
    std::atomic_bool replacement_applied{false};
    std::atomic_bool stale_callback_rejected{false};
    QObject::connect(
        &window,
        &QQuickWindow::beforeSynchronizing,
        &window,
        [&] {
            if (replacement_queued.exchange(true, std::memory_order_relaxed)) {
                return;
            }
            QMetaObject::invokeMethod(
                &item,
                [&] {
                    window.hide();
                    registry.clear();
                    registry.configure(replacement_store, replacement_context);
                    QMetaObject::invokeMethod(
                        &item,
                        [&] {
                            stale_callback_rejected.store(
                                item.presentedGeneration().isEmpty(),
                                std::memory_order_relaxed
                            );
                            replacement_applied.store(true, std::memory_order_relaxed);
                            window.show();
                            window.requestUpdate();
                        },
                        Qt::QueuedConnection
                    );
                },
                Qt::QueuedConnection
            );
        },
        Qt::DirectConnection
    );

    window.show();
    window.requestUpdate();
    require(
        wait_until([&] {
            return replacement_applied.load(std::memory_order_relaxed)
                   && replacement_frame->materializations() == 1U
                   && item.presentedGeneration() == QString::number(generation);
        }),
        "the replacement frame must render after the pending first-frame callback is drained"
    );
    require(
        stale_callback_rejected.load(std::memory_order_relaxed),
        "a pending readiness callback must validate source, frame owner, "
        "presentation binding, and registry revision before publication"
    );
    require(
        first_frame->materializations() == 1U,
        "the test must exercise a real first-frame texture before replacing its identity"
    );

    item.setParentItem(nullptr);
    window.hide();
    window.releaseResources();
}

void scene_graph_release_revokes_readiness_until_texture_recreation() {
    auto store = std::make_shared<EditPreviewStore>();
    auto context = std::make_shared<EditPreviewPresentationContext>();
    EditPreviewPresentationRegistry registry(store, context);
    constexpr quint64 settled_generation = 14U;
    constexpr quint64 live_generation = 15U;

    TestSceneGraphWindow window;
    window.resize(160, 120);
    TestableEditPreviewTextureItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(160.0);
    item.setHeight(120.0);
    item.setPresentationRegistry(&registry);

    store->publish(
        EditPreviewSlot::Current,
        QByteArrayLiteral("settled-release-fallback"),
        QSize(4, 2),
        0,
        {},
        settled_generation
    );
    item.setSource(preview_source(settled_generation));
    const auto frame = std::make_shared<TestFrame>();
    store->publish(EditPreviewSlot::Current, {}, QSize(4, 2), 12, {}, live_generation, frame);
    item.setSource(preview_source(live_generation));

    std::atomic_bool release_queued{false};
    std::atomic_bool pending_callback_observed{false};
    std::atomic_bool pending_callback_rejected{false};
    QObject::connect(
        &window,
        &QQuickWindow::beforeSynchronizing,
        &window,
        [&] {
            if (release_queued.exchange(true, std::memory_order_relaxed)) {
                return;
            }
            QMetaObject::invokeMethod(
                &item,
                [&] {
                    window.hide();
                    item.releaseSceneGraphResources();
                    QMetaObject::invokeMethod(
                        &item,
                        [&] {
                            pending_callback_rejected.store(
                                item.presentedGeneration().isEmpty(),
                                std::memory_order_relaxed
                            );
                            pending_callback_observed.store(true, std::memory_order_relaxed);
                            window.show();
                            window.requestUpdate();
                        },
                        Qt::QueuedConnection
                    );
                },
                Qt::QueuedConnection
            );
        },
        Qt::DirectConnection
    );

    window.show();
    window.requestUpdate();
    require(
        wait_until([&] {
            return pending_callback_observed.load(std::memory_order_relaxed)
                   && frame->materializations() >= 2U
                   && item.presentedGeneration() == QString::number(live_generation);
        }),
        "the same live generation must become ready only after a new texture "
        "is created following resource release"
    );
    require(
        pending_callback_rejected.load(std::memory_order_relaxed),
        "a texture callback queued before resource release must not publish "
        "readiness after its node is invalid"
    );
    require(
        item.fallbackSource() == preview_source(settled_generation)
            && item.source() == preview_source(live_generation) && item.liveFrameAvailable(),
        "resource release must retain the settled fallback and live frame owner"
    );

    const std::uint32_t before_successful_recreation = frame->materializations();
    item.releaseSceneGraphResources();
    require(
        item.presentedGeneration().isEmpty()
            && item.fallbackSource() == preview_source(settled_generation)
            && item.liveFrameAvailable(),
        "GUI-thread releaseResources must synchronously revoke readiness "
        "without discarding reimport state"
    );
    window.requestUpdate();
    require(
        wait_until([&] {
            return frame->materializations() > before_successful_recreation
                   && item.presentedGeneration() == QString::number(live_generation);
        }),
        "a successful same-generation texture recreation must restore readiness"
    );

    const std::uint32_t before_window_recreation = frame->materializations();
    window.setPersistentGraphics(false);
    window.setPersistentSceneGraph(false);
    window.hide();
    window.releaseResources();
    std::thread invalidation_thread([&window] {
        window.invalidateSceneGraphFromRenderThreadHarness();
    });
    invalidation_thread.join();
    require(
        wait_until([&] { return item.presentedGeneration().isEmpty(); }),
        "the render-thread sceneGraphInvalidated lifecycle must revoke item readiness on the GUI "
        "thread"
    );
    require(
        item.fallbackSource() == preview_source(settled_generation) && item.liveFrameAvailable(),
        "window resource release must retain the fallback and live owner"
    );
    window.show();
    window.requestUpdate();
    require(
        wait_until([&] {
            return frame->materializations() > before_window_recreation
                   && item.presentedGeneration() == QString::number(live_generation);
        }),
        "the item must reimport the same live source after window scene-graph reconstruction"
    );

    frame->setMaterializationFailure(true);
    const std::uint32_t before_failed_recreation = frame->materializations();
    item.releaseSceneGraphResources();
    require(
        item.presentedGeneration().isEmpty(),
        "the next release must revoke the successfully recreated node"
    );
    window.requestUpdate();
    require(
        wait_until([&] { return frame->materializations() > before_failed_recreation; }),
        "the forced materialization failure must reach the real texture factory"
    );
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    require(
        item.presentedGeneration().isEmpty()
            && item.fallbackSource() == preview_source(settled_generation)
            && item.liveFrameAvailable(),
        "a failed texture recreation must remain not-ready above the retained fallback"
    );

    item.setParentItem(nullptr);
    window.hide();
    window.releaseResources();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    QGuiApplication application(argc, argv);
    source_admission_preserves_settled_fallback();
    live_admission_roundtrip_preserves_last_settled_fallback();
    content_rect_preserves_geometry_contract();
    registry_is_explicit_reconfigurable_and_lifetime_safe();
    rendered_node_replaces_same_source_frame_after_registry_reconfiguration();
    pending_readiness_rejects_same_generation_replacement();
    scene_graph_release_revokes_readiness_until_texture_recreation();
    return EXIT_SUCCESS;
}
