#include "library_keyword_fixture.hpp"

namespace review_library_keyword_test {

void run_keyword_lifecycle_contracts() {
    auto state = std::make_shared<KeywordBackendState>();
    state->keywords = {
        keyword(QStringLiteral("nature"), QStringLiteral("Nature"), {}, 0, 2),
        keyword(QStringLiteral("forest"), QStringLiteral("Forest"), QStringLiteral("nature"), 1, 1),
    };
    state->assignments.insert(
        QStringLiteral("photo-a"),
        {
            assignment(state->keywords.at(1)),
        }
    );
    ReviewLibraryKeywordCoordinator coordinator(operations(state));
    int mutations = 0;
    QObject::connect(
        &coordinator,
        &ReviewLibraryKeywordCoordinator::keywordMutationAccepted,
        [&mutations]() { ++mutations; }
    );

    coordinator.setPhotoId(QStringLiteral(" photo-a "));
    wait_until(
        [&coordinator]() { return !coordinator.busy(); },
        "selected-photo keyword refresh completes"
    );
    require(
        coordinator.photoId() == QStringLiteral("photo-a") && coordinator.keywords().size() == 2
            && coordinator.photoKeywords().size() == 1,
        "one refresh publishes the taxonomy and selected-photo assignments"
    );
    const QVariantMap forest = coordinator.keywords().at(1).toMap();
    require(
        forest.value(QStringLiteral("parentId")) == QStringLiteral("nature")
            && forest.value(QStringLiteral("depth")).toUInt() == 1
            && forest.value(QStringLiteral("photoCount")).toULongLong() == 1,
        "projection preserves hierarchy depth and subtree count"
    );

    coordinator.createKeyword(QStringLiteral("nature"), QStringLiteral(" Wildlife "));
    wait_until([&coordinator]() { return !coordinator.busy(); }, "keyword creation completes");
    require(
        coordinator.keywords().size() == 3
            && coordinator.statusMessage().translated()
                   == QStringLiteral("Library keyword created"),
        "creation refreshes the authoritative tree and status"
    );

    coordinator.assignKeyword(
        QStringLiteral("nature"),
        {
            target(QStringLiteral("photo-a")),
            target(QStringLiteral(" photo-a ")),
            target(QStringLiteral("photo-b")),
            target({}),
        }
    );
    wait_until([&coordinator]() { return !coordinator.busy(); }, "batch assignment completes");
    {
        std::lock_guard lock(state->mutex);
        require(
            state->last_photo_ids
                == QStringList{
                    QStringLiteral("photo-a"),
                    QStringLiteral("photo-b"),
                },
            "batch assignment trims and deduplicates photo identities"
        );
    }
    require(
        mutations == 2
            && coordinator.statusMessage().translated()
                   == QStringLiteral("Tagged selected photos · 2 changed"),
        "accepted mutations invalidate consumers and report exact changes"
    );

    coordinator.deleteKeyword(QStringLiteral("forest"));
    wait_until([&coordinator]() { return !coordinator.busy(); }, "keyword deletion completes");
    require(
        coordinator.statusMessage().translated() == QStringLiteral("Deleted keyword nodes · 1"),
        "explicit subtree deletion reports its destructive scope"
    );
}

} // namespace review_library_keyword_test
