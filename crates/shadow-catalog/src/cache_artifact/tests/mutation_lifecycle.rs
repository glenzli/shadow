use super::{
    super::*,
    artifact_fixtures::{artifact, registered_catalog},
};

#[test]
fn cached_artifact_round_trips_and_replaces_one_variant() {
    let (mut catalog, representation_id, source) = registered_catalog();
    for (version, digest_byte) in [("1", 1_u8), ("2", 2_u8)] {
        let status = catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: source,
                artifact: artifact(version, digest_byte),
            })
            .expect("record artifact");
        assert_eq!(status, RecordCachedArtifactStatus::Recorded);
    }

    let records = catalog
        .cached_artifacts(representation_id)
        .expect("read artifacts");
    assert_eq!(records.len(), 1);
    assert_eq!(records[0].source, source);
    assert_eq!(records[0].artifact.generator_version, "2");
    assert_eq!(records[0].artifact.blob_digest, [2_u8; 32]);
}

#[test]
fn stale_artifact_reference_is_not_committed() {
    let (mut catalog, representation_id, source) = registered_catalog();
    let status = catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: source.byte_len + 1,
                modified_at_ms: source.modified_at_ms,
            },
            artifact: artifact("1", 1),
        })
        .expect("reject stale artifact");

    assert_eq!(status, RecordCachedArtifactStatus::StaleSource);
    assert!(
        catalog
            .cached_artifacts(representation_id)
            .expect("read artifacts")
            .is_empty()
    );
}

#[test]
fn invalidation_cannot_delete_a_concurrently_replaced_artifact() {
    let (mut catalog, representation_id, source) = registered_catalog();
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: artifact("1", 1),
        })
        .expect("record first artifact");
    let stale = catalog
        .cached_artifacts(representation_id)
        .expect("read first artifact")
        .remove(0);

    let mut replacement = artifact("2", 2);
    replacement.created_at_ms += 1;
    catalog
        .record_cached_artifact(&RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: replacement,
        })
        .expect("record replacement");
    assert_eq!(
        catalog
            .invalidate_cached_artifact(&stale)
            .expect("ignore stale invalidation"),
        InvalidateCachedArtifactStatus::NotCurrent
    );

    let current = catalog
        .cached_artifacts(representation_id)
        .expect("read replacement")
        .remove(0);
    assert_eq!(current.artifact.generator_version, "2");
    assert_eq!(
        catalog
            .invalidate_cached_artifact(&current)
            .expect("invalidate current artifact"),
        InvalidateCachedArtifactStatus::Invalidated
    );
    assert!(
        catalog
            .cached_artifacts(representation_id)
            .expect("read empty artifacts")
            .is_empty()
    );
}
