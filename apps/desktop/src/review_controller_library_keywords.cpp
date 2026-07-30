#include "review_controller.hpp"

// Hierarchical taxonomy management, selected-photo projection, and batch
// assignment remain in the dedicated coordinator. This stable QML facade owns
// only intent routing.

void ReviewController::refreshLibraryKeywords() {
    keyword_coordinator_.refresh();
}

void ReviewController::requestLibraryKeywordsForPhoto(const QString& photo_id) {
    keyword_coordinator_.setPhotoId(photo_id);
}

void ReviewController::createLibraryKeyword(const QString& parent_id, const QString& name) {
    keyword_coordinator_.createKeyword(parent_id, name);
}

void ReviewController::renameLibraryKeyword(const QString& keyword_id, const QString& name) {
    keyword_coordinator_.renameKeyword(keyword_id, name);
}

void ReviewController::moveLibraryKeyword(const QString& keyword_id, const QString& parent_id) {
    keyword_coordinator_.moveKeyword(keyword_id, parent_id);
}

void ReviewController::deleteLibraryKeyword(const QString& keyword_id) {
    keyword_coordinator_.deleteKeyword(keyword_id);
}

void ReviewController::assignLibraryKeyword(
    const QString& keyword_id,
    const QVariantList& targets
) {
    keyword_coordinator_.assignKeyword(keyword_id, targets);
}

void ReviewController::removeLibraryKeyword(
    const QString& keyword_id,
    const QVariantList& targets
) {
    keyword_coordinator_.removeKeyword(keyword_id, targets);
}
