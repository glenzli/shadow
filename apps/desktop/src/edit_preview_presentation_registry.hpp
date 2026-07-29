#pragma once

#include <QObject>

#include <cstdint>
#include <memory>
#include <mutex>

class EditPreviewPresentationContext;
class EditPreviewStore;

// Explicit application-composition owner for the two C++ services required by
// live Qt Quick preview presentation. QML passes this object to every
// EditPreviewTextureItem; items never discover process-global state.
class EditPreviewPresentationRegistry final : public QObject {
    Q_OBJECT

  public:
    struct Owners final {
        std::shared_ptr<EditPreviewStore> store;
        std::shared_ptr<EditPreviewPresentationContext> presentation_context;
        std::uint64_t configuration_revision = 0U;

        [[nodiscard]] bool available() const noexcept {
            return store != nullptr && presentation_context != nullptr;
        }
    };

    explicit EditPreviewPresentationRegistry(QObject* parent = nullptr);
    EditPreviewPresentationRegistry(
        std::shared_ptr<EditPreviewStore> store,
        std::shared_ptr<EditPreviewPresentationContext> presentation_context,
        QObject* parent = nullptr
    );
    ~EditPreviewPresentationRegistry() override;

    EditPreviewPresentationRegistry(const EditPreviewPresentationRegistry&) = delete;
    EditPreviewPresentationRegistry& operator=(const EditPreviewPresentationRegistry&) = delete;

    void configure(
        std::shared_ptr<EditPreviewStore> store,
        std::shared_ptr<EditPreviewPresentationContext> presentation_context
    );
    void clear();

    [[nodiscard]] Owners owners() const;

  signals:
    void configurationChanged();

  private:
    mutable std::mutex mutex_;
    Owners owners_;
};
