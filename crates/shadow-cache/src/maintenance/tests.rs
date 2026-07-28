use std::{
    collections::BTreeSet,
    fs,
    path::PathBuf,
    sync::atomic::Ordering,
    time::{Duration, SystemTime},
};

use super::DEFAULT_SWEEP_GRACE_PERIOD;
use crate::{ALGORITHM_DIRECTORY, ContentAddressedStore, TEMP_FILE_SEQUENCE};

#[test]
fn inventory_and_sweep_keep_catalog_references_and_preserve_unknown_files() {
    let root = fixture_root("sweep");
    let store = ContentAddressedStore::open(&root).expect("open cache");
    let retained = store.put(b"current recipe preview").expect("put retained");
    let stale = store.put(b"superseded thumbnail").expect("put stale");
    let diagnostics = root.join("blobs").join(ALGORITHM_DIRECTORY).join("notes");
    fs::create_dir_all(&diagnostics).expect("create diagnostics directory");
    let unknown = diagnostics.join("keep-for-diagnosis.txt");
    fs::write(&unknown, b"not a canonical cache blob").expect("write unknown file");

    let inventory = store.inventory().expect("inventory");
    assert_eq!(inventory.blobs.len(), 2);
    assert_eq!(inventory.total_byte_len, retained.byte_len + stale.byte_len);
    assert_eq!(inventory.unknown_relative_paths.len(), 1);

    let retained_digests = BTreeSet::from([retained.digest]);
    let protected = store
        .sweep_unreferenced(&retained_digests, true)
        .expect("recent blobs are protected from a default dry sweep");
    assert!(protected.dry_run);
    assert_eq!(protected.reclaimed.len(), 0);
    assert_eq!(protected.recently_protected_blob_count, 1);
    assert_eq!(protected.recently_protected_byte_len, stale.byte_len);

    let dry_run = store
        .sweep_inventory_at(
            &inventory,
            &retained_digests,
            true,
            SystemTime::now() + DEFAULT_SWEEP_GRACE_PERIOD + Duration::from_secs(1),
            DEFAULT_SWEEP_GRACE_PERIOD,
        )
        .expect("dry sweep after the explicit grace window");
    assert!(dry_run.dry_run);
    assert_eq!(dry_run.retained_blob_count, 1);
    assert_eq!(dry_run.reclaimed.len(), 1);
    assert_eq!(dry_run.reclaimed[0].digest, stale.digest);
    assert_eq!(dry_run.reclaimed[0].byte_len, stale.byte_len);
    assert!(store.resolve(stale.digest).exists());

    let sweep = store
        .sweep_inventory_at(
            &inventory,
            &retained_digests,
            false,
            SystemTime::now() + DEFAULT_SWEEP_GRACE_PERIOD + Duration::from_secs(1),
            DEFAULT_SWEEP_GRACE_PERIOD,
        )
        .expect("sweep after the explicit grace window");
    assert!(!sweep.dry_run);
    assert_eq!(sweep.reclaimed.len(), 1);
    assert_eq!(sweep.reclaimed[0].digest, stale.digest);
    assert!(store.resolve(retained.digest).exists());
    assert!(!store.resolve(sweep.reclaimed[0].digest).exists());
    assert!(unknown.exists());
    fs::remove_dir_all(root).expect("remove fixture");
}

fn fixture_root(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!(
        "shadow-cache-maintenance-{name}-{}-{}",
        std::process::id(),
        TEMP_FILE_SEQUENCE.fetch_add(1, Ordering::Relaxed)
    ))
}
