//! Explicit cache-maintenance inventory and sweep contracts.

use shadow_domain::{EntityId, RepresentationId};

use crate::open_desktop_session;

#[test]
fn cache_maintenance_requires_an_explicit_sweep_and_preserves_unknown_entries() {
    let root = std::env::temp_dir().join(format!(
        "shadow-desktop-cache-maintenance-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create cache-maintenance fixture");
    let cache_root = root.join("cache");
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        cache_root.to_str().expect("cache path"),
    )
    .expect("open desktop session");

    let unknown = cache_root.join("blobs/b3/not-a-prefix/diagnostic");
    std::fs::create_dir_all(unknown.parent().expect("unknown parent"))
        .expect("create unknown cache directory");
    std::fs::write(&unknown, b"future cache format").expect("write unknown cache entry");

    let inventory = session
        .cache_maintenance_inventory()
        .expect("read cache-maintenance inventory");
    assert_eq!(inventory.catalog_live_blob_count, 0);
    assert_eq!(inventory.cache_blob_count, 0);
    assert_eq!(inventory.unknown_entry_count, 1);

    let planned = session
        .cache_maintenance_sweep(true)
        .expect("plan cache maintenance");
    assert!(planned.dry_run);
    assert_eq!(planned.unknown_entry_count, 1);
    assert_eq!(planned.reclaimed_blob_count, 0);
    assert!(unknown.exists());

    let applied = session
        .cache_maintenance_sweep(false)
        .expect("run explicit cache maintenance");
    assert!(!applied.dry_run);
    assert_eq!(applied.unknown_entry_count, 1);
    assert_eq!(applied.reclaimed_blob_count, 0);
    assert!(unknown.exists());

    drop(session);
    std::fs::remove_dir_all(root).expect("remove cache-maintenance fixture");
}
