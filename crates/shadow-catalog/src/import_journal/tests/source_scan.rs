use super::import_fixtures::{request_at, root};
use crate::{Catalog, CatalogError, ImportSessionState, SourceScanReconciliation};

#[test]
fn source_freshness_is_recorded_only_after_completed_scan() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let cancelled = catalog
        .begin_import_session(&root(), 10)
        .expect("begin cancelled scan");
    let source_id = catalog
        .import_session(cancelled)
        .expect("read cancelled session")
        .expect("cancelled session exists")
        .source_id
        .expect("source attached");
    assert_eq!(
        catalog.library_sources().expect("sources")[0].last_scanned_at_ms,
        None
    );

    catalog
        .finish_import_session(cancelled, ImportSessionState::Cancelled, None, 20)
        .expect("cancel scan");
    assert_eq!(
        catalog.library_sources().expect("sources")[0].last_scanned_at_ms,
        None
    );

    let failed = catalog
        .begin_import_session(&root(), 30)
        .expect("begin failed scan");
    catalog
        .finish_import_session(failed, ImportSessionState::Failed, Some("unavailable"), 40)
        .expect("fail scan");
    assert_eq!(
        catalog.library_sources().expect("sources")[0].last_scanned_at_ms,
        None
    );

    let completed = catalog
        .begin_import_session(&root(), 50)
        .expect("begin completed scan");
    let observed = request_at("/photos/one.nef", 42, Some(100), 51);
    catalog
        .record_import_discovered(completed, &observed)
        .expect("record observed file");
    catalog
        .register_import_asset(completed, &observed)
        .expect("register observed file");
    catalog
        .finish_import_session(completed, ImportSessionState::Completed, None, 60)
        .expect("complete scan");

    let source = catalog
        .library_sources()
        .expect("sources")
        .into_iter()
        .find(|source| source.id == source_id)
        .expect("same source");
    assert_eq!(source.last_scanned_at_ms, Some(60));
}

#[test]
fn completed_scan_reports_source_locations_not_seen_without_marking_them_offline() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let first = catalog
        .begin_import_session(&root(), 10)
        .expect("begin initial scan");
    for observed in [
        request_at("/photos/one.nef", 42, Some(100), 11),
        request_at("/photos/two.nef", 84, Some(101), 12),
    ] {
        catalog
            .record_import_discovered(first, &observed)
            .expect("record initial source file");
        catalog
            .register_import_asset(first, &observed)
            .expect("register initial source file");
    }
    catalog
        .finish_import_session(first, ImportSessionState::Completed, None, 20)
        .expect("complete initial scan");
    assert_eq!(
        catalog
            .source_scan_reconciliation(first)
            .expect("reconcile initial scan"),
        Some(SourceScanReconciliation {
            session_id: first,
            source_id: catalog
                .import_session(first)
                .expect("read initial session")
                .expect("initial session exists")
                .source_id
                .expect("source attached"),
            completed_at_ms: 20,
            known_locations: 2,
            seen_locations: 2,
            not_seen_locations: 0,
        })
    );

    let second = catalog
        .begin_import_session(&root(), 30)
        .expect("begin follow-up scan");
    let observed = request_at("/photos/one.nef", 42, Some(100), 31);
    catalog
        .record_import_discovered(second, &observed)
        .expect("record remaining source file");
    catalog
        .register_import_asset(second, &observed)
        .expect("register remaining source file");
    catalog
        .finish_import_session(second, ImportSessionState::Completed, None, 40)
        .expect("complete follow-up scan");

    let reconciliation = catalog
        .source_scan_reconciliation(second)
        .expect("reconcile completed scan")
        .expect("source-backed session");
    assert_eq!(reconciliation.known_locations, 2);
    assert_eq!(reconciliation.seen_locations, 1);
    assert_eq!(reconciliation.not_seen_locations, 1);
    assert_eq!(
        catalog
            .library_photo_count(&crate::LibraryPhotoFilter::default())
            .expect("unseen source location remains visible"),
        2
    );
}

#[test]
fn source_reconciliation_requires_a_completed_session() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let running = catalog
        .begin_import_session(&root(), 10)
        .expect("begin scan");
    assert!(matches!(
        catalog.source_scan_reconciliation(running),
        Err(CatalogError::InvalidImportSessionState {
            id,
            state: "running"
        }) if id == running
    ));
}
