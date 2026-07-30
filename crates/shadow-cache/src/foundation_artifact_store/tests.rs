use std::{
    fs,
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
};

use super::*;

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

fn fixture_root(label: &str) -> PathBuf {
    let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let root = std::env::temp_dir().join(format!(
        "shadow-foundation-store-{label}-{}-{sequence}",
        std::process::id()
    ));
    fs::create_dir_all(&root).expect("create store fixture");
    root
}

#[test]
fn cache_keys_and_partial_ownership_fail_closed() {
    let root = fixture_root("ownership");
    let store = FoundationArtifactStore::open(&root).expect("open store");

    assert!(matches!(
        store.allocate_partial_path("not-a-digest"),
        Err(FoundationArtifactStoreError::InvalidCacheKey)
    ));
    assert!(matches!(
        store.discard_partial(root.join("foreign.shadowrawf")),
        Err(FoundationArtifactStoreError::UnownedPartial(_))
    ));
}

#[test]
fn cancellation_discards_only_the_owned_partial() {
    let root = fixture_root("cancel");
    let store = FoundationArtifactStore::open(&root).expect("open store");
    let cache_key = "1".repeat(64);
    let partial = store
        .allocate_partial_path(&cache_key)
        .expect("allocate partial");
    fs::write(&partial, b"incomplete").expect("write incomplete partial");

    store.discard_partial(&partial).expect("discard partial");
    assert!(!partial.exists());
    store
        .discard_partial(&partial)
        .expect("discard remains idempotent");
}

#[test]
fn real_artifact_publication_and_recovery_are_optionally_verified() {
    let Some(source) = std::env::var_os("SHADOW_TEST_FOUNDATION_ARTIFACT") else {
        return;
    };
    let source = PathBuf::from(source);
    let source_reader = FoundationArtifactReader::open(&source).expect("verify source artifact");
    let cache_key = source_reader.verification().cache_key_sha256.clone();
    let root = fixture_root("real");
    let store = FoundationArtifactStore::open(&root).expect("open store");

    let first_partial = copy_to_owned_partial(&store, &cache_key, &source);
    let first = store
        .publish_verified_partial(&cache_key, &first_partial)
        .expect("publish first artifact");
    assert_eq!(first.status, FoundationArtifactPublicationStatus::Published);
    assert!(!first_partial.exists());

    let second_partial = copy_to_owned_partial(&store, &cache_key, &source);
    let second = store
        .recover_completed_partial(&second_partial)
        .expect("recover exact duplicate");
    assert_eq!(
        second.status,
        FoundationArtifactPublicationStatus::ReusedExisting
    );
    assert_eq!(
        second.verification.artifact_identity_sha256,
        first.verification.artifact_identity_sha256
    );

    let mut cached = store
        .lookup_verified(&cache_key)
        .expect("lookup cached artifact")
        .expect("cached artifact exists");
    assert_eq!(cached.verification().path, first.path);
    let rows = cached
        .read_interleaved_rows(540, 36)
        .expect("read cached cross-stripe rows");
    assert_eq!(rows.len(), 36 * 5_202 * 3);
    assert!(rows.iter().all(|value| value.is_finite()));

    fs::remove_dir_all(root).expect("remove external test cache");
}

fn copy_to_owned_partial(
    store: &FoundationArtifactStore,
    cache_key: &str,
    source: &Path,
) -> PathBuf {
    let partial = store
        .allocate_partial_path(cache_key)
        .expect("allocate owned partial");
    fs::copy(source, &partial).expect("copy real artifact to owned partial");
    partial
}
