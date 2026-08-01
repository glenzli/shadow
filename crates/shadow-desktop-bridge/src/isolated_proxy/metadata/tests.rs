use std::fs;

use super::{parse_metadata_snapshot_protocol, read_metadata_snapshot, write_metadata_snapshot};
use crate::isolated_proxy::{
    descriptor_protocol_fixture::metadata_snapshot_protocol, route_fixture::safety_fixture,
    route_identity::DecoderSafetyKey,
};

#[test]
fn parses_a_nonce_bound_child_metadata_snapshot() {
    let snapshot = parse_metadata_snapshot_protocol(
        &metadata_snapshot_protocol("metadata-nonce"),
        "metadata-nonce",
    )
    .expect("parse metadata snapshot");
    assert_eq!(snapshot.router_provider_id, "shadow-router");
    assert_eq!(snapshot.router_provider_version, "v1");
    assert_eq!(snapshot.metadata.make, "Nikon");
    assert_eq!(snapshot.metadata.model, "Z 9");
    assert_eq!(snapshot.metadata.dng_version, None);
    assert_eq!(snapshot.metadata.raw_dimensions.width, 8256);
    assert_eq!(snapshot.metadata.raw_dimensions.height, 5504);
    assert_eq!(snapshot.metadata.orientation, 1);
    assert_eq!(snapshot.metadata.cfa_pattern, "RGGB");
    assert_eq!(snapshot.metadata.lens_model, "NIKKOR Z 24-120mm");
    assert_eq!(snapshot.metadata.iso_speed.to_bits(), 100.0_f64.to_bits());
    assert_eq!(
        snapshot.metadata.focal_length_35mm.to_bits(),
        36.0_f64.to_bits()
    );
    let focus = snapshot
        .metadata
        .focus_observation
        .as_ref()
        .expect("camera focus observation");
    assert_eq!(focus.center_x.to_bits(), 0.625_f64.to_bits());
    assert_eq!(focus.center_y.to_bits(), 0.375_f64.to_bits());
    assert_eq!(focus.width.to_bits(), 0.125_f64.to_bits());
    assert_eq!(focus.height.to_bits(), 0.25_f64.to_bits());
    assert!(focus.focus_confirmed);
    let gps = snapshot.metadata.gps.expect("GPS metadata");
    assert_eq!(gps.latitude_degrees.to_bits(), 31.23_f64.to_bits());
    assert_eq!(gps.longitude_degrees.to_bits(), 121.4735_f64.to_bits());
    assert_eq!(gps.altitude_meters, Some(50.0));
}

#[test]
fn rejects_stale_nonce_and_nonfinite_child_metadata() {
    let response = metadata_snapshot_protocol("metadata-nonce");
    assert!(parse_metadata_snapshot_protocol(&response, "other-nonce").is_err());

    let invalid = String::from_utf8(response)
        .expect("metadata test protocol is UTF-8")
        .replacen("3ff0000000000000", "7ff8000000000000", 1);
    assert!(parse_metadata_snapshot_protocol(invalid.as_bytes(), "metadata-nonce").is_err());
}

#[test]
fn child_metadata_snapshots_are_keyed_and_persisted() {
    let (root, source, helper) = safety_fixture("metadata-snapshot");
    let key = DecoderSafetyKey::metadata_snapshot(&source, &helper)
        .expect("metadata snapshot safety key");
    let snapshot = parse_metadata_snapshot_protocol(
        &metadata_snapshot_protocol("metadata-nonce"),
        "metadata-nonce",
    )
    .expect("parse metadata snapshot fixture");
    write_metadata_snapshot(&root, &key, &snapshot).expect("persist metadata snapshot");
    assert_eq!(
        read_metadata_snapshot(&root, &key).expect("read metadata snapshot"),
        Some(snapshot)
    );

    fs::write(&source, b"changed source identity").expect("change metadata source fixture");
    let changed_key = DecoderSafetyKey::metadata_snapshot(&source, &helper)
        .expect("changed metadata snapshot safety key");
    assert_eq!(
        read_metadata_snapshot(&root, &changed_key).expect("read changed metadata snapshot"),
        None,
        "metadata from one source revision must not attach to another"
    );

    fs::remove_dir_all(root).expect("remove metadata snapshot fixture");
}
