#include "review_controller.hpp"

#include <cstdint>

QVariantList ReviewController::locationReferenceLibraries() const {
    return location_reference_coordinator_.libraries();
}

bool ReviewController::locationReferenceBusy() const noexcept {
    return location_reference_coordinator_.busy();
}

QString ReviewController::locationReferenceErrorText() const {
    return location_reference_coordinator_.errorText();
}

void ReviewController::refreshLocationReferenceLibraries() {
    location_reference_coordinator_.refresh();
}

void ReviewController::addLocationReferenceLibrary(
    const QUrl& root_url,
    const qlonglong clock_offset_seconds
) {
    if (!root_url.isLocalFile()) {
        return;
    }
    location_reference_coordinator_.addOrRescan(
        root_url.toLocalFile(), static_cast<std::int64_t>(clock_offset_seconds)
    );
}

void ReviewController::removeLocationReferenceLibrary(const QString& id) {
    location_reference_coordinator_.remove(id);
}
