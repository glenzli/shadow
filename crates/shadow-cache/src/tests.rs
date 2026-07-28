use super::*;

#[test]
fn duplicate_content_reuses_one_verified_blob() {
    let root = fixture_root("reuse");
    let store = ContentAddressedStore::open(&root).expect("open cache");
    let first = store.put(b"preview bytes").expect("first put");
    let second = store.put(b"preview bytes").expect("second put");

    assert_eq!(first, second);
    assert_eq!(
        fs::read(store.resolve(first.digest)).expect("read blob"),
        b"preview bytes"
    );
    assert_eq!(first.digest.to_hex().len(), 64);
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn corrupt_existing_blob_is_never_silently_reused() {
    let root = fixture_root("corrupt");
    let store = ContentAddressedStore::open(&root).expect("open cache");
    let blob = store.put(b"correct").expect("put blob");
    fs::write(store.resolve(blob.digest), b"corrupt").expect("corrupt blob");

    assert!(matches!(
        store.put(b"correct"),
        Err(CacheError::CorruptBlob(_))
    ));
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn lazy_read_verifies_bytes_before_returning_them() {
    let root = fixture_root("verified-read");
    let store = ContentAddressedStore::open(&root).expect("open cache");
    let blob = store.put(b"display proxy").expect("put blob");
    assert_eq!(
        store.read_verified(blob.digest).expect("verified read"),
        b"display proxy"
    );

    fs::write(store.resolve(blob.digest), b"tampered proxy").expect("tamper blob");
    assert!(matches!(
        store.read_verified(blob.digest),
        Err(CacheError::CorruptBlob(_))
    ));
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn corrupt_blob_is_quarantined_and_can_be_regenerated() {
    let root = fixture_root("quarantine");
    let store = ContentAddressedStore::open(&root).expect("open cache");
    let blob = store.put(b"correct proxy").expect("put blob");
    fs::write(store.resolve(blob.digest), b"corrupt proxy").expect("corrupt blob");

    let status = store
        .quarantine_corrupt(blob.digest)
        .expect("quarantine corrupt blob");
    let QuarantineStatus::Quarantined { relative_path } = status else {
        panic!("expected quarantined blob, found {status:?}");
    };
    assert!(!store.resolve(blob.digest).exists());
    assert_eq!(
        fs::read(root.join(relative_path)).expect("read quarantined bytes"),
        b"corrupt proxy"
    );

    let regenerated = store.put(b"correct proxy").expect("regenerate blob");
    assert_eq!(regenerated.digest, blob.digest);
    assert_eq!(
        store
            .read_verified(regenerated.digest)
            .expect("read regenerated blob"),
        b"correct proxy"
    );
    fs::remove_dir_all(root).expect("remove fixture");
}

fn fixture_root(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!(
        "shadow-cache-{name}-{}-{}",
        std::process::id(),
        TEMP_FILE_SEQUENCE.fetch_add(1, Ordering::Relaxed)
    ))
}
