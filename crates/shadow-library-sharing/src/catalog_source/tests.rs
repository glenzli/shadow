use std::{fs, path::PathBuf};

use shadow_catalog::Catalog;

use super::CatalogShareSource;
use crate::LibraryShareSource;

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

fn temporary_directory(label: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!(
        "shadow-library-sharing-{label}-{}",
        uuid::Uuid::now_v7()
    ));
    fs::create_dir_all(&path).expect("create fixture directory");
    path
}
