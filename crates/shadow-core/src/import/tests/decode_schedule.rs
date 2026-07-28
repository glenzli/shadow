use std::{
    fs,
    path::Path,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
};

use shadow_catalog::CatalogActor;
use shadow_domain::{
    DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot, EntityId,
    ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, PhotoId,
    RawDevelopmentCapabilitySnapshot, RawMetadataSnapshot, RepresentationKind,
};

use crate::{
    DecodeInspectionActor, DecodeInspector,
    import::{scan_folder_with_inspection, scan_folder_with_inspection_profiled},
};

#[test]
fn profiled_scan_and_actor_keep_scanner_and_worker_phases_separate() {
    let test_id = PhotoId::new_v7();
    let root = std::env::temp_dir().join(format!("shadow-profiled-actor-{test_id}"));
    let database_path =
        std::env::temp_dir().join(format!("shadow-profiled-actor-{test_id}.sqlite"));
    fs::create_dir_all(&root).expect("create fixture directory");
    fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
    fs::write(root.join("two.jpg"), b"jpeg").expect("write raster fixture");

    let actor = CatalogActor::spawn(&database_path).expect("spawn catalog actor");
    let mut catalog = actor.handle();
    let worker = DecodeInspectionActor::spawn_profiled(catalog.clone(), |_path: &Path| {
        Ok(scheduled_snapshot())
    })
    .expect("spawn profiled decode worker");
    let scan = scan_folder_with_inspection_profiled(&mut catalog, &worker.handle(), &root)
        .expect("profile scan and scheduling");
    let terminal = worker
        .shutdown_with_performance()
        .expect("drain profiled worker");

    assert_eq!(scan.report.decode_inspections_queued, 1);
    assert_eq!(scan.performance.decode_current_query.samples, 1);
    assert_eq!(scan.performance.decode_submit_wait.samples, 1);
    assert_eq!(terminal.summary.completed, 1);
    assert!(terminal.decode.profiled);
    assert_eq!(terminal.decode.queue_wait.samples, 1);
    assert_eq!(terminal.decode.provider_inspect.samples, 1);
    assert_eq!(terminal.decode.snapshot_catalog_commit.samples, 1);
    assert_eq!(terminal.decode.embedded_preview_extract.samples, 0);
    assert_eq!(terminal.decode.proxy_render.samples, 0);
    assert!(!terminal.technical.profiled);

    actor.shutdown().expect("shutdown catalog");
    fs::remove_dir_all(&root).expect("remove fixture directory");
    fs::remove_file(&database_path).expect("remove test catalog");
    for extension in ["sqlite-wal", "sqlite-shm"] {
        let sidecar = database_path.with_extension(extension);
        if sidecar.exists() {
            fs::remove_file(sidecar).expect("remove catalog sidecar");
        }
    }
}

#[test]
fn scan_reconciles_only_missing_provider_snapshots() {
    let test_id = PhotoId::new_v7();
    let root = std::env::temp_dir().join(format!("shadow-scheduled-scan-{test_id}"));
    let database_path = std::env::temp_dir().join(format!("shadow-scheduled-{test_id}.sqlite"));
    fs::create_dir_all(&root).expect("create fixture directory");
    fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
    fs::write(root.join("two.jpg"), b"jpeg").expect("write raster fixture");

    let actor = CatalogActor::spawn(&database_path).expect("spawn catalog actor");
    let mut catalog = actor.handle();
    let calls = Arc::new(AtomicUsize::new(0));
    let worker_calls = Arc::clone(&calls);
    let worker = DecodeInspectionActor::spawn(catalog.clone(), move |_path: &Path| {
        worker_calls.fetch_add(1, Ordering::SeqCst);
        Ok(scheduled_snapshot())
    })
    .expect("spawn decode worker");
    assert!(
        worker
            .handle()
            .supports_source(RepresentationKind::OriginalRaw, Path::new("one.NEF"))
    );
    assert!(
        !worker
            .handle()
            .supports_source(RepresentationKind::OriginalRaster, Path::new("two.jpg"))
    );
    let first = scan_folder_with_inspection(&mut catalog, &worker.handle(), &root)
        .expect("scan and schedule");
    assert_eq!(first.decode_inspections_queued, 1);
    worker.shutdown().expect("drain first decode worker");

    let second_worker_calls = Arc::clone(&calls);
    let second_worker = DecodeInspectionActor::spawn(catalog.clone(), move |_path: &Path| {
        second_worker_calls.fetch_add(1, Ordering::SeqCst);
        Ok(scheduled_snapshot())
    })
    .expect("spawn second decode worker");
    let second = scan_folder_with_inspection(&mut catalog, &second_worker.handle(), &root)
        .expect("rescan and reconcile");
    assert_eq!(second.decode_inspections_queued, 0);
    second_worker.shutdown().expect("shutdown second worker");
    assert_eq!(calls.load(Ordering::SeqCst), 1);

    actor.shutdown().expect("shutdown catalog");
    fs::remove_dir_all(&root).expect("remove fixture directory");
    fs::remove_file(&database_path).expect("remove test catalog");
}

#[derive(Debug)]
struct RasterOnlyInspector {
    calls: Arc<AtomicUsize>,
}

impl DecodeInspector for RasterOnlyInspector {
    fn provider_id(&self) -> &'static str {
        "raster-fixture"
    }

    fn supports_original_raw(&self) -> bool {
        false
    }

    fn supported_original_raster_extensions(&self) -> Vec<String> {
        vec!["jpg".to_owned(), "jpeg".to_owned()]
    }

    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String> {
        assert_eq!(
            path.extension().and_then(std::ffi::OsStr::to_str),
            Some("jpg"),
            "the scheduler must not submit RAW to a raster-only inspector"
        );
        self.calls.fetch_add(1, Ordering::SeqCst);
        let mut snapshot = scheduled_snapshot();
        snapshot.provider.id = self.provider_id().to_owned();
        Ok(snapshot)
    }
}

#[test]
fn jpeg_capable_inspector_schedules_jpeg_without_sending_raw_or_other_rasters() {
    let test_id = PhotoId::new_v7();
    let root = std::env::temp_dir().join(format!("shadow-raster-scan-{test_id}"));
    let database_path = std::env::temp_dir().join(format!("shadow-raster-{test_id}.sqlite"));
    fs::create_dir_all(&root).expect("create fixture directory");
    fs::write(root.join("one.NEF"), b"raw").expect("write raw fixture");
    fs::write(root.join("two.jpg"), b"jpeg").expect("write raster fixture");
    fs::write(root.join("three.png"), b"png").expect("write unsupported raster fixture");
    fs::write(root.join("four.tiff"), b"tiff").expect("write unsupported raster fixture");
    fs::write(root.join("five.heic"), b"heif").expect("write unavailable HEIF fixture");

    let actor = CatalogActor::spawn(&database_path).expect("spawn catalog actor");
    let mut catalog = actor.handle();
    let calls = Arc::new(AtomicUsize::new(0));
    let worker = DecodeInspectionActor::spawn(
        catalog.clone(),
        RasterOnlyInspector {
            calls: Arc::clone(&calls),
        },
    )
    .expect("spawn raster inspector");
    assert!(
        !worker
            .handle()
            .supports_source(RepresentationKind::OriginalRaw, Path::new("one.NEF"))
    );
    assert!(
        worker
            .handle()
            .supports_source(RepresentationKind::OriginalRaster, Path::new("two.jpg"))
    );
    assert!(
        !worker
            .handle()
            .supports_source(RepresentationKind::OriginalRaster, Path::new("three.png"))
    );
    assert!(
        !worker
            .handle()
            .supports_source(RepresentationKind::OriginalRaster, Path::new("four.tiff"))
    );
    assert!(
        !worker
            .handle()
            .supports_source(RepresentationKind::OriginalRaster, Path::new("five.heic"))
    );

    let report = scan_folder_with_inspection(&mut catalog, &worker.handle(), &root)
        .expect("scan with raster inspection");
    assert_eq!(report.supported_files, 5);
    assert_eq!(report.decode_inspections_queued, 1);
    let summary = worker
        .shutdown_with_summary()
        .expect("drain raster inspector");
    assert_eq!(summary.completed, 1);
    assert_eq!(calls.load(Ordering::SeqCst), 1);
    let page = catalog
        .review_page(None, 16)
        .expect("read inspected sources");
    let raw_representation_id = page
        .items
        .iter()
        .find(|item| item.location.display_path.ends_with("one.NEF"))
        .expect("registered RAW source")
        .representation_id;
    let raster_representation_id = page
        .items
        .iter()
        .find(|item| item.location.display_path.ends_with("two.jpg"))
        .expect("registered raster source")
        .representation_id;
    assert!(
        catalog
            .decode_snapshots(raw_representation_id)
            .expect("read RAW snapshots")
            .is_empty(),
        "a raster-only inspector must not record a RAW snapshot"
    );
    assert_eq!(
        catalog
            .decode_snapshots(raster_representation_id)
            .expect("read raster snapshots")
            .len(),
        1,
        "an opted-in raster inspector must complete the ordinary snapshot path"
    );

    actor.shutdown().expect("shutdown catalog");
    fs::remove_dir_all(&root).expect("remove fixture directory");
    fs::remove_file(&database_path).expect("remove test catalog");
    for extension in ["sqlite-wal", "sqlite-shm"] {
        let sidecar = database_path.with_extension(extension);
        if sidecar.exists() {
            fs::remove_file(sidecar).expect("remove catalog sidecar");
        }
    }
}

fn scheduled_snapshot() -> DecoderSnapshot {
    DecoderSnapshot {
        provider: DecodeProviderSnapshot {
            id: "anonymous".into(),
            version: "1".into(),
            dng_sdk: false,
            rawspeed: false,
            jpeg: false,
        },
        metadata: RawMetadataSnapshot {
            make: "Test".into(),
            model: "Fixture".into(),
            normalized_make: "Test".into(),
            normalized_model: "Fixture".into(),
            dng_version: None,
            raw_count: 1,
            raw_dimensions: ImageDimensions {
                width: 10,
                height: 10,
            },
            image_dimensions: ImageDimensions {
                width: 10,
                height: 10,
            },
            margins: ImageMargins::default(),
            orientation: 0,
            cfa_pattern: "RGGB".into(),
            sensor_colors: 3,
            sensor_bits: 12,
            black_level: 0,
            white_level: 4_095,
            as_shot_neutral: [1.0; 4],
            baseline_exposure: 0.0,
            iso_speed: 0.0,
            exposure_time_seconds: 0.0,
            aperture_f_number: 0.0,
            focal_length_mm: 0.0,
            captured_at_unix_seconds: 0,
            lens_make: String::new(),
            lens_model: String::new(),
            focal_length_35mm: 0.0,
        },
        capabilities: DecodeCapabilitySnapshot {
            metadata: DecodeSupport::Available,
            embedded_previews: DecodeSupport::Unavailable,
            raw_frame: DecodeSupport::Available,
            reference_rgb: DecodeSupport::Unavailable,
            pending_corrections: PendingCorrectionsSnapshot::default(),
            raw_development: RawDevelopmentCapabilitySnapshot::default(),
        },
        previews: Vec::new(),
    }
}
