use std::{fs, path::PathBuf};

use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use super::*;
use crate::{Catalog, RegisterAsset, create_catalog_backup};

struct TempDirectory(PathBuf);

impl TempDirectory {
    fn new() -> Self {
        let path =
            std::env::temp_dir().join(format!("shadow-catalog-restore-test-{}", Uuid::now_v7()));
        fs::create_dir_all(&path).expect("create restore test directory");
        Self(path)
    }

    fn join(&self, path: &str) -> PathBuf {
        self.0.join(path)
    }
}

impl Drop for TempDirectory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn request(path: &str, now_ms: i64) -> RegisterAsset {
    RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
        byte_len: 42,
        modified_at_ms: Some(now_ms),
        now_ms,
    }
}

#[test]
fn offline_restore_replaces_catalog_and_preserves_previous_file_set() {
    let directory = TempDirectory::new();
    let catalog_path = directory.join("catalog.sqlite");
    let backup_path = directory.join("backup.sqlite");
    let mut catalog = Catalog::open(&catalog_path).expect("open current Catalog");
    catalog
        .register_asset(&request("/photos/one.nef", 1))
        .expect("register backup photo");
    create_catalog_backup(&catalog_path, &backup_path).expect("create source backup");
    catalog
        .register_asset(&request("/photos/two.nef", 2))
        .expect("register later current photo");
    drop(catalog);

    fs::write(
        sidecar_path(&catalog_path, "-wal"),
        b"preserved wal evidence",
    )
    .expect("seed stale WAL evidence");
    fs::write(
        sidecar_path(&catalog_path, "-shm"),
        b"preserved shm evidence",
    )
    .expect("seed stale SHM evidence");

    let receipt = restore_catalog_backup_offline(&backup_path, &catalog_path)
        .expect("restore verified backup over offline Catalog");

    assert_eq!(receipt.source_verification.stats.photos, 1);
    assert_eq!(receipt.restored_verification.stats.photos, 1);
    let restored = Catalog::open(&catalog_path).expect("open restored Catalog");
    assert_eq!(restored.stats().expect("restored stats").photos, 1);
    drop(restored);

    let rollback = receipt
        .rollback_directory
        .expect("replacement retains one rollback directory");
    assert_eq!(
        fs::read(rollback.join("catalog.sqlite-wal")).expect("read preserved WAL"),
        b"preserved wal evidence"
    );
    assert_eq!(
        fs::read(rollback.join("catalog.sqlite-shm")).expect("read preserved SHM"),
        b"preserved shm evidence"
    );
    fs::remove_file(rollback.join("catalog.sqlite-wal")).expect("remove synthetic WAL evidence");
    fs::remove_file(rollback.join("catalog.sqlite-shm")).expect("remove synthetic SHM evidence");
    let previous =
        Catalog::open(&rollback.join("catalog.sqlite")).expect("open displaced previous Catalog");
    assert_eq!(previous.stats().expect("previous stats").photos, 2);
    assert_eq!(
        crate::verify_catalog_backup(&backup_path)
            .expect("source backup remains valid")
            .stats
            .photos,
        1
    );
}

#[test]
fn offline_restore_can_publish_a_new_catalog_path() {
    let directory = TempDirectory::new();
    let source = directory.join("source.sqlite");
    let backup = directory.join("backup.sqlite");
    let destination = directory.join("restored/catalog.sqlite");
    let mut catalog = Catalog::open(&source).expect("open source Catalog");
    catalog
        .register_asset(&request("/photos/one.nef", 1))
        .expect("register source photo");
    create_catalog_backup(&source, &backup).expect("create source backup");
    drop(catalog);

    let receipt = restore_catalog_backup_offline(&backup, &destination)
        .expect("publish verified restore to a new path");

    assert!(receipt.rollback_directory.is_none());
    assert_eq!(
        Catalog::open(&destination)
            .expect("open restored destination")
            .stats()
            .expect("restored destination stats")
            .photos,
        1
    );
}

#[test]
fn invalid_or_self_restore_never_changes_the_destination() {
    let directory = TempDirectory::new();
    let destination = directory.join("catalog.sqlite");
    let corrupt = directory.join("corrupt.sqlite");
    let mut catalog = Catalog::open(&destination).expect("open destination Catalog");
    catalog
        .register_asset(&request("/photos/current.nef", 1))
        .expect("register current photo");
    drop(catalog);
    fs::write(&corrupt, b"not a SQLite backup").expect("write corrupt backup");

    assert!(restore_catalog_backup_offline(&corrupt, &destination).is_err());
    assert_eq!(
        Catalog::open(&destination)
            .expect("reopen unchanged destination")
            .stats()
            .expect("unchanged stats")
            .photos,
        1
    );
    assert!(matches!(
        restore_catalog_backup_offline(&destination, &destination),
        Err(CatalogRestoreError::SourceEqualsDestination(path))
            if path == destination.canonicalize().expect("canonical self-restore path")
    ));
}
