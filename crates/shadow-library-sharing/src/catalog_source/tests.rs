use std::{fs, path::PathBuf};

use shadow_catalog::{Catalog, RegisterAsset};
use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use super::{CatalogSharePolicy, CatalogShareSource};
use crate::{LibraryShareSource, protocol::CapabilityAvailability};

#[test]
fn server_identity_is_stable_across_source_reopen() {
    let root = temporary_directory("server-identity");
    let catalog_path = root.join("catalog.sqlite");
    Catalog::open(&catalog_path).expect("create catalog");
    let cache = root.join("cache");
    let state = root.join("state");
    let first = CatalogShareSource::open(&catalog_path, &cache, &state, "Studio", false)
        .expect("open first source");
    let first_id = first.server_info().server_id;
    drop(first);
    let second = CatalogShareSource::open(&catalog_path, &cache, &state, "Studio", false)
        .expect("reopen source");
    assert_eq!(second.server_info().server_id, first_id);
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn explicit_roots_and_original_permission_bound_the_manifest() {
    let root = temporary_directory("root-policy");
    let shared = root.join("shared");
    let private = root.join("private");
    fs::create_dir_all(&shared).expect("create shared root");
    fs::create_dir_all(&private).expect("create private root");
    let shared_photo = shared.join("shared.nef");
    let private_photo = private.join("private.nef");
    fs::write(&shared_photo, b"shared").expect("write shared photo");
    fs::write(&private_photo, b"private").expect("write private photo");
    let catalog_path = root.join("catalog.sqlite");
    let mut catalog = Catalog::open(&catalog_path).expect("create catalog");
    let shared_asset = catalog
        .register_asset(&registration(&shared_photo, 6))
        .expect("register shared photo");
    catalog
        .register_asset(&registration(&private_photo, 7))
        .expect("register private photo");
    let source = CatalogShareSource::open_with_policy(
        &catalog_path,
        root.join("cache"),
        root.join("state"),
        "Studio",
        false,
        CatalogSharePolicy::for_roots(vec![shared.clone()], false),
    )
    .expect("open constrained source");
    assert_eq!(
        source.server_info().capabilities.serves_originals,
        CapabilityAvailability::Unavailable
    );
    let page = source.list_photos(None, 96).expect("list shared photos");
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.items[0].photo_id, shared_asset.photo_id);
    assert!(
        source
            .prepare_original(shared_asset.photo_id, shared_asset.representation_id)
            .is_err()
    );
    fs::remove_dir_all(root).expect("remove fixture");
}

#[cfg(unix)]
#[test]
fn shared_root_symlink_cannot_admit_an_outside_photo() {
    use std::os::unix::fs::symlink;

    let root = temporary_directory("root-symlink-policy");
    let shared = root.join("shared");
    let private = root.join("private");
    fs::create_dir_all(&shared).expect("create shared root");
    fs::create_dir_all(&private).expect("create private root");
    let private_photo = private.join("private.nef");
    fs::write(&private_photo, b"private").expect("write private photo");
    let shared_link = shared.join("linked-private.nef");
    symlink(&private_photo, &shared_link).expect("link outside photo into shared root");
    let catalog_path = root.join("catalog.sqlite");
    let mut catalog = Catalog::open(&catalog_path).expect("create catalog");
    catalog
        .register_asset(&registration(&shared_link, 7))
        .expect("register linked photo");
    let source = CatalogShareSource::open_with_policy(
        &catalog_path,
        root.join("cache"),
        root.join("state"),
        "Studio",
        false,
        CatalogSharePolicy::for_roots(vec![shared.canonicalize().expect("canonical shared")], true),
    )
    .expect("open constrained source");
    assert!(
        source
            .list_photos(None, 96)
            .expect("list shared photos")
            .items
            .is_empty()
    );
    fs::remove_dir_all(root).expect("remove fixture");
}

fn registration(path: &std::path::Path, byte_len: u64) -> RegisterAsset {
    RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(
            current_platform(),
            path.to_string_lossy().as_bytes().to_vec(),
            path.to_string_lossy(),
        ),
        byte_len,
        modified_at_ms: Some(1),
        now_ms: 1,
    }
}

#[cfg(target_os = "macos")]
const fn current_platform() -> Platform {
    Platform::MacOs
}

#[cfg(target_os = "windows")]
const fn current_platform() -> Platform {
    Platform::Windows
}

#[cfg(all(unix, not(target_os = "macos")))]
const fn current_platform() -> Platform {
    Platform::OtherUnix
}

fn temporary_directory(label: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!(
        "shadow-library-sharing-{label}-{}",
        uuid::Uuid::now_v7()
    ));
    fs::create_dir_all(&path).expect("create fixture directory");
    path
}
