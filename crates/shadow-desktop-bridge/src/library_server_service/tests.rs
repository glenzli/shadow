use std::{fs, net::SocketAddr};

use rusqlite::Connection;
use shadow_library_sharing::AuthorizationToken;
use uuid::Uuid;

use super::{LibraryServerService, LibraryServerStartRequest, LibraryServerStorage};

#[test]
fn empty_root_set_is_rejected_without_starting_listener() {
    let root = std::env::temp_dir().join(format!("shadow-library-server-{}", Uuid::now_v7()));
    let service = LibraryServerService::new(LibraryServerStorage::for_root(&root));
    let request = LibraryServerStartRequest {
        bind_address: "127.0.0.1:0".parse::<SocketAddr>().expect("address"),
        authorization: AuthorizationToken::parse("01234567890123456789012345678901")
            .expect("token"),
        display_name: "Studio Mac".to_owned(),
        share_roots: Vec::new(),
        serves_originals: true,
    };
    let error = service.start(request).expect_err("empty roots must fail");
    assert!(error.to_string().contains("at least one folder"));
    assert!(!root.exists());
}

#[test]
fn cache_reset_preserves_stable_state_directory() {
    let root = std::env::temp_dir().join(format!("shadow-library-server-{}", Uuid::now_v7()));
    let storage = LibraryServerStorage::for_root(&root);
    fs::create_dir_all(&storage.preview_cache_root).expect("create preview cache");
    fs::write(storage.preview_cache_root.join("blob"), b"cache").expect("write cache");
    fs::create_dir_all(&storage.state_root).expect("create state");
    fs::write(storage.state_root.join("server-id"), b"stable").expect("write state");
    let service = LibraryServerService::new(storage.clone());
    service.reset_cache().expect("reset cache");
    assert!(!storage.preview_cache_root.exists());
    assert_eq!(
        fs::read(storage.state_root.join("server-id")).expect("read state"),
        b"stable"
    );
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn managed_listener_scans_configured_root_and_stops_cleanly() {
    let root = std::env::temp_dir().join(format!("shadow-library-server-{}", Uuid::now_v7()));
    let photos = root.join("photos");
    fs::create_dir_all(&photos).expect("create photo root");
    fs::write(photos.join("fixture.nef"), b"not-a-real-raw").expect("write fixture");
    let service = LibraryServerService::new(LibraryServerStorage::for_root(root.join("service")));
    let running = service
        .start(LibraryServerStartRequest {
            bind_address: "127.0.0.1:0".parse::<SocketAddr>().expect("address"),
            authorization: AuthorizationToken::parse("01234567890123456789012345678901")
                .expect("token"),
            display_name: "Studio Mac".to_owned(),
            share_roots: vec![photos],
            serves_originals: false,
        })
        .expect("start managed server");
    assert!(running.running);
    assert_eq!(running.photo_count, 1);
    assert!(running.local_address.is_some());
    let stopped = service.stop().expect("stop managed server");
    assert!(!stopped.running);
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn incompatible_rebuildable_catalog_is_recreated_before_scanning() {
    let root = std::env::temp_dir().join(format!("shadow-library-server-{}", Uuid::now_v7()));
    let storage = LibraryServerStorage::for_root(root.join("service"));
    fs::create_dir_all(storage.catalog_path.parent().expect("catalog parent"))
        .expect("create service root");
    let legacy = Connection::open(&storage.catalog_path).expect("open legacy catalog");
    legacy
        .execute_batch(
            "CREATE TABLE catalog_schema (
                 version INTEGER PRIMARY KEY NOT NULL,
                 identity TEXT NOT NULL,
                 created_at_ms INTEGER NOT NULL
             ) STRICT;
             INSERT INTO catalog_schema(version, identity, created_at_ms)
             VALUES (1, 'shadow-catalog-v1-older-development-schema', 1);",
        )
        .expect("write legacy schema marker");
    drop(legacy);

    let service = LibraryServerService::new(storage.clone());
    service
        .open_catalog_actor()
        .expect("rebuild incompatible server Catalog")
        .shutdown()
        .expect("stop rebuilt Catalog actor");
    let identity: String = Connection::open(&storage.catalog_path)
        .expect("reopen rebuilt catalog")
        .query_row("SELECT identity FROM catalog_schema", [], |row| row.get(0))
        .expect("read rebuilt schema identity");
    assert_eq!(identity, "shadow-catalog-20260809.2-photo-relationships");
    fs::remove_dir_all(root).expect("remove fixture");
}
