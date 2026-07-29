#pragma once

#include "edit_preview_contract.hpp"

#include <QObject>

#include <memory>

class QQuickWindow;
class EditPreviewPresentationContextTestAccess;

// Process-local owner for the root Qt Quick scene graph's presentation
// capability. Render-thread signals publish one immutable tuple atomically;
// loading threads only copy the tuple and never call QSG/native APIs.
class EditPreviewPresentationContext final : public QObject {
  public:
    explicit EditPreviewPresentationContext(QObject* parent = nullptr);
    ~EditPreviewPresentationContext() override;

    EditPreviewPresentationContext(const EditPreviewPresentationContext&) = delete;
    EditPreviewPresentationContext& operator=(const EditPreviewPresentationContext&) = delete;

    void attach(QQuickWindow* window);

    [[nodiscard]] EditPreviewPresentationBinding snapshot() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;

    void observeWindow(QQuickWindow* window) noexcept;
    void publishObservation(
        std::uintptr_t window_identity,
        EditPreviewPresentationApi api,
        std::uintptr_t device_handle,
        bool initialized
    ) noexcept;

    friend class EditPreviewPresentationContextTestAccess;
};
