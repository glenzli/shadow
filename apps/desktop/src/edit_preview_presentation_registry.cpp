#include "edit_preview_presentation_registry.hpp"

#include <limits>
#include <utility>

EditPreviewPresentationRegistry::EditPreviewPresentationRegistry(QObject* const parent) :
    QObject(parent) {}

EditPreviewPresentationRegistry::EditPreviewPresentationRegistry(
    std::shared_ptr<EditPreviewStore> store,
    std::shared_ptr<EditPreviewPresentationContext> presentation_context,
    QObject* const parent
) :
    QObject(parent), owners_{
                         .store = std::move(store),
                         .presentation_context = std::move(presentation_context),
                         .configuration_revision = 1U,
                     } {}

EditPreviewPresentationRegistry::~EditPreviewPresentationRegistry() = default;

void EditPreviewPresentationRegistry::configure(
    std::shared_ptr<EditPreviewStore> store,
    std::shared_ptr<EditPreviewPresentationContext> presentation_context
) {
    {
        const std::scoped_lock lock(mutex_);
        if (owners_.store == store && owners_.presentation_context == presentation_context) {
            return;
        }
        const std::uint64_t next_revision =
            owners_.configuration_revision == std::numeric_limits<std::uint64_t>::max()
                ? 1U
                : owners_.configuration_revision + 1U;
        owners_ = {
            .store = std::move(store),
            .presentation_context = std::move(presentation_context),
            .configuration_revision = next_revision,
        };
    }
    emit configurationChanged();
}

void EditPreviewPresentationRegistry::clear() {
    configure({}, {});
}

EditPreviewPresentationRegistry::Owners EditPreviewPresentationRegistry::owners() const {
    const std::scoped_lock lock(mutex_);
    return owners_;
}
