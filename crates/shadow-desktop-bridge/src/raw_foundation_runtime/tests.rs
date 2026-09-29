use std::{cell::Cell, path::PathBuf, sync::Arc, thread, time::Duration};

use shadow_ai::CancellationToken;
use shadow_catalog::RepresentationFingerprint;

use super::{
    PreparedRawFoundationSourceCache, PreparedRawFoundationSourceIdentity, RawFoundationRuntime,
    config::RawFoundationRuntimePaths,
};

fn identity(
    path: &str,
    byte_len: u64,
    modified_at_ms: i64,
    source_sha256: &str,
) -> PreparedRawFoundationSourceIdentity {
    PreparedRawFoundationSourceIdentity {
        source_path: PathBuf::from(path),
        source: RepresentationFingerprint {
            byte_len,
            modified_at_ms: Some(modified_at_ms),
        },
        source_sha256: source_sha256.into(),
    }
}

#[test]
fn exact_source_identity_reuses_one_preparation() {
    let prepared_count = Cell::new(0_u32);
    let source = identity("source.nef", 42, 7, "a");
    let mut cache = PreparedRawFoundationSourceCache::default();

    let first = cache
        .get_or_prepare(source.clone(), || {
            prepared_count.set(prepared_count.get() + 1);
            Ok::<_, ()>(11_u8)
        })
        .expect("prepare source");
    let second = cache
        .get_or_prepare(source, || {
            prepared_count.set(prepared_count.get() + 1);
            Ok::<_, ()>(22_u8)
        })
        .expect("reuse source");

    assert_eq!(prepared_count.get(), 1);
    assert!(Arc::ptr_eq(&first, &second));
    assert_eq!(*second, 11);
}

#[test]
fn path_fingerprint_or_hash_change_replaces_the_preparation() {
    let prepared_count = Cell::new(0_u32);
    let mut cache = PreparedRawFoundationSourceCache::default();
    let identities = [
        identity("source.nef", 42, 7, "a"),
        identity("other.nef", 42, 7, "a"),
        identity("other.nef", 43, 7, "a"),
        identity("other.nef", 43, 8, "a"),
        identity("other.nef", 43, 8, "b"),
    ];

    let mut previous = None;
    for source in identities {
        let prepared = cache
            .get_or_prepare(source, || {
                prepared_count.set(prepared_count.get() + 1);
                Ok::<_, ()>(prepared_count.get())
            })
            .expect("prepare changed source");
        if let Some(previous) = previous {
            assert!(!Arc::ptr_eq(&previous, &prepared));
        }
        previous = Some(prepared);
    }

    assert_eq!(prepared_count.get(), 5);
    assert_eq!(cache.entry.as_ref().map(|entry| *entry.payload), Some(5));
}

#[test]
fn cancellation_stops_waiting_for_an_inflight_source_preparation() {
    let root = tempfile::tempdir().expect("runtime root");
    let runtime = Arc::new(
        RawFoundationRuntime::open(RawFoundationRuntimePaths {
            foundation_store_root: root.path().join("foundations"),
            raw_frame_staging_root: root.path().join("staging"),
            infer_base_url_override: None,
            infer_credential_file: root.path().join("credential"),
        })
        .expect("runtime"),
    );
    let held_preparation = runtime
        .prepared_source
        .lock()
        .expect("hold source preparation");
    let cancellation = CancellationToken::default();
    let worker_runtime = Arc::clone(&runtime);
    let worker_cancellation = cancellation.clone();
    let worker = thread::spawn(move || {
        worker_runtime
            .prepared_source_guard(&worker_cancellation)
            .expect("wait for source preparation")
            .is_none()
    });

    thread::sleep(Duration::from_millis(25));
    cancellation.cancel();
    assert!(worker.join().expect("join cancelled waiter"));
    drop(held_preparation);
}
