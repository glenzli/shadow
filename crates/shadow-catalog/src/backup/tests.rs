use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use super::*;
use crate::{Catalog, RegisterAsset};

struct TempDirectory(PathBuf);

impl TempDirectory {
    fn new() -> Self {
        let path =
            std::env::temp_dir().join(format!("shadow-catalog-backup-test-{}", Uuid::now_v7()));
        fs::create_dir_all(&path).expect("create backup test directory");
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
fn online_backup_is_a_stable_verified_snapshot_of_live_wal_state() {
    let directory = TempDirectory::new();
    let source = directory.join("catalog.sqlite");
    let destination = directory.join("backups/catalog-1.sqlite");
    let mut catalog = Catalog::open(&source).expect("open source catalog");
    catalog
        .register_asset(&request("/photos/one.nef", 1))
        .expect("register first asset");

    let receipt =
        create_catalog_backup(&source, &destination).expect("create verified online backup");
    assert_eq!(
        receipt.destination,
        destination
            .parent()
            .expect("backup parent")
            .canonicalize()
            .expect("canonical backup parent")
            .join(destination.file_name().expect("backup file name"))
    );
    assert_eq!(receipt.verification.schema_version, SCHEMA_VERSION);
    assert_eq!(receipt.verification.stats.photos, 1);
    assert!(receipt.verification.page_count > 0);
    assert!(receipt.verification.page_size > 0);
    assert!(receipt.verification.database_bytes > 0);

    catalog
        .register_asset(&request("/photos/two.nef", 2))
        .expect("register source after snapshot");
    assert_eq!(catalog.stats().expect("source stats").photos, 2);
    assert_eq!(
        verify_catalog_backup(&destination)
            .expect("repeat restore drill")
            .stats
            .photos,
        1,
        "later source writes cannot rewrite the published backup snapshot"
    );

    drop(catalog);
    let restored = Catalog::open(&destination).expect("open backup as restored catalog");
    assert_eq!(restored.stats().expect("restored stats").photos, 1);
}

#[test]
fn existing_destination_is_preserved() {
    let directory = TempDirectory::new();
    let source = directory.join("catalog.sqlite");
    let destination = directory.join("existing.sqlite");
    let _catalog = Catalog::open(&source).expect("open source catalog");
    fs::write(&destination, b"do not replace").expect("seed destination");

    assert!(matches!(
        create_catalog_backup(&source, &destination),
        Err(CatalogBackupError::DestinationExists(path))
            if path == destination.canonicalize().expect("canonical destination")
    ));
    assert_eq!(
        fs::read(&destination).expect("read preserved destination"),
        b"do not replace"
    );
}

#[test]
fn corrupt_file_fails_the_restore_drill() {
    let directory = TempDirectory::new();
    let corrupt = directory.join("corrupt.sqlite");
    fs::write(&corrupt, b"not a sqlite database").expect("write corrupt fixture");

    assert!(verify_catalog_backup(&corrupt).is_err());
}
