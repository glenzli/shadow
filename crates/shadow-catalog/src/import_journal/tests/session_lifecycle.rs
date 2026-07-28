use shadow_domain::{EntityId, ImportSessionId};

use super::import_fixtures::{request, root};
use crate::{Catalog, CatalogError, ImportSessionState};

#[test]
fn journal_tracks_registration_and_completion() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let session_id = catalog
        .begin_import_session(&root(), 10)
        .expect("begin session");
    let source_id = catalog
        .import_session(session_id)
        .expect("read session")
        .expect("session exists")
        .source_id
        .expect("v11 source is attached to the session");
    let request = request();
    catalog
        .record_import_discovered(session_id, &request)
        .expect("record discovered");
    catalog
        .register_import_asset(session_id, &request)
        .expect("register journaled asset");
    catalog
        .finish_import_session(session_id, ImportSessionState::Completed, None, 20)
        .expect("finish session");

    let summary = catalog
        .import_session_summary(session_id)
        .expect("session summary");
    assert_eq!(summary.session.state, ImportSessionState::Completed);
    assert_eq!(summary.session.source_id, Some(source_id));
    assert_eq!(summary.discovered, 1);
    assert_eq!(summary.inserted, 1);
    let sources = catalog.library_sources().expect("list scan sources");
    assert_eq!(sources.len(), 1);
    assert_eq!(sources[0].id, source_id);
    assert_eq!(sources[0].root, root());
    assert_eq!(sources[0].last_scanned_at_ms, Some(20));
    assert!(
        catalog
            .unfinished_import_sessions()
            .expect("unfinished sessions")
            .is_empty()
    );
}

#[test]
fn running_session_survives_catalog_reopen() {
    let database_path = std::env::temp_dir().join(format!(
        "shadow-import-journal-{}.sqlite",
        ImportSessionId::new_v7()
    ));
    let session_id;
    {
        let mut catalog = Catalog::open(&database_path).expect("open catalog");
        session_id = catalog
            .begin_import_session(&root(), 10)
            .expect("begin session");
        catalog
            .record_import_discovered(session_id, &request())
            .expect("record discovered");
    }
    {
        let catalog = Catalog::open(&database_path).expect("reopen catalog");
        let sessions = catalog
            .unfinished_import_sessions()
            .expect("unfinished sessions");
        assert_eq!(sessions.len(), 1);
        assert_eq!(sessions[0].id, session_id);
    }

    std::fs::remove_file(&database_path).expect("remove test catalog");
    let wal_path = database_path.with_extension("sqlite-wal");
    let shm_path = database_path.with_extension("sqlite-shm");
    if wal_path.exists() {
        std::fs::remove_file(wal_path).expect("remove test wal");
    }
    if shm_path.exists() {
        std::fs::remove_file(shm_path).expect("remove test shm");
    }
}

#[test]
fn completed_session_cannot_be_rewritten() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let session_id = catalog
        .begin_import_session(&root(), 10)
        .expect("begin session");
    catalog
        .finish_import_session(session_id, ImportSessionState::Completed, None, 20)
        .expect("complete session");

    let error = catalog
        .finish_import_session(session_id, ImportSessionState::Failed, Some("late"), 30)
        .expect_err("terminal session must be immutable");
    assert!(matches!(
        error,
        CatalogError::InvalidImportSessionState { .. }
    ));
    assert_eq!(
        catalog
            .import_session(session_id)
            .expect("load session")
            .expect("session exists")
            .state,
        ImportSessionState::Completed
    );
}
