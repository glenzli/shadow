#include "edit_preview_presentation_context.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::uintptr_t window_identity(const QQuickWindow* const window) noexcept {
    return reinterpret_cast<std::uintptr_t>(window);
}

[[nodiscard]] EditPreviewPresentationApi
presentation_api(const QSGRendererInterface::GraphicsApi api) noexcept {
    switch (api) {
    case QSGRendererInterface::Software:
        return EditPreviewPresentationApi::Software;
    case QSGRendererInterface::Metal:
        return EditPreviewPresentationApi::Metal;
    case QSGRendererInterface::Unknown:
    case QSGRendererInterface::Null:
        return EditPreviewPresentationApi::Unavailable;
    default:
        return EditPreviewPresentationApi::Other;
    }
}

} // namespace

struct EditPreviewPresentationContext::State final {
    // Use the shared_ptr atomic free functions: the deployment libc++ does
    // not yet expose the C++20 std::atomic<shared_ptr<T>> specialization.
    std::shared_ptr<const EditPreviewPresentationBinding> published =
        std::make_shared<const EditPreviewPresentationBinding>();
    std::atomic<std::uintptr_t> attached_window_identity{0U};
    QPointer<QQuickWindow> attached_window;
    std::vector<QMetaObject::Connection> connections;
};

EditPreviewPresentationContext::EditPreviewPresentationContext(QObject* const parent) :
    QObject(parent), state_(std::make_unique<State>()) {}

EditPreviewPresentationContext::~EditPreviewPresentationContext() {
    state_->attached_window_identity.store(0U, std::memory_order_release);
    for (const auto& connection : state_->connections) {
        QObject::disconnect(connection);
    }
}

void EditPreviewPresentationContext::attach(QQuickWindow* const window) {
    if (state_->attached_window == window) {
        return;
    }
    for (const auto& connection : state_->connections) {
        QObject::disconnect(connection);
    }
    state_->connections.clear();
    state_->attached_window = window;
    const auto identity = window_identity(window);
    state_->attached_window_identity.store(identity, std::memory_order_release);
    publishObservation(identity, EditPreviewPresentationApi::Unavailable, 0U, false);
    if (window == nullptr) {
        return;
    }

    state_->connections.push_back(
        QObject::connect(
            window,
            &QQuickWindow::sceneGraphInitialized,
            this,
            [this, window] { observeWindow(window); },
            Qt::DirectConnection
        )
    );
    state_->connections.push_back(
        QObject::connect(
            window,
            &QQuickWindow::beforeRendering,
            this,
            [this, window] {
                // A context can be attached after the sceneGraphInitialized edge.
                // Re-observation is idempotent within one live tuple.
                observeWindow(window);
            },
            Qt::DirectConnection
        )
    );
    state_->connections.push_back(
        QObject::connect(
            window,
            &QQuickWindow::sceneGraphInvalidated,
            this,
            [this, window] {
                const auto identity = window_identity(window);
                if (state_->attached_window_identity.load(std::memory_order_acquire) != identity) {
                    return;
                }
                publishObservation(identity, EditPreviewPresentationApi::Unavailable, 0U, false);
            },
            Qt::DirectConnection
        )
    );
    state_->connections.push_back(
        QObject::connect(
            window,
            &QObject::destroyed,
            this,
            [this, identity](QObject*) {
                std::uintptr_t expected = identity;
                if (!state_->attached_window_identity
                         .compare_exchange_strong(expected, 0U, std::memory_order_acq_rel)) {
                    return;
                }
                publishObservation(0U, EditPreviewPresentationApi::Unavailable, 0U, false);
            },
            Qt::DirectConnection
        )
    );
}

EditPreviewPresentationBinding EditPreviewPresentationContext::snapshot() const noexcept {
    return *std::atomic_load_explicit(&state_->published, std::memory_order_acquire);
}

void EditPreviewPresentationContext::observeWindow(QQuickWindow* const window) noexcept {
    const auto identity = window_identity(window);
    if (window == nullptr
        || state_->attached_window_identity.load(std::memory_order_acquire) != identity) {
        return;
    }
    if (!window->isSceneGraphInitialized()) {
        publishObservation(identity, EditPreviewPresentationApi::Unavailable, 0U, false);
        return;
    }
    QSGRendererInterface* const renderer = window->rendererInterface();
    if (renderer == nullptr) {
        publishObservation(identity, EditPreviewPresentationApi::Unavailable, 0U, true);
        return;
    }
    const auto api = presentation_api(renderer->graphicsApi());
    std::uintptr_t device_handle = 0U;
    if (api == EditPreviewPresentationApi::Metal) {
        // Qt 6.11 returns the Metal id<MTLDevice> itself for
        // DeviceResource, not a pointer to a stored object pointer.
        device_handle = reinterpret_cast<std::uintptr_t>(
            renderer->getResource(window, QSGRendererInterface::DeviceResource)
        );
    }
    publishObservation(identity, api, device_handle, true);
}

void EditPreviewPresentationContext::publishObservation(
    const std::uintptr_t window_identity_value,
    const EditPreviewPresentationApi api,
    const std::uintptr_t device_handle,
    const bool initialized
) noexcept {
    auto current = std::atomic_load_explicit(&state_->published, std::memory_order_acquire);
    for (;;) {
        if (current->window_identity == window_identity_value && current->api == api
            && current->device_handle == device_handle && current->initialized == initialized) {
            return;
        }
        EditPreviewPresentationBinding next{
            .epoch = current->epoch == std::numeric_limits<std::uint64_t>::max()
                         ? current->epoch
                         : current->epoch + 1U,
            .window_identity = window_identity_value,
            .api = api,
            .device_handle = device_handle,
            .initialized = initialized,
        };
        auto published = std::make_shared<const EditPreviewPresentationBinding>(next);
        if (std::atomic_compare_exchange_weak_explicit(
                &state_->published,
                &current,
                published,
                std::memory_order_release,
                std::memory_order_acquire
            )) {
            return;
        }
    }
}
