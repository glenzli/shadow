use std::path::PathBuf;

use shadow_domain::RepresentationKind;

use super::{
    super::{
        RelinkCandidate, RelinkSource, SourceRelinkResolution, UnsupportedRelinkSource,
        WeakRelinkMetadata, discover_source_relink,
    },
    candidate_scenarios::{candidate, fingerprint, source},
};

#[test]
fn weak_discovery_requires_two_independent_facts() {
    let source = RelinkSource {
        path: PathBuf::from("/new/DSC_0001.NEF"),
        kind: RepresentationKind::OriginalRaw,
        fingerprint: fingerprint(10),
        metadata: WeakRelinkMetadata {
            file_name: Some("DSC_0001.NEF".to_owned()),
            captured_at_unix_seconds: None,
            camera_key: None,
        },
    };
    let candidate = RelinkCandidate {
        fingerprint: fingerprint(99),
        metadata: WeakRelinkMetadata {
            file_name: Some("DSC_0001.NEF".to_owned()),
            ..WeakRelinkMetadata::default()
        },
        ..candidate("irrelevant.NEF", 99)
    };

    assert_eq!(
        discover_source_relink(source, [candidate]),
        SourceRelinkResolution::NoLikelyCandidate
    );
}

#[test]
fn weak_discovery_reports_ambiguous_candidates_without_hashing() {
    let source = source("/new/DSC_0001.NEF", 10);
    let first = candidate("DSC_0001.NEF", 10);
    let second = candidate("DSC_0001.NEF", 10);

    let SourceRelinkResolution::Ambiguous { candidates } =
        discover_source_relink(source, [first.clone(), second.clone()])
    else {
        panic!("two plausible candidates must remain ambiguous")
    };
    assert_eq!(candidates, vec![first, second]);
}

#[test]
fn weak_discovery_deduplicates_multiple_locations_of_one_representation() {
    let source = source("/new/DSC_0001.NEF", 10);
    let first = candidate("DSC_0001.NEF", 10);
    let same_representation_elsewhere = RelinkCandidate {
        location_label: "/other-mounted-drive/DSC_0001.NEF".to_owned(),
        ..first.clone()
    };

    let SourceRelinkResolution::VerificationRequired(pending) =
        discover_source_relink(source, [first.clone(), same_representation_elsewhere])
    else {
        panic!("multiple locations of one representation are not ambiguous")
    };
    assert_eq!(pending.candidate, first);
}

#[test]
fn weak_discovery_rejects_derived_representations() {
    let source = RelinkSource {
        kind: RepresentationKind::Proxy,
        ..source("/new/proxy.jpg", 10)
    };

    assert_eq!(
        discover_source_relink(source, std::iter::empty()),
        SourceRelinkResolution::Unsupported(
            UnsupportedRelinkSource::UnsupportedRepresentationKind(RepresentationKind::Proxy)
        )
    );
}

#[test]
fn weak_discovery_accepts_a_renamed_file_only_with_three_matching_facts() {
    let source = RelinkSource {
        path: PathBuf::from("/new/renamed.nef"),
        kind: RepresentationKind::OriginalRaw,
        fingerprint: fingerprint(10),
        metadata: WeakRelinkMetadata {
            file_name: Some("renamed.nef".to_owned()),
            captured_at_unix_seconds: Some(1_700_000_000),
            camera_key: Some("nikon z 9".to_owned()),
        },
    };
    let candidate = candidate("DSC_0001.NEF", 10);

    let SourceRelinkResolution::VerificationRequired(pending) =
        discover_source_relink(source, [candidate])
    else {
        panic!("three matching facts should allow verification")
    };
    assert!(!pending.evidence.same_file_name());
    assert!(pending.evidence.same_byte_len());
    assert!(pending.evidence.same_capture_time());
    assert!(pending.evidence.same_camera());
}
