use std::{fs, path::PathBuf};

use shadow_domain::{EntityId, PhotoId, RepresentationId};

use crate::protocol::{
    PreviewUnavailableReason, RemotePhotoManifest, RemotePhotoMetadata, RemotePreviewAvailability,
};

use super::{
    MirroredLocalSource, RemoteLibraryMirror, RemotePhotoMirror, RemoteReviewFlag,
    RemoteReviewState,
};

#[test]
fn materialized_mapping_survives_mirror_reopen() {
    let root = temporary_directory("mirror-reopen");
    let remote_photo_id = PhotoId::new_v7();
    let remote_representation_id = RepresentationId::new_v7();
    let local_photo_id = PhotoId::new_v7();
    let local_representation_id = RepresentationId::new_v7();
    let mut mirror = RemoteLibraryMirror::open(&root).expect("open mirror");
    let remote_manifest = RemotePhotoManifest {
        photo_id: remote_photo_id,
        representation_id: remote_representation_id,
        display_name: "source.nef".to_owned(),
        source_byte_len: 12,
        source_modified_at_ms: Some(7),
        metadata: RemotePhotoMetadata::default(),
        preview: RemotePreviewAvailability::Unavailable {
            reason: PreviewUnavailableReason::DecoderCapabilityMissing,
        },
    };
    mirror.snapshot.photos.push(RemotePhotoMirror {
        manifest: remote_manifest.clone(),
        cached_preview: None,
        review_state: RemoteReviewState::default(),
        local_source: None,
    });
    mirror
        .mark_materialized(
            remote_photo_id,
            remote_representation_id,
            MirroredLocalSource::for_remote_manifest(
                local_photo_id,
                local_representation_id,
                [9; 32],
                "/client/cache/source.nef".to_owned(),
                &remote_manifest,
            ),
        )
        .expect("record local source");
    drop(mirror);

    let reopened = RemoteLibraryMirror::open(&root).expect("reopen mirror");
    let local = reopened.snapshot().photos[0]
        .local_source
        .as_ref()
        .expect("local source mapping");
    assert_eq!(local.photo_id, local_photo_id);
    assert_eq!(local.representation_id, local_representation_id);
    assert!(local.matches_remote_source(&remote_manifest));
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn materialized_mapping_rejects_a_changed_remote_source_revision() {
    let remote_photo_id = PhotoId::new_v7();
    let remote_representation_id = RepresentationId::new_v7();
    let manifest = RemotePhotoManifest {
        photo_id: remote_photo_id,
        representation_id: remote_representation_id,
        display_name: "source.nef".to_owned(),
        source_byte_len: 12,
        source_modified_at_ms: Some(7),
        metadata: RemotePhotoMetadata::default(),
        preview: RemotePreviewAvailability::Unavailable {
            reason: PreviewUnavailableReason::DecoderCapabilityMissing,
        },
    };
    let local = MirroredLocalSource::for_remote_manifest(
        PhotoId::new_v7(),
        RepresentationId::new_v7(),
        [9; 32],
        "/client/cache/source.nef".to_owned(),
        &manifest,
    );
    let mut changed = manifest.clone();
    changed.source_modified_at_ms = Some(8);

    assert!(local.matches_remote_source(&manifest));
    assert!(!local.matches_remote_source(&changed));
}

#[test]
fn remote_review_state_survives_mirror_reopen() {
    let root = temporary_directory("review-state-reopen");
    let remote_photo_id = PhotoId::new_v7();
    let remote_representation_id = RepresentationId::new_v7();
    let mut mirror = RemoteLibraryMirror::open(&root).expect("open mirror");
    mirror.snapshot.photos.push(RemotePhotoMirror {
        manifest: RemotePhotoManifest {
            photo_id: remote_photo_id,
            representation_id: remote_representation_id,
            display_name: "curated.nef".to_owned(),
            source_byte_len: 12,
            source_modified_at_ms: Some(7),
            metadata: RemotePhotoMetadata::default(),
            preview: RemotePreviewAvailability::Unavailable {
                reason: PreviewUnavailableReason::NotPrepared,
            },
        },
        cached_preview: None,
        review_state: RemoteReviewState::default(),
        local_source: None,
    });
    mirror
        .set_review_state(
            remote_photo_id,
            remote_representation_id,
            RemoteReviewState {
                flag: RemoteReviewFlag::Picked,
                rating: 4,
                liked: true,
                color_label: "red".to_owned(),
                updated_at_ms: 42,
            },
        )
        .expect("persist remote review state");
    drop(mirror);

    let reopened = RemoteLibraryMirror::open(&root).expect("reopen mirror");
    assert_eq!(
        reopened.snapshot().photos[0].review_state,
        RemoteReviewState {
            flag: RemoteReviewFlag::Picked,
            rating: 4,
            liked: true,
            color_label: "red".to_owned(),
            updated_at_ms: 42,
        }
    );
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn rejects_out_of_range_remote_rating() {
    let root = temporary_directory("review-state-invalid");
    let remote_photo_id = PhotoId::new_v7();
    let remote_representation_id = RepresentationId::new_v7();
    let mut mirror = RemoteLibraryMirror::open(&root).expect("open mirror");
    mirror.snapshot.photos.push(RemotePhotoMirror {
        manifest: RemotePhotoManifest {
            photo_id: remote_photo_id,
            representation_id: remote_representation_id,
            display_name: "invalid.nef".to_owned(),
            source_byte_len: 12,
            source_modified_at_ms: None,
            metadata: RemotePhotoMetadata::default(),
            preview: RemotePreviewAvailability::Unavailable {
                reason: PreviewUnavailableReason::NotPrepared,
            },
        },
        cached_preview: None,
        review_state: RemoteReviewState::default(),
        local_source: None,
    });

    let error = mirror
        .set_review_state(
            remote_photo_id,
            remote_representation_id,
            RemoteReviewState {
                rating: 6,
                ..RemoteReviewState::default()
            },
        )
        .expect_err("invalid rating must fail");
    assert!(error.to_string().contains("between zero and five"));
    fs::remove_dir_all(root).expect("remove fixture");
}

fn temporary_directory(label: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!(
        "shadow-library-sharing-{label}-{}",
        uuid::Uuid::now_v7()
    ));
    fs::create_dir_all(&path).expect("create fixture directory");
    path
}
