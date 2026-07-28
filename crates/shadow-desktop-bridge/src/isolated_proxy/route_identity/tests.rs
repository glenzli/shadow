use std::fs;

use super::{DecoderSafetyKey, isolated_helper_implementation_identity};
use crate::isolated_proxy::route_fixture::safety_fixture;

#[test]
fn decoder_safety_key_samples_contents_beyond_size_and_timestamp() {
    let (root, source, helper) = safety_fixture("content-sample");
    fs::write(&source, b"aaaaaaaaaaaaaaaa").expect("write first same-size source");
    let first = DecoderSafetyKey::decoder_snapshot(&source, &helper)
        .expect("build first decoder safety key");
    fs::write(&source, b"bbbbbbbbbbbbbbbb").expect("replace same-size source");
    let second = DecoderSafetyKey::decoder_snapshot(&source, &helper)
        .expect("build second decoder safety key");
    assert_ne!(
        first.source_content_sample, second.source_content_sample,
        "same-size content replacement must change the helper cache identity"
    );
    assert_ne!(first, second);
    fs::remove_dir_all(root).expect("remove content-sample fixture");
}

#[test]
fn helper_implementation_identity_uses_contents_not_only_stat_metadata() {
    let (root, _source, helper) = safety_fixture("helper-content-identity");
    fs::write(&helper, b"aaaaaaaaaaaaaaaa").expect("write first same-size helper");
    let first = isolated_helper_implementation_identity(Some(&helper))
        .expect("build first helper implementation identity");
    fs::write(&helper, b"bbbbbbbbbbbbbbbb").expect("replace same-size helper");
    let second = isolated_helper_implementation_identity(Some(&helper))
        .expect("build second helper implementation identity");
    assert_ne!(
        first, second,
        "a helper replacement must invalidate the catalog-facing helper graph identity"
    );
    fs::remove_dir_all(root).expect("remove helper-content identity fixture");
}

#[test]
fn legacy_metadata_probe_key_keeps_its_persisted_protocol_identity() {
    let (root, source, helper) = safety_fixture("legacy-metadata-probe-identity");
    let key = DecoderSafetyKey::legacy_metadata_probe(&source, &helper)
        .expect("build legacy metadata-probe key");
    assert_eq!(key.helper_protocol, "shadow-probe-v1");
    assert_eq!(key.operation, "metadata-probe");
    let serialized = serde_json::to_string(&key).expect("serialize legacy metadata-probe key");
    assert!(
        serialized
            .ends_with("\"helper_protocol\":\"shadow-probe-v1\",\"operation\":\"metadata-probe\"}"),
        "serialized key field order is part of the persisted safety identity"
    );
    fs::remove_dir_all(root).expect("remove legacy metadata-probe fixture");
}
