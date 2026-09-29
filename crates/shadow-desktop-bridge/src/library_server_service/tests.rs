use std::{
    fs,
    net::SocketAddr,
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

use rusqlite::Connection;
use shadow_library_sharing::{AuthorizationToken, LibraryClient, LibraryClientConfig};
use uuid::Uuid;

use super::{
    LibraryServerService, LibraryServerSnapshot, LibraryServerStartRequest, LibraryServerStorage,
};

const TEST_TOKEN: &str = "01234567890123456789012345678901";

fn request(share_roots: Vec<std::path::PathBuf>) -> LibraryServerStartRequest {
    LibraryServerStartRequest {
        bind_address: "127.0.0.1:0".parse::<SocketAddr>().expect("address"),
        authorization: AuthorizationToken::parse(TEST_TOKEN).expect("token"),
        display_name: "Studio Mac".to_owned(),
        share_roots,
        serves_originals: false,
    }
}

fn wait_for_index_state(service: &LibraryServerService, expected: &str) -> LibraryServerSnapshot {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let snapshot = service.snapshot().expect("read server snapshot");
        if snapshot.index_state == expected {
            return snapshot;
        }
        assert!(
            Instant::now() < deadline,
            "server index did not reach {expected}; current state is {} ({})",
            snapshot.index_state,
            snapshot.index_diagnostic
        );
        thread::sleep(Duration::from_millis(10));
    }
}

#[test]
fn empty_root_set_is_rejected_without_starting_listener() {
    let root = std::env::temp_dir().join(format!("shadow-library-server-{}", Uuid::now_v7()));
    let service = LibraryServerService::new(LibraryServerStorage::for_root(&root));
    let error = service
        .start(request(Vec::new()))
        .expect_err("empty roots must fail");
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
        .start(request(vec![photos]))
        .expect("start managed server");
    assert!(running.running);
    assert_eq!(running.index_state, "scanning");
    assert!(running.local_address.is_some());
    let indexed = wait_for_index_state(&service, "ready");
    assert_eq!(indexed.photo_count, 1);
    let stopped = service.stop().expect("stop managed server");
    assert!(!stopped.running);
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn listener_accepts_clients_before_background_scan_completes() {
    let root = std::env::temp_dir().join(format!("shadow-library-server-{}", Uuid::now_v7()));
    let photos = root.join("photos");
    fs::create_dir_all(&photos).expect("create photo root");
    let hook_entered = Arc::new(AtomicBool::new(false));
    let release_hook = Arc::new(AtomicBool::new(false));
    let service = LibraryServerService::new_with_indexing_hook(
        LibraryServerStorage::for_root(root.join("service")),
        {
            let hook_entered = Arc::clone(&hook_entered);
            let release_hook = Arc::clone(&release_hook);
            Arc::new(move |cancellation| {
                hook_entered.store(true, Ordering::Release);
                while !release_hook.load(Ordering::Acquire) && !cancellation.is_cancelled() {
                    thread::sleep(Duration::from_millis(2));
                }
                Ok(())
            })
        },
    );
    let running = service
        .start(request(vec![photos]))
        .expect("start listener");
    let deadline = Instant::now() + Duration::from_secs(2);
    while !hook_entered.load(Ordering::Acquire) {
        assert!(Instant::now() < deadline, "background worker did not start");
        thread::sleep(Duration::from_millis(2));
    }
    let address = running.local_address.expect("listener address");
    let client = LibraryClient::new(LibraryClientConfig::new(
        address,
        AuthorizationToken::parse(TEST_TOKEN).expect("client token"),
    ));
    assert_eq!(
        client
            .server_info()
            .expect("connect before scan completes")
            .display_name,
        "Studio Mac"
    );
    assert_eq!(running.index_state, "scanning");
    release_hook.store(true, Ordering::Release);
    wait_for_index_state(&service, "ready");
    service.stop().expect("stop server");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn failed_reindex_keeps_the_previous_completed_catalog_available() {
    let root = std::env::temp_dir().join(format!("shadow-library-server-{}", Uuid::now_v7()));
    let photos = root.join("photos");
    fs::create_dir_all(&photos).expect("create photo root");
    fs::write(photos.join("fixture.nef"), b"not-a-real-raw").expect("write fixture");
    let attempts = Arc::new(AtomicUsize::new(0));
    let storage = LibraryServerStorage::for_root(root.join("service"));
    let service = LibraryServerService::new_with_indexing_hook(storage, {
        let attempts = Arc::clone(&attempts);
        Arc::new(move |_| {
            if attempts.fetch_add(1, Ordering::AcqRel) == 0 {
                Ok(())
            } else {
                anyhow::bail!("synthetic reindex failure")
            }
        })
    });
    service
        .start(request(vec![photos.clone()]))
        .expect("start first generation");
    assert_eq!(wait_for_index_state(&service, "ready").photo_count, 1);
    service.stop().expect("stop first generation");

    let running = service
        .start(request(vec![photos]))
        .expect("serve previous generation");
    let failed = wait_for_index_state(&service, "failed");
    assert!(failed.running);
    assert_eq!(failed.photo_count, 1);
    assert!(
        failed
            .index_diagnostic
            .contains("synthetic reindex failure")
    );
    let client = LibraryClient::new(LibraryClientConfig::new(
        running.local_address.expect("listener address"),
        AuthorizationToken::parse(TEST_TOKEN).expect("client token"),
    ));
    let page = client
        .list_photos(None, 96)
        .expect("read retained generation");
    assert_eq!(page.items.len(), 1);
    service.stop().expect("stop retained server");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn cursor_started_before_publish_continues_against_the_same_catalog_generation() {
    let root = std::env::temp_dir().join(format!("shadow-library-server-{}", Uuid::now_v7()));
    let photos = root.join("photos");
    fs::create_dir_all(&photos).expect("create photo root");
    fs::write(photos.join("one.nef"), b"one").expect("write first fixture");
    fs::write(photos.join("two.nef"), b"two").expect("write second fixture");
    let attempts = Arc::new(AtomicUsize::new(0));
    let second_hook_entered = Arc::new(AtomicBool::new(false));
    let release_second_hook = Arc::new(AtomicBool::new(false));
    let service = LibraryServerService::new_with_indexing_hook(
        LibraryServerStorage::for_root(root.join("service")),
        {
            let attempts = Arc::clone(&attempts);
            let second_hook_entered = Arc::clone(&second_hook_entered);
            let release_second_hook = Arc::clone(&release_second_hook);
            Arc::new(move |cancellation| {
                if attempts.fetch_add(1, Ordering::AcqRel) == 0 {
                    return Ok(());
                }
                second_hook_entered.store(true, Ordering::Release);
                while !release_second_hook.load(Ordering::Acquire) && !cancellation.is_cancelled() {
                    thread::sleep(Duration::from_millis(2));
                }
                Ok(())
            })
        },
    );
    service
        .start(request(vec![photos.clone()]))
        .expect("start first generation");
    assert_eq!(wait_for_index_state(&service, "ready").photo_count, 2);
    service.stop().expect("stop first generation");
    fs::write(photos.join("three.nef"), b"three").expect("write third fixture");

    let running = service
        .start(request(vec![photos]))
        .expect("serve first generation while rebuilding");
    let deadline = Instant::now() + Duration::from_secs(2);
    while !second_hook_entered.load(Ordering::Acquire) {
        assert!(Instant::now() < deadline, "second index did not start");
        thread::sleep(Duration::from_millis(2));
    }
    let client = LibraryClient::new(LibraryClientConfig::new(
        running.local_address.expect("listener address"),
        AuthorizationToken::parse(TEST_TOKEN).expect("client token"),
    ));
    let first_page = client
        .list_photos(None, 1)
        .expect("read first page from published generation");
    assert_eq!(first_page.items.len(), 1);
    let old_cursor = first_page.next_cursor.expect("old generation cursor");

    release_second_hook.store(true, Ordering::Release);
    assert_eq!(wait_for_index_state(&service, "ready").photo_count, 3);
    let old_second_page = client
        .list_photos(Some(old_cursor), 96)
        .expect("continue old generation after publication");
    assert_eq!(old_second_page.items.len(), 1);
    assert!(old_second_page.next_cursor.is_none());
    assert_eq!(
        client
            .list_photos(None, 96)
            .expect("read newly published generation")
            .items
            .len(),
        3
    );
    service.stop().expect("stop server");

    service
        .start(request(vec![root.join("photos")]))
        .expect("start third generation");
    wait_for_index_state(&service, "ready");
    service.stop().expect("stop third generation");
    let retained_generations = fs::read_dir(
        root.join("service")
            .join("state")
            .join("catalog-generations"),
    )
    .expect("read retained generations")
    .filter_map(Result::ok)
    .filter(|entry| entry.file_type().is_ok_and(|kind| kind.is_dir()))
    .count();
    assert_eq!(retained_generations, 2);
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn stop_cancels_and_joins_background_scan_without_deadlock() {
    let root = std::env::temp_dir().join(format!("shadow-library-server-{}", Uuid::now_v7()));
    let photos = root.join("photos");
    fs::create_dir_all(&photos).expect("create photo root");
    let hook_entered = Arc::new(AtomicBool::new(false));
    let hook_exited = Arc::new(AtomicBool::new(false));
    let service = LibraryServerService::new_with_indexing_hook(
        LibraryServerStorage::for_root(root.join("service")),
        {
            let hook_entered = Arc::clone(&hook_entered);
            let hook_exited = Arc::clone(&hook_exited);
            Arc::new(move |cancellation| {
                hook_entered.store(true, Ordering::Release);
                while !cancellation.is_cancelled() {
                    thread::sleep(Duration::from_millis(2));
                }
                hook_exited.store(true, Ordering::Release);
                Ok(())
            })
        },
    );
    service
        .start(request(vec![photos]))
        .expect("start listener");
    let deadline = Instant::now() + Duration::from_secs(2);
    while !hook_entered.load(Ordering::Acquire) {
        assert!(Instant::now() < deadline, "background worker did not start");
        thread::sleep(Duration::from_millis(2));
    }
    let started = Instant::now();
    let stopped = service.stop().expect("cancel and stop server");
    assert!(started.elapsed() < Duration::from_secs(2));
    assert!(!stopped.running);
    assert_eq!(stopped.index_state, "cancelled");
    assert!(hook_exited.load(Ordering::Acquire));
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
    assert_eq!(identity, "shadow-catalog-20260821.1-photo-relationships");
    fs::remove_dir_all(root).expect("remove fixture");
}
