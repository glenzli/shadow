use std::fs;

use super::{DecoderSafetyRegistry, IsolatedDecodeObservation, safety_record_path};
use crate::isolated_proxy::{route_fixture::safety_fixture, route_identity::DecoderSafetyKey};

#[test]
fn child_crash_quarantine_persists_but_invalidates_when_source_changes() {
    let (root, source, helper) = safety_fixture("source-change");
    let key = DecoderSafetyKey::reference_proxy(&source, &helper).expect("fingerprint key");
    let metadata_probe = DecoderSafetyKey::legacy_metadata_probe(&source, &helper)
        .expect("fingerprint legacy probe key");
    assert_ne!(
        key, metadata_probe,
        "helper stages cannot share safety proof"
    );

    let serialized_key = serde_json::to_vec(&key).expect("serialize safety key");
    let expected_path = root
        .join("decode-helper")
        .join("safety")
        .join(format!("{}.json", blake3::hash(&serialized_key).to_hex()));
    assert_eq!(
        safety_record_path(&root, &key).expect("derive safety record path"),
        expected_path,
        "the serialized key digest and safety cache path are one persistence contract"
    );

    let first_process = DecoderSafetyRegistry::default();
    first_process
        .record(&root, &key, IsolatedDecodeObservation::ChildCrashed)
        .expect("persist child crash");

    let restarted_process = DecoderSafetyRegistry::default();
    assert_eq!(
        restarted_process
            .observation(&root, &key)
            .expect("read persisted crash"),
        Some(IsolatedDecodeObservation::ChildCrashed)
    );

    fs::write(&source, b"a deliberately changed and longer source payload")
        .expect("change source fixture");
    let changed_key =
        DecoderSafetyKey::reference_proxy(&source, &helper).expect("fingerprint changed key");
    assert_ne!(key, changed_key);
    assert_eq!(
        restarted_process
            .observation(&root, &changed_key)
            .expect("read changed source observation"),
        None,
        "a crash quarantine must not follow a changed source fingerprint"
    );

    fs::remove_dir_all(root).expect("remove safety fixture");
}
