use shadow_domain::EntityId;

use crate::{CatalogError, row_codec::read_id};

use super::{
    super::{
        DecodeSnapshotRecord, RecordDecodeSnapshot, RecordDecodeSnapshotStatus,
        RepresentationFingerprint,
    },
    fixture::{registered_catalog, snapshot},
};

#[test]
fn snapshot_round_trips_with_source_identity() {
    let (mut catalog, representation_id, source) = registered_catalog();
    let snapshot = snapshot("libraw", "0.22.2", &[1, 2]);
    let status = catalog
        .record_decode_snapshot(&RecordDecodeSnapshot {
            representation_id,
            expected_source: source,
            snapshot: snapshot.clone(),
            inspected_at_ms: 456,
        })
        .expect("record snapshot");

    assert_eq!(status, RecordDecodeSnapshotStatus::Recorded);
    assert_eq!(
        catalog.decode_snapshots(representation_id).expect("read"),
        vec![DecodeSnapshotRecord {
            representation_id,
            source,
            inspected_at_ms: 456,
            snapshot,
        }]
    );

    let photo_id = catalog
        .connection
        .query_row(
            "SELECT photo_id FROM representations WHERE id = ?1",
            [representation_id.as_bytes().as_slice()],
            |row| read_id(row, 0),
        )
        .expect("read owner photo");
    let facts = catalog
        .photo_library_facts(photo_id)
        .expect("read projected Library facts")
        .expect("metadata projection");
    assert_eq!(facts.capture_day, "2023-11-14");
    assert_eq!(facts.camera_make, "Pentax");
    assert_eq!(facts.camera_model, "K10D");
    assert_eq!(facts.lens_model, "smc PENTAX-DA 35mm");
    assert_eq!(facts.aperture_milli, Some(5_600));
    assert_eq!(facts.focal_length_tenth_mm, Some(350));
    assert_eq!(facts.iso_speed, Some(100.0));
    assert_eq!(facts.latitude_e7, Some(312_300_000));
    assert_eq!(facts.longitude_e7, Some(1_214_735_000));
    assert_eq!(facts.indexed_representation_id, Some(representation_id));
    assert_eq!(facts.indexed_source, Some(source));
    assert_eq!(facts.indexed_at_ms, 456);
}

#[test]
fn same_provider_replaces_snapshot_and_preview_rows() {
    let (mut catalog, representation_id, source) = registered_catalog();
    for (version, preview_ids) in [("0.22.1", &[1, 2][..]), ("0.22.2", &[7][..])] {
        catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id,
                expected_source: source,
                snapshot: snapshot("libraw", version, preview_ids),
                inspected_at_ms: 456,
            })
            .expect("record snapshot");
    }

    let records = catalog.decode_snapshots(representation_id).expect("read");
    assert_eq!(records.len(), 1);
    assert_eq!(records[0].snapshot.provider.version, "0.22.2");
    assert_eq!(records[0].snapshot.previews[0].provider_id, 7);
    let preview_rows: i64 = catalog
        .connection
        .query_row("SELECT COUNT(*) FROM representation_previews", [], |row| {
            row.get(0)
        })
        .expect("count previews");
    assert_eq!(preview_rows, 1);
}

#[test]
fn distinct_providers_coexist() {
    let (mut catalog, representation_id, source) = registered_catalog();
    for provider_id in ["libraw", "nikon-private"] {
        catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id,
                expected_source: source,
                snapshot: snapshot(provider_id, "1", &[]),
                inspected_at_ms: 456,
            })
            .expect("record snapshot");
    }

    let records = catalog.decode_snapshots(representation_id).expect("read");
    assert_eq!(records.len(), 2);
    assert_eq!(records[0].snapshot.provider.id, "libraw");
    assert_eq!(records[1].snapshot.provider.id, "nikon-private");
}

#[test]
fn stale_source_cannot_overwrite_current_snapshot() {
    let (mut catalog, representation_id, source) = registered_catalog();
    let first = snapshot("libraw", "current", &[1]);
    catalog
        .record_decode_snapshot(&RecordDecodeSnapshot {
            representation_id,
            expected_source: source,
            snapshot: first.clone(),
            inspected_at_ms: 456,
        })
        .expect("record current snapshot");

    let status = catalog
        .record_decode_snapshot(&RecordDecodeSnapshot {
            representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: source.byte_len + 1,
                modified_at_ms: source.modified_at_ms,
            },
            snapshot: snapshot("libraw", "stale", &[2]),
            inspected_at_ms: 789,
        })
        .expect("reject stale snapshot");

    assert_eq!(status, RecordDecodeSnapshotStatus::StaleSource);
    let records = catalog.decode_snapshots(representation_id).expect("read");
    assert_eq!(records[0].snapshot, first);
}

#[test]
fn non_finite_metadata_is_rejected_before_json_persistence() {
    let (mut catalog, representation_id, source) = registered_catalog();
    let mut invalid = snapshot("libraw", "invalid", &[]);
    invalid.metadata.baseline_exposure = f64::NAN;

    let error = catalog
        .record_decode_snapshot(&RecordDecodeSnapshot {
            representation_id,
            expected_source: source,
            snapshot: invalid,
            inspected_at_ms: 456,
        })
        .expect_err("reject non-finite snapshot");
    assert!(matches!(error, CatalogError::InvalidDecodeSnapshot(_)));
    assert!(
        catalog
            .decode_snapshots(representation_id)
            .expect("read snapshots")
            .is_empty()
    );
}
