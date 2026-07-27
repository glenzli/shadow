use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{
    CatalogStore, ContentIdentity, ImportSessionState, RecordRepresentationContentIdentity,
    RegisterAsset, RepresentationFingerprint,
};

use super::CatalogActor;

#[test]
fn actor_attaches_a_confirmed_relocation_only_through_the_journal() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let mut handle = actor.handle();
    let original = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/original.nef".to_vec(),
                "/photos/original.nef",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1,
        })
        .expect("register original");
    let identity = ContentIdentity::whole_file_blake3([41; 32]);
    handle
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 42,
                modified_at_ms: Some(100),
            },
            identity: identity.clone(),
            observed_at_ms: 2,
        })
        .expect("record identity");

    let session = handle
        .begin_relocation_session(
            &AssetLocation::new(Platform::MacOs, b"/consolidated".to_vec(), "/consolidated"),
            3,
        )
        .expect("begin relocation session");
    assert!(
        handle
            .import_session_summary(session)
            .expect("read relocation session")
            .session
            .source_id
            .is_none()
    );
    let moved_request = RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(
            Platform::MacOs,
            b"/consolidated/renamed.nef".to_vec(),
            "/consolidated/renamed.nef",
        ),
        byte_len: 42,
        modified_at_ms: Some(200),
        now_ms: 4,
    };
    handle
        .record_import_discovered(session, &moved_request)
        .expect("journal discovery");

    let moved = CatalogStore::register_import_verified_relocation(
        &mut handle,
        session,
        &moved_request,
        original.representation_id,
        &identity,
    )
    .expect("attach through actor trait adapter");
    assert_eq!(moved.photo_id, original.photo_id);
    assert_eq!(moved.representation_id, original.representation_id);
    assert_eq!(handle.stats().expect("stats").locations, 2);
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_routes_the_normal_import_lifecycle_and_summary() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let mut handle = actor.handle();
    let root = AssetLocation::new(Platform::MacOs, b"/incoming".to_vec(), "/incoming");
    let session = handle
        .begin_import_session(&root, 10)
        .expect("begin import session");
    assert!(
        handle
            .unfinished_import_sessions()
            .expect("list running sessions")
            .iter()
            .any(|candidate| candidate.id == session)
    );

    let request = RegisterAsset {
        kind: RepresentationKind::OriginalRaster,
        location: AssetLocation::new(
            Platform::MacOs,
            b"/incoming/frame.jpg".to_vec(),
            "/incoming/frame.jpg",
        ),
        byte_len: 128,
        modified_at_ms: Some(11),
        now_ms: 12,
    };
    handle
        .record_import_discovered(session, &request)
        .expect("record discovered asset");
    handle
        .register_import_asset(session, &request)
        .expect("register import asset");
    handle
        .record_import_issue(
            session,
            &AssetLocation::new(
                Platform::MacOs,
                b"/incoming/unreadable.raw".to_vec(),
                "/incoming/unreadable.raw",
            ),
            "metadata unavailable",
            13,
        )
        .expect("record recoverable issue");
    handle
        .finish_import_session(session, ImportSessionState::Completed, None, 14)
        .expect("finish import session");

    let summary = handle
        .import_session_summary(session)
        .expect("read import summary");
    assert_eq!(summary.session.state, ImportSessionState::Completed);
    assert_eq!(summary.discovered, 1);
    assert_eq!(summary.inserted, 1);
    assert_eq!(summary.issues, 1);
    assert!(
        handle
            .unfinished_import_sessions()
            .expect("list completed sessions")
            .iter()
            .all(|candidate| candidate.id != session)
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_resumes_a_failed_import_session() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let mut handle = actor.handle();
    let session = handle
        .begin_import_session(
            &AssetLocation::new(Platform::MacOs, b"/retry".to_vec(), "/retry"),
            20,
        )
        .expect("begin import session");
    handle
        .finish_import_session(
            session,
            ImportSessionState::Failed,
            Some("device disconnected"),
            21,
        )
        .expect("fail import session");
    assert_eq!(
        handle
            .unfinished_import_sessions()
            .expect("list failed session")[0]
            .state,
        ImportSessionState::Failed
    );

    let resumed = handle
        .resume_import_session(session, 22)
        .expect("resume failed session");
    assert_eq!(resumed.state, ImportSessionState::Running);
    assert_eq!(resumed.last_error, None);
    handle
        .finish_import_session(session, ImportSessionState::Cancelled, None, 23)
        .expect("cancel resumed session");
    assert!(
        handle
            .unfinished_import_sessions()
            .expect("list terminal sessions")
            .is_empty()
    );
    actor.shutdown().expect("shutdown actor");
}
