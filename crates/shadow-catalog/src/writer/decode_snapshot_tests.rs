use shadow_domain::{
    AssetLocation, DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport,
    DecoderSnapshot, ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, Platform,
    RawMetadataSnapshot, RepresentationKind,
};

use crate::{
    RecordDecodeSnapshot, RecordDecodeSnapshotStatus, RegisterAsset, RepresentationFingerprint,
};

use super::CatalogActor;

fn snapshot() -> DecoderSnapshot {
    DecoderSnapshot {
        provider: DecodeProviderSnapshot {
            id: "libraw".into(),
            version: "0.22.2".into(),
            dng_sdk: false,
            rawspeed: false,
            jpeg: true,
        },
        metadata: RawMetadataSnapshot {
            make: String::new(),
            model: String::new(),
            normalized_make: String::new(),
            normalized_model: String::new(),
            dng_version: None,
            raw_count: 1,
            raw_dimensions: ImageDimensions {
                width: 100,
                height: 80,
            },
            image_dimensions: ImageDimensions {
                width: 100,
                height: 80,
            },
            margins: ImageMargins::default(),
            orientation: 0,
            cfa_pattern: "RGGB".into(),
            sensor_colors: 3,
            sensor_bits: 12,
            black_level: 0,
            white_level: 4_095,
            as_shot_neutral: [1.0, 1.0, 1.0, 0.0],
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
            metadata: DecodeSupport::Unavailable,
            embedded_previews: DecodeSupport::Unavailable,
            raw_frame: DecodeSupport::Available,
            reference_rgb: DecodeSupport::Unavailable,
            pending_corrections: PendingCorrectionsSnapshot::default(),
            raw_development: Default::default(),
        },
        previews: Vec::new(),
    }
}

#[test]
fn actor_routes_decoder_snapshots_and_output_freshness() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let source = RepresentationFingerprint {
        byte_len: 1_024,
        modified_at_ms: Some(123),
    };
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/decode-snapshot.dng".to_vec(),
                "/photos/decode-snapshot.dng",
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 100,
        })
        .expect("register source through actor");
    let snapshot = snapshot();

    assert_eq!(
        handle
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id: registered.representation_id,
                expected_source: source,
                snapshot: snapshot.clone(),
                inspected_at_ms: 456,
            })
            .expect("record snapshot through actor"),
        RecordDecodeSnapshotStatus::Recorded
    );
    let records = handle
        .decode_snapshots(registered.representation_id)
        .expect("read snapshots through actor");
    assert_eq!(records.len(), 1);
    assert_eq!(records[0].snapshot, snapshot);
    assert!(
        handle
            .is_decode_output_current(
                registered.representation_id,
                "libraw",
                "0.22.2",
                source,
                false,
                "unused-without-proxy",
                None,
            )
            .expect("read current output through actor")
    );
    assert!(
        !handle
            .is_decode_output_current(
                registered.representation_id,
                "libraw",
                "0.22.3",
                source,
                false,
                "unused-without-proxy",
                None,
            )
            .expect("read stale provider output through actor")
    );
    actor.shutdown().expect("shutdown actor");
}
