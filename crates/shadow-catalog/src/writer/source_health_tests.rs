use shadow_domain::{AssetLocation, ImportSessionId, Platform, RepresentationKind};

use crate::{CatalogStore, ImportSessionState, RegisterAsset, RegisteredAsset};

use super::{CatalogActor, CatalogHandle};

fn register_scan_entry(
    handle: &mut CatalogHandle,
    session_id: ImportSessionId,
    path: &str,
    now_ms: i64,
) -> RegisteredAsset {
    let request = RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
        byte_len: 100,
        modified_at_ms: Some(10),
        now_ms,
    };
    handle
        .record_import_discovered(session_id, &request)
        .expect("record scan discovery through actor");
    handle
        .register_import_asset(session_id, &request)
        .expect("register scan entry through actor")
}

#[test]
fn actor_routes_source_inventory_health_and_missing_location_review() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let mut handle = actor.handle();
    let root = AssetLocation::new(Platform::MacOs, b"/archive".to_vec(), "/archive");
    let first_scan = handle
        .begin_import_session(&root, 1)
        .expect("begin first source scan");
    let observed = register_scan_entry(&mut handle, first_scan, "/archive/observed.nef", 2);
    let missing = register_scan_entry(&mut handle, first_scan, "/archive/missing.nef", 3);
    handle
        .finish_import_session(first_scan, ImportSessionState::Completed, None, 4)
        .expect("finish first source scan");

    let second_scan = handle
        .begin_import_session(&root, 10)
        .expect("begin second source scan");
    let observed_again = register_scan_entry(&mut handle, second_scan, "/archive/observed.nef", 11);
    assert_eq!(observed_again.representation_id, observed.representation_id);
    handle
        .finish_import_session(second_scan, ImportSessionState::Completed, None, 12)
        .expect("finish second source scan");

    let sources = handle
        .library_sources()
        .expect("list sources through actor");
    assert_eq!(sources.len(), 1);
    assert_eq!(sources[0].root.display_path, "/archive");

    let health = handle
        .library_source_health()
        .expect("read source health through actor");
    let scan = health[0]
        .latest_completed_scan
        .as_ref()
        .expect("completed scan evidence");
    assert_eq!(scan.session_id, second_scan);
    assert_eq!(scan.known_locations, 2);
    assert_eq!(scan.seen_locations, 1);
    assert_eq!(scan.not_seen_locations, 1);

    let page = handle
        .missing_source_location_page(second_scan, None, 16)
        .expect("page missing locations through actor")
        .expect("durable source page");
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.items[0].representation_id, missing.representation_id);
    assert_eq!(page.reconciliation, *scan);
    assert!(
        page.next_cursor.is_none(),
        "single missing location fits in one page"
    );

    let target = handle
        .missing_source_relink_target(second_scan, page.items[0].location_id)
        .expect("read relink target through actor")
        .expect("missing location remains valid evidence");
    assert_eq!(target.location, page.items[0]);
    assert!(
        handle
            .missing_source_relink_target(second_scan, observed_again.location_id)
            .expect("check observed location through actor")
            .is_none()
    );
    actor.shutdown().expect("shutdown actor");
}
