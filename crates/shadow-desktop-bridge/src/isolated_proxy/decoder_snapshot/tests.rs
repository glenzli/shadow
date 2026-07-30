use std::fs;

#[cfg(unix)]
use std::{os::unix::fs::PermissionsExt, path::Path};

use shadow_domain::{DecodeSupport, PreviewCodec, RawDevelopmentCapabilitySnapshot};

use super::{
    DECODER_SNAPSHOT_BASE_FIELD_COUNT, parse_decoder_snapshot_protocol, read_decoder_snapshot,
    snapshot_isolated_photo_decoder, write_decoder_snapshot,
};
use crate::isolated_proxy::{
    descriptor_protocol_fixture::decoder_snapshot_protocol, route_fixture::safety_fixture,
    route_identity::DecoderSafetyKey,
};

#[test]
fn parses_and_normalizes_a_nonce_bound_child_decoder_snapshot() {
    let helper_snapshot = parse_decoder_snapshot_protocol(
        &decoder_snapshot_protocol("decoder-nonce"),
        "decoder-nonce",
    )
    .expect("parse decoder snapshot");
    assert_eq!(helper_snapshot.router_provider_id, "shadow-helper");
    assert_eq!(helper_snapshot.router_provider_version, "v3-private");
    assert_eq!(helper_snapshot.snapshot.provider.id, "shadow-helper");
    assert!(
        helper_snapshot
            .snapshot
            .capabilities
            .raw_frame
            .is_available()
    );
    assert!(
        helper_snapshot
            .snapshot
            .capabilities
            .raw_development
            .available
            .is_available()
    );
    assert_eq!(helper_snapshot.snapshot.previews.len(), 2);
    assert_eq!(
        helper_snapshot.snapshot.previews[0].codec,
        PreviewCodec::Jpeg
    );
    assert_eq!(helper_snapshot.snapshot.previews[0].dimensions.width, 4000);

    let catalog_snapshot = helper_snapshot
        .clone()
        .into_catalog_snapshot("shadow-photo-router", "host-public-v1");
    assert_eq!(catalog_snapshot.provider.id, "shadow-photo-router");
    assert_eq!(catalog_snapshot.provider.version, "host-public-v1");
    assert!(!catalog_snapshot.provider.dng_sdk);
    assert!(!catalog_snapshot.provider.rawspeed);
    assert!(!catalog_snapshot.provider.jpeg);
    assert_eq!(
        catalog_snapshot.capabilities.metadata,
        DecodeSupport::Available,
        "catalog projection can use copied metadata without reopening the RAW"
    );
    assert_eq!(
        catalog_snapshot.capabilities.embedded_previews,
        DecodeSupport::Unavailable,
        "the host cannot safely extract helper-side embedded preview bytes"
    );
    assert_eq!(
        catalog_snapshot.capabilities.raw_frame,
        DecodeSupport::Unavailable,
        "helper-only RawFrame access must not become a desktop capability"
    );
    assert_eq!(
        catalog_snapshot.capabilities.reference_rgb,
        DecodeSupport::Unavailable,
        "helper-only RGB rendering must not become a desktop capability"
    );
    assert_eq!(
        catalog_snapshot.capabilities.raw_development,
        RawDevelopmentCapabilitySnapshot::default(),
        "a catalog-only descriptor snapshot cannot negotiate host RAW development"
    );
    assert!(catalog_snapshot.previews.is_empty());
}

#[test]
fn rejects_stale_or_invalid_child_decoder_snapshots() {
    let response = decoder_snapshot_protocol("decoder-nonce");
    assert!(parse_decoder_snapshot_protocol(&response, "other-nonce").is_err());

    let mut control_byte_fields = String::from_utf8(response.clone())
        .expect("decoder fixture is UTF-8")
        .split_whitespace()
        .map(str::as_bytes)
        .map(ToOwned::to_owned)
        .collect::<Vec<_>>();
    // Field 38 is the GPS-coordinate presence flag. A stale private provider
    // once projected an old std::string layout into this bool and made the
    // helper emit NUL instead of the final hexadecimal digit.
    *control_byte_fields[38]
        .last_mut()
        .expect("GPS presence field is non-empty") = 0;
    let control_byte_response = control_byte_fields.join(&b' ');
    assert!(
        parse_decoder_snapshot_protocol(&control_byte_response, "decoder-nonce").is_err(),
        "control bytes from a malformed native ABI must fail closed"
    );

    let mut fields = String::from_utf8(response)
        .expect("decoder fixture is UTF-8")
        .split_whitespace()
        .map(ToOwned::to_owned)
        .collect::<Vec<_>>();
    fields[DECODER_SNAPSHOT_BASE_FIELD_COUNT + 1] = "0000000000000005".to_owned();
    assert!(parse_decoder_snapshot_protocol(fields.join(" ").as_bytes(), "decoder-nonce").is_err());

    let mut impossible_count = String::from_utf8(decoder_snapshot_protocol("decoder-nonce"))
        .expect("decoder fixture is UTF-8")
        .split_whitespace()
        .map(ToOwned::to_owned)
        .collect::<Vec<_>>();
    impossible_count[57] = "0000000000000041".to_owned();
    assert!(
        parse_decoder_snapshot_protocol(impossible_count.join(" ").as_bytes(), "decoder-nonce")
            .is_err()
    );
}

#[test]
fn child_decoder_snapshots_are_keyed_and_persisted() {
    let (root, source, helper) = safety_fixture("decoder-snapshot");
    let key =
        DecoderSafetyKey::decoder_snapshot(&source, &helper).expect("decoder snapshot safety key");
    let snapshot = parse_decoder_snapshot_protocol(
        &decoder_snapshot_protocol("decoder-nonce"),
        "decoder-nonce",
    )
    .expect("parse decoder snapshot fixture");
    write_decoder_snapshot(&root, &key, &snapshot).expect("persist decoder snapshot");
    assert_eq!(
        read_decoder_snapshot(&root, &key).expect("read decoder snapshot"),
        Some(snapshot)
    );

    fs::write(&helper, b"changed helper identity").expect("change decoder helper fixture");
    let changed_key = DecoderSafetyKey::decoder_snapshot(&source, &helper)
        .expect("changed decoder snapshot safety key");
    assert_eq!(
        read_decoder_snapshot(&root, &changed_key).expect("read changed decoder snapshot"),
        None,
        "a helper replacement must not reuse a prior child decoder snapshot"
    );

    fs::remove_dir_all(root).expect("remove decoder snapshot fixture");
}

#[cfg(unix)]
#[test]
fn child_decoder_snapshot_stage_executes_and_reuses_its_bounded_cache() {
    let (root, source, helper) = safety_fixture("decoder-snapshot-execution");
    write_decoder_snapshot_helper(&helper);
    let first = snapshot_isolated_photo_decoder(&helper, &root, &source)
        .expect("execute fake decoder snapshot helper");
    assert_eq!(first.router_provider_id, "shadow-helper");
    assert_eq!(first.snapshot.previews.len(), 2);

    // The second request must consume the immutable source/helper-keyed
    // record rather than launch another native helper process.
    let second = snapshot_isolated_photo_decoder(&helper, &root, &source)
        .expect("reuse cached decoder snapshot");
    assert_eq!(second, first);

    fs::write(&helper, b"this replacement is intentionally not executable")
        .expect("replace helper after cache write");
    let replacement_key =
        DecoderSafetyKey::decoder_snapshot(&source, &helper).expect("replacement helper key");
    assert!(
        read_decoder_snapshot(&root, &replacement_key)
            .expect("read replacement cache")
            .is_none(),
        "a changed helper cannot reuse an inspection snapshot"
    );

    fs::remove_dir_all(root).expect("remove decoder snapshot execution fixture");
}

#[cfg(unix)]
fn write_decoder_snapshot_helper(path: &Path) {
    let mut fields = String::from_utf8(decoder_snapshot_protocol("placeholder-nonce"))
        .expect("decoder fixture is UTF-8")
        .split_whitespace()
        .map(ToOwned::to_owned)
        .collect::<Vec<_>>();
    // The helper receives `decoder-snapshot <source> <nonce>`; let the
    // tiny shell fixture echo the caller's nonce so the production parser
    // validates the full child command/wiring rather than only a fixture.
    fields[2] = "$3".to_owned();
    let script = format!("#!/bin/sh\nprintf '%s\\n' \"{}\"\n", fields.join(" "));
    fs::write(path, script).expect("write fake decoder snapshot helper");
    let mut permissions = fs::metadata(path)
        .expect("read fake helper metadata")
        .permissions();
    permissions.set_mode(0o700);
    fs::set_permissions(path, permissions).expect("make fake helper executable");
}
