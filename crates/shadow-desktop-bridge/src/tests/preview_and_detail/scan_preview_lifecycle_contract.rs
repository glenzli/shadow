//! Folder scanning, decode inspection, and preview-drain terminal contracts.

use shadow_core::{DecodeInspectionSummary, ScanCompletion};
use shadow_domain::{EntityId, ImportSessionId, RepresentationId};

use crate::scan_service::validate_decode_inspection_summary;
use crate::{ffi, open_desktop_session};

#[test]
fn folder_scan_registry_is_fail_closed_cancel_safe_and_terminal() {
    let root = std::env::temp_dir().join(format!(
        "shadow-desktop-scan-registry-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    let import_root = root.join("photos");
    std::fs::create_dir_all(&import_root).expect("create scan fixture");
    std::fs::write(import_root.join("broken.jpg"), b"not a valid JPEG")
        .expect("write malformed JPEG fixture");
    std::fs::write(import_root.join("broken.NEF"), b"not a raw file")
        .expect("write broken RAW fixture");
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open desktop session");

    assert!(!session.scan_progress(41).expect("missing progress").valid);
    session.begin_folder_scan(41).expect("prepare first scan");
    assert!(session.begin_folder_scan(42).is_err());
    assert!(session.cancel_folder_scan(42).is_err());
    assert!(
        session
            .cancel_folder_scan(41)
            .expect("cancel prepared scan")
    );
    assert!(!session.cancel_folder_scan(41).expect("repeat cancellation"));
    assert_eq!(
        session
            .scan_progress(41)
            .expect("cancelling progress")
            .phase,
        ffi::FfiScanPhase::Cancelling
    );

    let cancelled = session
        .scan_folder(import_root.to_str().expect("import path"), 41)
        .expect("collect cancelled scan");
    assert!(cancelled.cancelled);
    assert_eq!(cancelled.files_seen, 0);
    assert_eq!(
        session.scan_progress(41).expect("cancelled progress").phase,
        ffi::FfiScanPhase::Cancelled
    );
    assert!(
        session
            .scan_folder(import_root.to_str().expect("import path"), 41)
            .is_err()
    );

    session
        .begin_folder_scan(42)
        .expect("prepare replacement scan");
    let completed = session
        .scan_folder(import_root.to_str().expect("import path"), 42)
        .expect("complete replacement scan");
    assert!(!completed.cancelled);
    assert_eq!(completed.supported_files, 2);
    assert_eq!(completed.inserted, 2);
    assert_eq!(completed.decode_inspections_queued, 2);
    assert_eq!(completed.decode_inspections_completed, 2);
    assert_eq!(completed.decode_hard_failures, 2);
    assert_eq!(completed.preview_failures, 0);
    assert_eq!(completed.decode_inspections_cancelled, 0);
    let progress = session.scan_progress(42).expect("completed progress");
    assert!(progress.valid);
    assert_eq!(progress.scan_id, 42);
    assert_eq!(progress.phase, ffi::FfiScanPhase::Completed);
    assert_eq!(progress.supported_files, completed.supported_files);
    assert_eq!(progress.inserted, completed.inserted);
    assert_eq!(
        progress.decode_hard_failures,
        completed.decode_hard_failures
    );
    assert!(!session.scan_progress(41).expect("stale progress").valid);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove scan fixture");
}

#[test]
fn decode_inspection_summary_validation_is_fail_closed() {
    let valid = DecodeInspectionSummary {
        completed: 4,
        hard_failures: 1,
        preview_failures: 1,
        cancelled: 1,
    };
    validate_decode_inspection_summary(4, &valid).expect("valid terminal summary");

    let partial_cancelled = DecodeInspectionSummary {
        completed: 2,
        cancelled: 2,
        ..DecodeInspectionSummary::default()
    };
    assert!(validate_decode_inspection_summary(4, &partial_cancelled).is_err());

    let impossible_total = DecodeInspectionSummary {
        completed: 5,
        ..DecodeInspectionSummary::default()
    };
    assert!(validate_decode_inspection_summary(4, &impossible_total).is_err());

    let overlapping_diagnostics = DecodeInspectionSummary {
        completed: 2,
        hard_failures: 1,
        preview_failures: 1,
        cancelled: 1,
    };
    assert!(validate_decode_inspection_summary(2, &overlapping_diagnostics).is_err());
}

#[test]
fn preparing_preview_cancel_and_finish_are_terminally_linearized() {
    let root = std::env::temp_dir().join(format!(
        "shadow-desktop-scan-terminal-race-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open desktop session");
    let completed_report = shadow_core::ScanReport {
        session_id: ImportSessionId::new_v7(),
        completion: ScanCompletion::Completed,
        files_seen: 4,
        supported_files: 3,
        inserted: 3,
        unchanged: 0,
        needs_revalidation: 0,
        decode_inspections_queued: 0,
        skipped: 1,
        issues: Vec::new(),
    };
    let summary = DecodeInspectionSummary::default();

    session.begin_folder_scan(501).expect("prepare late cancel");
    drop(
        session
            .folder_scan_cancellation(501)
            .expect("mark late cancel scan started"),
    );
    session
        .update_folder_scan_report(501, &completed_report, ffi::FfiScanPhase::PreparingPreviews)
        .expect("enter preview drain");
    assert!(session.cancel_folder_scan(501).expect("win cancel race"));
    assert!(
        session
            .finish_folder_scan(501, &completed_report, &summary)
            .expect("finish cancelled job")
    );
    assert_eq!(
        session.scan_progress(501).expect("cancel terminal").phase,
        ffi::FfiScanPhase::Cancelled
    );

    session
        .begin_folder_scan(502)
        .expect("prepare finish winner");
    drop(
        session
            .folder_scan_cancellation(502)
            .expect("mark finish-winner scan started"),
    );
    session
        .update_folder_scan_report(502, &completed_report, ffi::FfiScanPhase::PreparingPreviews)
        .expect("enter second preview drain");
    assert!(
        !session
            .finish_folder_scan(502, &completed_report, &summary)
            .expect("win finish race")
    );
    assert!(
        !session
            .cancel_folder_scan(502)
            .expect("late cancel rejected")
    );
    assert_eq!(
        session.scan_progress(502).expect("complete terminal").phase,
        ffi::FfiScanPhase::Completed
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove scan race fixture");
}
