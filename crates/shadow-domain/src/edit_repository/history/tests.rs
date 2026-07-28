use std::str::FromStr;

use super::*;

#[test]
fn repository_commit_is_content_addressed_and_round_trips_exact_bytes() {
    let commit = EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
        root: EditObjectId::from_bytes([7; 32]),
        parents: Vec::new(),
        message: Some("Initial Library edit state".into()),
        created_at_ms: 1_721_500_000_000,
    })
    .unwrap();
    assert_eq!(
        commit.id().to_string(),
        "f5a32b09534be03293c8eff4d14d96bcf19ab6e4c2ab7c66ce05dd0bcd3c8c64"
    );

    assert_eq!(
        EditRepositoryCommit::from_stored_parts(commit.id(), commit.canonical_json().to_vec())
            .unwrap(),
        commit
    );
}

#[test]
fn repository_commit_rejects_duplicate_parents_without_reordering_them() {
    let parent = EditCommitId::from_bytes([3; 32]);
    assert!(matches!(
        EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
            root: EditObjectId::from_bytes([7; 32]),
            parents: vec![parent, parent],
            message: None,
            created_at_ms: 0,
        }),
        Err(EditRepositoryError::DuplicateCommitParent)
    ));
}

#[test]
fn repository_ref_kind_keeps_its_wire_spelling() {
    for (kind, encoded) in [
        (EditRepositoryRefKind::Branch, "branch"),
        (EditRepositoryRefKind::NamedVersion, "named_version"),
        (EditRepositoryRefKind::Tag, "tag"),
    ] {
        assert_eq!(kind.as_str(), encoded);
        assert_eq!(EditRepositoryRefKind::from_str(encoded).unwrap(), kind);
    }
}
