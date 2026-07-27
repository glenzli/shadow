//! Preview cache, cancellation, detail viewport, and decode-admission contracts.

use super::*;
use crate::scan_service::validate_decode_inspection_summary;

#[test]
fn desktop_session_can_back_concurrent_qt_image_requests() {
    fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<DesktopSession>();
}

#[test]
fn generated_photo_proxy_cache_identity_matches_its_encoder_request() {
    let inspector = PhotoInspector::new().expect("construct photo inspector");

    assert_eq!(inspector.provider_id(), "shadow-photo-router");
    assert_eq!(inspector.provider_version(), photo_provider_version());
    assert_eq!(PHOTO_GRID_PROXY_MAX_EDGE, 2_048);
    assert_eq!(PHOTO_GRID_PROXY_JPEG_QUALITY, 90);
    assert_eq!(
        inspector.proxy_variant_key(),
        format!(
            "shadow-photo-router:grid-jpeg-2048-q90-444-v1;\
             source=provider-neutral-raw-plan;{}",
            raw_development_plan_identity(RawDevelopmentPlan::preview())
                .expect("canonical preview plan identity")
        )
    );
    let extensions = inspector.supported_original_raster_extensions();
    assert!(extensions.contains(&"jpg".to_owned()));
    assert!(extensions.contains(&"jpeg".to_owned()));
}

#[test]
fn edit_session_cache_keeps_adjusted_raw_plan_requests_distinct() {
    let requested_before_adjustment = "shadow-raw-plan-v1;intent=detail;quality=high";
    let effective_after_adjustment = "shadow-raw-plan-v1;intent=detail;quality=balanced";

    assert!(requested_raw_development_plan_cache_matches(
        requested_before_adjustment,
        requested_before_adjustment,
    ));
    // A future provider can negotiate the high-quality request down to balanced. A later
    // balanced request must still negotiate and receive its own receipt, rather than being
    // served the session whose receipt records the earlier high-quality request.
    assert!(!requested_raw_development_plan_cache_matches(
        requested_before_adjustment,
        effective_after_adjustment,
    ));
}

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

#[test]
fn detail_viewport_tiles_cover_center_and_clipped_edges_without_duplicates() {
    let dimensions = ImageDimensions {
        width: 1_300,
        height: 900,
    };
    let center =
        detail_viewport_rects(dimensions, 0.5, 0.5, 700, 600, 512).expect("tile centered viewport");
    assert_eq!(center.len(), 4);
    let unique = center
        .iter()
        .map(|rect| (rect.x, rect.y, rect.width, rect.height))
        .collect::<BTreeSet<_>>();
    assert_eq!(unique.len(), center.len());
    assert!(center.iter().all(|rect| {
        rect.x + rect.width <= dimensions.width && rect.y + rect.height <= dimensions.height
    }));

    let bottom_right =
        detail_viewport_rects(dimensions, 1.0, 1.0, 512, 512, 512).expect("tile edge viewport");
    assert!(bottom_right.iter().any(|rect| {
        rect.x == 1_024 && rect.y == 512 && rect.width == 276 && rect.height == 388
    }));
    assert!(bottom_right.iter().all(|rect| {
        rect.x + rect.width <= dimensions.width && rect.y + rect.height <= dimensions.height
    }));
}

#[test]
fn detail_viewport_geometry_fails_closed() {
    let dimensions = ImageDimensions {
        width: 1_300,
        height: 900,
    };
    for (center_x, tile_side) in [(f64::NAN, 512), (0.5, 0), (0.5, 1_025)] {
        assert!(detail_viewport_rects(dimensions, center_x, 0.5, 700, 600, tile_side).is_err());
    }
    assert!(
        detail_viewport_rects(
            ImageDimensions {
                width: 0,
                height: 900,
            },
            0.5,
            0.5,
            700,
            600,
            512,
        )
        .is_err()
    );
}

#[test]
fn detail_request_rejects_an_excessive_grid_before_source_work() {
    let mut request = ffi::FfiEditDetailViewportRequest {
        base_commit_id: String::new(),
        settings: ffi_parameters(0.0, 1.0, [0.0; 2], 1.0),
        render_token: 1,
        center_x: 0.5,
        center_y: 0.5,
        viewport_width: 4_096,
        viewport_height: 4_096,
        tile_side: 512,
        use_working_recipe: true,
    };
    validate_detail_viewport_request(&request).expect("the desktop 512px grid is admitted");

    request.viewport_width = 6_016;
    request.viewport_height = 3_384;
    request.tile_side = 1_024;
    validate_detail_viewport_request(&request)
        .expect("an adaptive 1024px grid admits a 6K display viewport");

    request.viewport_width = 8_193;
    assert!(validate_detail_viewport_request(&request).is_err());

    request.viewport_width = 4_096;
    request.viewport_height = 4_096;
    request.tile_side = 1;
    let error = validate_detail_viewport_request(&request)
        .expect_err("a pathological grid must fail before source lookup or decode");
    assert!(error.to_string().contains("pre-decode admission"));
}

#[test]
fn quarantined_native_decode_is_rejected_before_source_opening() {
    let source = Path::new("/photos/unsafe.raw");
    let error = reject_quarantined_native_decode(
        NativeDecodeAdmission::Quarantined {
            observation: crate::isolated_proxy::IsolatedDecodeObservation::ChildCrashed,
        },
        source,
    )
    .expect_err("a child decoder crash must block the direct native opener");
    assert!(error.to_string().contains("preview-only"));
    assert!(error.to_string().contains("child decoder crashed"));
    assert!(
        reject_quarantined_native_decode(NativeDecodeAdmission::NotQuarantined, source).is_ok()
    );
}

#[test]
fn newer_detail_render_tokens_cancel_older_tile_work() {
    let (root, session, _, _) = test_edit_session();
    let first = session.begin_basic_edit_detail();
    session
        .ensure_current_edit_detail_render(first)
        .expect("fresh token is current");
    let second = session.begin_basic_edit_detail();
    assert!(session.ensure_current_edit_detail_render(first).is_err());
    session
        .ensure_current_edit_detail_render(second)
        .expect("new token supersedes the old token");

    drop(session);
    std::fs::remove_dir_all(root).expect("remove detail-token fixture");
}
