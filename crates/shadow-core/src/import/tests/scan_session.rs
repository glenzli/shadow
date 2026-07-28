use std::{
    fs,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
};

use shadow_catalog::{Catalog, CatalogActor, ImportSessionState};
use shadow_domain::{EntityId, PhotoId};

use crate::{
    import::{
        ScanCancellation, ScanCompletion, ScanPhase, resume_scan, scan_folder,
        scan_folder_controlled, scan_folder_profiled_controlled, scan_session::now_ms,
    },
    native_path::encode_location,
};

#[test]
fn scan_is_recursive_filtered_and_idempotent() {
    let root = std::env::temp_dir().join(format!("shadow-scan-{}", PhotoId::new_v7()));
    let nested = root.join("nested");
    fs::create_dir_all(&nested).expect("create fixture directory");
    fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
    fs::write(nested.join("two.jpg"), b"jpeg").expect("write jpeg fixture");
    fs::write(root.join("notes.txt"), b"ignore").expect("write ignored fixture");

    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let first = scan_folder(&mut catalog, &root).expect("first scan");
    let second = scan_folder(&mut catalog, &root).expect("second scan");

    assert_eq!(first.inserted, 2);
    assert_eq!(first.skipped, 1);
    assert_eq!(second.unchanged, 2);
    assert_eq!(catalog.stats().expect("stats").photos, 2);
    assert_eq!(
        catalog
            .import_session_summary(first.session_id)
            .expect("first session summary")
            .inserted,
        2
    );

    fs::remove_dir_all(&root).expect("remove fixture directory");
}

#[test]
fn controlled_scan_progress_is_monotonic_and_terminal() {
    let root = std::env::temp_dir().join(format!("shadow-progress-{}", PhotoId::new_v7()));
    fs::create_dir_all(&root).expect("create fixture directory");
    fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
    fs::write(root.join("two.jpg"), b"jpeg").expect("write raster fixture");
    fs::write(root.join("notes.txt"), b"ignore").expect("write ignored fixture");

    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let mut progress = Vec::new();
    let report =
        scan_folder_controlled(&mut catalog, &root, &ScanCancellation::new(), |snapshot| {
            progress.push(snapshot.clone());
        })
        .expect("controlled scan");

    assert_eq!(report.completion, ScanCompletion::Completed);
    assert_eq!(
        progress.first().expect("initial progress").phase,
        ScanPhase::Discovering
    );
    assert_eq!(
        progress.last().expect("terminal progress").phase,
        ScanPhase::Completed
    );
    assert_eq!(
        progress.last().expect("terminal progress").files_seen,
        report.files_seen
    );
    for pair in progress.windows(2) {
        let [before, after] = pair else {
            unreachable!("windows of two always contain two elements")
        };
        assert!(before.files_seen <= after.files_seen);
        assert!(before.supported_files <= after.supported_files);
        assert!(before.inserted <= after.inserted);
        assert!(before.unchanged <= after.unchanged);
        assert!(before.needs_revalidation <= after.needs_revalidation);
        assert!(before.decode_inspections_queued <= after.decode_inspections_queued);
        assert!(before.skipped <= after.skipped);
        assert!(before.issue_count <= after.issue_count);
    }

    fs::remove_dir_all(&root).expect("remove fixture directory");
}

#[test]
fn profiled_scan_routes_scanner_operations_without_per_file_samples() {
    let root = std::env::temp_dir().join(format!("shadow-profiled-scan-{}", PhotoId::new_v7()));
    let nested = root.join("nested");
    fs::create_dir_all(&nested).expect("create fixture directory");
    fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
    fs::write(nested.join("two.jpg"), b"jpeg").expect("write raster fixture");
    fs::write(root.join("notes.txt"), b"ignore").expect("write ignored fixture");

    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let callback_count = Arc::new(AtomicUsize::new(0));
    let observed_callbacks = Arc::clone(&callback_count);
    let profiled =
        scan_folder_profiled_controlled(&mut catalog, &root, &ScanCancellation::new(), move |_| {
            observed_callbacks.fetch_add(1, Ordering::SeqCst);
        })
        .expect("profile scan");

    assert_eq!(profiled.report.supported_files, 2);
    assert!(profiled.performance.profiled);
    assert_eq!(profiled.performance.metadata_stat.samples, 2);
    assert_eq!(profiled.performance.discovered_journal.samples, 2);
    assert_eq!(profiled.performance.asset_registration.samples, 2);
    assert_eq!(profiled.performance.decode_current_query.samples, 0);
    assert_eq!(profiled.performance.decode_submit_wait.samples, 0);
    assert_eq!(profiled.performance.decode_queue_full_events, 0);
    assert_eq!(
        profiled.performance.progress_callback.samples,
        u64::try_from(callback_count.load(Ordering::SeqCst)).expect("callback count fits u64")
    );
    assert!(profiled.performance.discovery_io.samples >= 2);
    assert!(profiled.performance.first_catalogued_ms.is_some());

    fs::remove_dir_all(&root).expect("remove fixture directory");
}

#[test]
fn cancellation_is_journaled_before_registration_and_a_new_scan_is_idempotent() {
    let root = std::env::temp_dir().join(format!("shadow-cancel-{}", PhotoId::new_v7()));
    fs::create_dir_all(&root).expect("create fixture directory");
    fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");

    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let cancellation = ScanCancellation::new();
    let callback_token = cancellation.clone();
    let mut progress = Vec::new();
    let cancelled = scan_folder_controlled(&mut catalog, &root, &cancellation, |snapshot| {
        progress.push(snapshot.clone());
        if snapshot.supported_files == 1 {
            callback_token.cancel();
        }
    })
    .expect("cancel scan");

    assert_eq!(cancelled.completion, ScanCompletion::Cancelled);
    assert_eq!(cancelled.inserted, 0);
    assert_eq!(catalog.stats().expect("catalog stats").photos, 0);
    assert_eq!(
        progress.last().expect("terminal progress").phase,
        ScanPhase::Cancelled
    );
    assert_eq!(
        catalog
            .import_session_summary(cancelled.session_id)
            .expect("cancelled session")
            .session
            .state,
        ImportSessionState::Cancelled
    );

    let completed = scan_folder(&mut catalog, &root).expect("fresh scan after cancellation");
    assert_eq!(completed.completion, ScanCompletion::Completed);
    assert_eq!(completed.inserted, 1);
    let repeated = scan_folder(&mut catalog, &root).expect("repeat completed scan");
    assert_eq!(repeated.inserted, 0);
    assert_eq!(repeated.unchanged, 1);
    assert_eq!(catalog.stats().expect("catalog stats").photos, 1);

    fs::remove_dir_all(&root).expect("remove fixture directory");
}

#[test]
fn cancellation_after_one_registration_stops_before_the_next_asset() {
    let root = std::env::temp_dir().join(format!("shadow-partial-{}", PhotoId::new_v7()));
    fs::create_dir_all(&root).expect("create fixture directory");
    fs::write(root.join("one.NEF"), b"raw one").expect("write first fixture");
    fs::write(root.join("two.NEF"), b"raw two").expect("write second fixture");

    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let cancellation = ScanCancellation::new();
    let callback_token = cancellation.clone();
    let cancelled = scan_folder_controlled(&mut catalog, &root, &cancellation, |snapshot| {
        if snapshot.inserted == 1 {
            callback_token.cancel();
        }
    })
    .expect("partial scan");
    assert_eq!(cancelled.completion, ScanCompletion::Cancelled);
    assert_eq!(cancelled.inserted, 1);

    let completed = scan_folder(&mut catalog, &root).expect("new idempotent scan");
    assert_eq!(completed.completion, ScanCompletion::Completed);
    assert_eq!(completed.inserted, 1);
    assert_eq!(completed.unchanged, 1);

    fs::remove_dir_all(&root).expect("remove fixture directory");
}

#[test]
fn interrupted_session_can_be_resumed() {
    let root = std::env::temp_dir().join(format!("shadow-resume-{}", PhotoId::new_v7()));
    fs::create_dir_all(&root).expect("create fixture directory");
    fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");

    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let session_id = catalog
        .begin_import_session(&encode_location(&root), now_ms())
        .expect("begin interrupted session");
    let report = resume_scan(&mut catalog, session_id).expect("resume scan");

    assert_eq!(report.session_id, session_id);
    assert_eq!(report.inserted, 1);
    assert_eq!(
        catalog
            .import_session_summary(session_id)
            .expect("session summary")
            .session
            .state,
        ImportSessionState::Completed
    );

    fs::remove_dir_all(&root).expect("remove fixture directory");
}

#[test]
fn production_actor_keeps_scanning_off_the_writer_thread() {
    let test_id = PhotoId::new_v7();
    let root = std::env::temp_dir().join(format!("shadow-actor-scan-{test_id}"));
    let database_path = std::env::temp_dir().join(format!("shadow-actor-{test_id}.sqlite"));
    fs::create_dir_all(&root).expect("create fixture directory");
    fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");

    let actor = CatalogActor::spawn(&database_path).expect("spawn catalog actor");
    let mut handle = actor.handle();
    let report = scan_folder(&mut handle, &root).expect("scan through actor");
    assert_eq!(report.inserted, 1);
    assert_eq!(handle.stats().expect("stats").photos, 1);
    actor.shutdown().expect("shutdown actor");

    fs::remove_dir_all(&root).expect("remove fixture directory");
    fs::remove_file(&database_path).expect("remove test catalog");
    for extension in ["sqlite-wal", "sqlite-shm"] {
        let sidecar = database_path.with_extension(extension);
        if sidecar.exists() {
            fs::remove_file(sidecar).expect("remove catalog sidecar");
        }
    }
}
