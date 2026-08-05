#include "review_model_fixture.hpp"

namespace review_model_test {

void remote_rows_use_local_proxy_urls_and_replace_without_resetting_local_rows() {
    ReviewModel model;
    ReviewItem local = keyed_item("local", "Local");
    local.captured_at_unix_seconds = 200;
    local.visual_handle = QStringLiteral("local-ticket");
    local.has_visual = true;
    model.replace({local}, 9);

    ReviewItem remote = keyed_item("remote", "Remote");
    remote.is_remote = true;
    remote.remote_server_id = QStringLiteral("server-a");
    remote.remote_photo_id = QStringLiteral("photo-a");
    remote.remote_representation_id = QStringLiteral("representation-a");
    remote.visual_source_override = QStringLiteral("file:///client-cache/proxy-a");
    remote.has_visual = true;
    remote.visual_role = QStringLiteral("embedded_preview");
    remote.captured_at_unix_seconds = 100;
    remote.remote_original_cached = true;

    ModelSignalCounts counts;
    observe_model(model, counts);
    require(
        model.replaceRemoteItems({remote}),
        "a valid remote snapshot must append beside local Catalog rows"
    );
    require(
        model.rowCount() == 2
            && value(model, 0, ReviewModel::TitleRole).toString() == QStringLiteral("Local")
            && value(model, 1, ReviewModel::IsRemoteRole).toBool()
            && value(model, 1, ReviewModel::RemoteServerIdRole).toString()
                   == QStringLiteral("server-a")
            && value(model, 1, ReviewModel::RemotePhotoIdRole).toString()
                   == QStringLiteral("photo-a")
            && value(model, 1, ReviewModel::RemoteRepresentationIdRole).toString()
                   == QStringLiteral("representation-a")
            && value(model, 1, ReviewModel::RemoteOriginalCachedRole).toBool()
            && value(model, 1, ReviewModel::VisualSourceRole).toString()
                   == QStringLiteral("file:///client-cache/proxy-a"),
        "remote identity and verified client-local proxy must retain stable roles"
    );
    require(
        counts.resets == 0 && counts.inserted_rows == 1,
        "remote refresh must not reset the local grid"
    );

    ReviewItem missing = remote;
    missing.photo_id = QStringLiteral("remote-missing-photo");
    missing.has_visual = false;
    missing.visual_source_override.clear();
    missing.remote_preview_unavailable_reason = QStringLiteral("decoder_capability_missing");
    require(
        model.replaceRemoteItems({missing}),
        "a remote capability gap must remain a visible row"
    );
    require(
        model.rowCount() == 2
            && value(model, 0, ReviewModel::TitleRole).toString() == QStringLiteral("Local")
            && value(model, 1, ReviewModel::VisualSourceRole).toString().isEmpty()
            && value(model, 1, ReviewModel::VisualErrorRole).toString()
                   == QStringLiteral("decoder_capability_missing")
            && value(model, 1, ReviewModel::RemotePreviewUnavailableReasonRole).toString()
                   == QStringLiteral("decoder_capability_missing"),
        "private-provider preview gaps must not disappear from the Library"
    );
}

void local_and_remote_rows_share_one_presentation_order() {
    ReviewModel model;
    ReviewItem older_local = keyed_item("local-old", "Zulu.nef");
    older_local.captured_at_unix_seconds = 100;
    ReviewItem newer_local = keyed_item("local-new", "Alpha.nef");
    newer_local.captured_at_unix_seconds = 300;
    model.replace({newer_local, older_local}, 4);

    ReviewItem middle_remote = keyed_item("remote-middle", "Middle.nef");
    middle_remote.is_remote = true;
    middle_remote.remote_photo_id = QStringLiteral("remote-photo");
    middle_remote.remote_representation_id = QStringLiteral("remote-representation");
    middle_remote.captured_at_unix_seconds = 200;
    require(model.replaceRemoteItems({middle_remote}), "merge one remote row");
    require(
        value(model, 0, ReviewModel::PhotoIdRole).toString() == QStringLiteral("local-new-photo")
            && value(model, 1, ReviewModel::PhotoIdRole).toString()
                   == QStringLiteral("remote-middle-photo")
            && value(model, 2, ReviewModel::PhotoIdRole).toString()
                   == QStringLiteral("local-old-photo"),
        "capture-time order must interleave local and remote rows (actual: "
            + value(model, 0, ReviewModel::PhotoIdRole).toString().toStdString() + ", "
            + value(model, 1, ReviewModel::PhotoIdRole).toString().toStdString() + ", "
            + value(model, 2, ReviewModel::PhotoIdRole).toString().toStdString() + ")"
    );

    model.setPresentationOrder(ReviewModel::PresentationSortKey::Name, false);
    require(
        value(model, 0, ReviewModel::TitleRole).toString() == QStringLiteral("Alpha.nef")
            && value(model, 1, ReviewModel::TitleRole).toString()
                   == QStringLiteral("Middle.nef")
            && value(model, 2, ReviewModel::TitleRole).toString() == QStringLiteral("Zulu.nef"),
        "name order must use one merged local/remote sequence"
    );
}

void remote_replacement_rejects_local_items_and_key_collisions() {
    ReviewModel model;
    ReviewItem local = keyed_item("local", "Local");
    model.replace({local}, 3);

    ReviewItem invalid = keyed_item("invalid", "Invalid");
    require(
        !model.replaceRemoteItems({invalid}) && model.rowCount() == 1,
        "the remote replacement boundary must reject local rows"
    );

    ReviewItem collision = local;
    collision.is_remote = true;
    collision.remote_photo_id = QStringLiteral("remote-photo");
    collision.remote_representation_id = QStringLiteral("remote-representation");
    require(
        !model.replaceRemoteItems({collision}) && model.rowCount() == 1,
        "remote presentation keys must not collide with local photo identities"
    );
}

} // namespace review_model_test
