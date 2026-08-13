use std::fs;

use shadow_domain::{
    DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot,
    GpsMetadataSnapshot, ImageDimensions, ImageMargins, PendingCorrectionsSnapshot,
    RawDevelopmentCapabilitySnapshot, RawMetadataSnapshot,
};
use uuid::Uuid;

use super::{LocationReferenceService, collect_anchors};

fn snapshot(captured_at_unix_seconds: i64, gps: Option<(f64, f64)>) -> DecoderSnapshot {
    DecoderSnapshot {
        provider: DecodeProviderSnapshot {
            id: "fixture".into(),
            version: "1".into(),
            dng_sdk: false,
            rawspeed: false,
            jpeg: true,
        },
        metadata: RawMetadataSnapshot {
            make: "Fixture".into(),
            model: "Location reference".into(),
            normalized_make: "Fixture".into(),
            normalized_model: "Location reference".into(),
            dng_version: None,
            raw_count: 0,
            raw_dimensions: ImageDimensions::default(),
            image_dimensions: ImageDimensions::default(),
            margins: ImageMargins::default(),
            orientation: 0,
            cfa_pattern: String::new(),
            sensor_colors: 0,
            sensor_bits: 0,
            black_level: 0,
            white_level: 0,
            as_shot_neutral: [1.0; 4],
            baseline_exposure: 0.0,
            iso_speed: 0.0,
            exposure_time_seconds: 0.0,
            aperture_f_number: 0.0,
            focal_length_mm: 0.0,
            focus_observation: None,
            captured_at_unix_seconds,
            gps: gps.map(
                |(latitude_degrees, longitude_degrees)| GpsMetadataSnapshot {
                    latitude_degrees,
                    longitude_degrees,
                    altitude_meters: None,
                },
            ),
            lens_make: String::new(),
            lens_model: String::new(),
            focal_length_35mm: 0.0,
        },
        capabilities: DecodeCapabilitySnapshot {
            metadata: DecodeSupport::Available,
            embedded_previews: DecodeSupport::Unavailable,
            raw_frame: DecodeSupport::Unavailable,
            reference_rgb: DecodeSupport::Unavailable,
            pending_corrections: PendingCorrectionsSnapshot::default(),
            raw_development: RawDevelopmentCapabilitySnapshot::default(),
        },
        previews: Vec::new(),
    }
}

#[test]
fn reference_scan_keeps_only_time_and_gps_and_applies_clock_offset() {
    let temporary = test_root();
    let source = temporary.join("phone");
    fs::create_dir_all(source.join("nested")).expect("create source root");
    fs::write(source.join("located.jpg"), b"located").expect("write located fixture");
    fs::write(source.join("nested/no-gps.jpg"), b"no gps").expect("write no-gps fixture");

    let anchors = collect_anchors("phone-2026", &source, 90, |path| {
        if path.ends_with("located.jpg") {
            Ok(snapshot(1_700_000_000, Some((31.2304, 121.4737))))
        } else {
            Ok(snapshot(1_700_000_100, None))
        }
    })
    .expect("scan anchors");
    assert_eq!(anchors.len(), 1);
    assert_eq!(anchors[0].relative_path, "located.jpg");
    assert_eq!(anchors[0].captured_at_unix_seconds, 1_700_000_090);
    assert_eq!(anchors[0].latitude_e7, 312_304_000);
    assert_eq!(anchors[0].longitude_e7, 1_214_737_000);
    fs::remove_dir_all(temporary).expect("remove fixture root");
}

#[test]
fn service_rebuild_replaces_one_library_without_touching_another() {
    let temporary = test_root();
    let root_one = temporary.join("one");
    let root_two = temporary.join("two");
    fs::create_dir_all(&root_one).expect("create first root");
    fs::create_dir_all(&root_two).expect("create second root");
    fs::write(root_one.join("one.jpg"), b"one").expect("write first fixture");
    fs::write(root_two.join("two.jpg"), b"two").expect("write second fixture");
    let mut service =
        LocationReferenceService::open(temporary.join("state")).expect("open service");
    service
        .add_or_rescan_with("one", &root_one, 0, 1, |_| {
            Ok(snapshot(1_700_000_000, Some((1.0, 2.0))))
        })
        .expect("index first library");
    service
        .add_or_rescan_with("two", &root_two, 0, 2, |_| {
            Ok(snapshot(1_700_000_100, Some((3.0, 4.0))))
        })
        .expect("index second library");
    service
        .add_or_rescan_with("one", &root_one, 30, 3, |_| {
            Ok(snapshot(1_700_000_200, Some((5.0, 6.0))))
        })
        .expect("rebuild first library");
    let snapshot = service.snapshot();
    assert_eq!(snapshot.libraries.len(), 2);
    assert_eq!(snapshot.anchors.len(), 2);
    assert_eq!(
        snapshot
            .anchors
            .iter()
            .filter(|anchor| anchor.library_id == "one")
            .count(),
        1
    );
    assert!(
        snapshot
            .anchors
            .iter()
            .any(|anchor| anchor.library_id == "two")
    );
    assert!(snapshot.anchors.iter().any(
        |anchor| anchor.library_id == "one" && anchor.captured_at_unix_seconds == 1_700_000_230
    ));
    assert!(service.remove("one").expect("remove reference library"));
    assert_eq!(service.snapshot().anchors.len(), 1);
    fs::remove_dir_all(temporary).expect("remove fixture root");
}

fn test_root() -> std::path::PathBuf {
    std::env::temp_dir().join(format!("shadow-location-reference-{}", Uuid::now_v7()))
}
