use std::{fs, path::Path};

use super::{
    NativeDecodeAdmission, native_decode_admission_after_isolated_stages_for,
    native_decode_admission_from_observation,
};
use crate::isolated_proxy::{
    persistent_evidence::{DecoderSafetyRegistry, IsolatedDecodeObservation},
    route_fixture::safety_fixture,
    route_identity::DecoderSafetyKey,
};

#[test]
fn legacy_proxy_success_never_promotes_native_raw_safety() {
    assert_eq!(
        native_decode_admission_from_observation(Some(
            IsolatedDecodeObservation::LegacyStageSucceededUnproven,
        )),
        NativeDecodeAdmission::NotQuarantined,
        "the legacy helper only proves bounded RGB proxy success"
    );
    assert_eq!(
        native_decode_admission_from_observation(Some(IsolatedDecodeObservation::ChildCrashed,)),
        NativeDecodeAdmission::Quarantined {
            observation: IsolatedDecodeObservation::ChildCrashed,
        }
    );
}

#[test]
fn aggregate_admission_fails_closed_for_either_isolated_stage() {
    let (root, source, helper) = safety_fixture("aggregate-stage-admission");
    let stages = [
        (
            "legacy-metadata-probe",
            DecoderSafetyKey::legacy_metadata_probe(&source, &helper)
                .expect("legacy metadata-probe safety key"),
            IsolatedDecodeObservation::ChildCrashed,
        ),
        (
            "metadata-snapshot",
            DecoderSafetyKey::metadata_snapshot(&source, &helper)
                .expect("metadata snapshot safety key"),
            IsolatedDecodeObservation::ChildCrashed,
        ),
        (
            "decoder-snapshot",
            DecoderSafetyKey::decoder_snapshot(&source, &helper)
                .expect("decoder snapshot safety key"),
            IsolatedDecodeObservation::ChildCrashed,
        ),
        (
            "reference-proxy",
            DecoderSafetyKey::reference_proxy(&source, &helper)
                .expect("reference-proxy safety key"),
            IsolatedDecodeObservation::TimedOut,
        ),
    ];

    for (stage, key, observation) in stages {
        let stage_root = root.join(stage);
        fs::create_dir_all(&stage_root).expect("create stage safety root");
        let registry = DecoderSafetyRegistry::default();
        registry
            .record(&stage_root, &key, observation)
            .expect("persist stage negative evidence");
        assert_eq!(
            admission_for(&registry, &stage_root, &source, &helper),
            NativeDecodeAdmission::Quarantined { observation },
            "{stage} negative evidence must block a later direct RawFrame session"
        );
    }

    fs::remove_dir_all(root).expect("remove aggregate safety fixture");
}

fn admission_for(
    registry: &DecoderSafetyRegistry,
    runtime_cache_root: &Path,
    source: &Path,
    helper: &Path,
) -> NativeDecodeAdmission {
    native_decode_admission_after_isolated_stages_for(registry, runtime_cache_root, source, helper)
        .expect("read aggregate safety admission")
}
