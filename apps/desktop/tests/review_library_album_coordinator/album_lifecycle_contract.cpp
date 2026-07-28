#include "library_album_fixture.hpp"

namespace review_library_album_test {

void run_album_lifecycle_contracts() {
    auto state = std::make_shared<AlbumBackendState>();
    state->albums = {
        album(QStringLiteral("manual"), QStringLiteral("Manual")),
        album(
            QStringLiteral("smart"),
            QStringLiteral("Smart"),
            BackendLibraryAlbumKind::Smart
        ),
    };
    ReviewLibraryAlbumCoordinator coordinator(operations(state));
    int query_changes = 0;
    QObject::connect(
        &coordinator,
        &ReviewLibraryAlbumCoordinator::queryChanged,
        [&query_changes]() { ++query_changes; }
    );

    coordinator.refresh();
    wait_until(
        [&coordinator]() { return !coordinator.busy(); },
        "initial album refresh completes"
    );
    const QVariantList initial = coordinator.albums();
    require(initial.size() == 2, "refresh publishes the complete snapshot");
    require(
        initial.at(0).toMap().value(QStringLiteral("kind"))
            == QStringLiteral("manual")
            && initial.at(1).toMap().value(QStringLiteral("kind"))
                == QStringLiteral("smart"),
        "projection preserves manual and smart album kinds"
    );

    coordinator.setAlbumId(QStringLiteral(" manual "));
    require(
        coordinator.albumId() == QStringLiteral("manual")
            && query_changes == 1,
        "selection normalizes identity and invalidates the Library query"
    );

    BackendLibraryPhotoFilter smart_query;
    smart_query.color_label = QStringLiteral("red");
    smart_query.album_id = QStringLiteral("nested-membership");
    coordinator.createSmart(QStringLiteral(" New Smart "), smart_query);
    wait_until(
        [&coordinator]() { return !coordinator.busy(); },
        "smart album creation completes"
    );
    {
        std::lock_guard lock(state->mutex);
        require(
            state->smart_query.color_label == QStringLiteral("red")
                && state->smart_query.album_id.isEmpty(),
            "smart album freezes filters but never nests album membership"
        );
    }
    require(
        coordinator.statusChannel()
            == ReviewLibraryAlbumCoordinator::StatusChannel::Decision
            && coordinator.statusMessage().translated()
                == QStringLiteral("Library album created"),
        "creation publishes the localized decision status"
    );

    coordinator.addPhotos(
        QStringLiteral("manual"),
        {
            target(QStringLiteral("photo-a")),
            target(QStringLiteral(" photo-a ")),
            target(QStringLiteral("photo-b")),
            target({}),
        }
    );
    wait_until(
        [&coordinator]() { return !coordinator.busy(); },
        "membership addition completes"
    );
    {
        std::lock_guard lock(state->mutex);
        require(
            state->added
                == QStringList{
                    QStringLiteral("manual:photo-a"),
                    QStringLiteral("manual:photo-b"),
                },
            "membership mutation trims and deduplicates photo identities"
        );
    }
    require(
        query_changes == 2
            && coordinator.statusMessage().translated()
                == QStringLiteral("2 photos added to the album"),
        "selected-album membership refreshes the query and reports its count"
    );

    coordinator.remove(QStringLiteral("manual"));
    wait_until(
        [&coordinator]() { return !coordinator.busy(); },
        "album deletion completes"
    );
    require(
        coordinator.albumId().isEmpty() && query_changes == 3,
        "deleting the selected album clears selection and refreshes the query"
    );
}

} // namespace review_library_album_test
