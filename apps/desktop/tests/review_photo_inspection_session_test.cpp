#include "review_photo_inspection_session.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "photo inspection session contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void newer_selection_rejects_older_completion() {
    ReviewPhotoInspectionSession session;
    const auto first = session.request(
        QStringLiteral("photo-a"),
        QStringLiteral("representation-a")
    );
    const auto second = session.request(
        QStringLiteral("photo-b"),
        QStringLiteral("representation-b")
    );

    require(!session.accepts(first), "selection B must reject late selection A");
    require(session.accepts(second), "selection B must accept its exact request");
}

void clear_rejects_late_completion() {
    ReviewPhotoInspectionSession session;
    const auto request = session.request(
        QStringLiteral("photo-a"),
        QStringLiteral("representation-a")
    );

    session.clear();

    require(!session.accepts(request), "clear must reject an in-flight completion");
    require(!session.current().valid(), "clear must remove the desired identity");
}

void same_identity_refresh_supersedes_older_generation() {
    ReviewPhotoInspectionSession session;
    const auto first = session.request(
        QStringLiteral("photo-a"),
        QStringLiteral("representation-a")
    );
    const auto refreshed = session.request(
        QStringLiteral("photo-a"),
        QStringLiteral("representation-a")
    );

    require(
        first.generation != refreshed.generation,
        "same-identity refresh must receive a new generation"
    );
    require(!session.accepts(first), "refresh must reject its older completion");
    require(session.accepts(refreshed), "refresh must accept its latest completion");
}

void library_generation_changes_do_not_touch_inspection_identity() {
    ReviewPhotoInspectionSession session;
    const auto request = session.request(
        QStringLiteral("photo-a"),
        QStringLiteral("representation-a")
    );
    quint64 library_generation = 1;

    ++library_generation;

    require(library_generation == 2, "the unrelated Library generation advanced");
    require(
        session.current().generation == request.generation,
        "Library refresh must not advance photo inspection generation"
    );
    require(
        session.accepts(request),
        "Library refresh must not invalidate the exact inspection request"
    );
}

} // namespace

int main() {
    newer_selection_rejects_older_completion();
    clear_rejects_late_completion();
    same_identity_refresh_supersedes_older_generation();
    library_generation_changes_do_not_touch_inspection_identity();
    return EXIT_SUCCESS;
}
