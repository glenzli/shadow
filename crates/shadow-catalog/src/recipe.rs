use rusqlite::{OptionalExtension, Transaction, params};
use shadow_domain::{
    EntityId, PhotoId, RecipeCommit, RecipeCommitId, RecipeId, canonical_recipe_snapshot_digest,
};

use crate::{Catalog, CatalogError, cache_artifact::digest, read_id};

/// The semantic role of a movable name that points at an immutable commit.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub enum RecipeRefKind {
    Working,
    Branch,
    NamedVersion,
    Tag,
}

impl RecipeRefKind {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Working => "working",
            Self::Branch => "branch",
            Self::NamedVersion => "named_version",
            Self::Tag => "tag",
        }
    }
}

/// One ref update performed atomically with a new immutable commit.
///
/// Callers that read a ref before deriving a new commit should carry that
/// observed state in [`Self::expectation`] to avoid overwriting a newer head.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RecipeRefTarget {
    pub name: String,
    pub kind: RecipeRefKind,
    /// Optional compare-and-swap guard for the ref's current commit.
    ///
    /// `None` preserves the original unconditional update behavior.
    pub expectation: Option<RecipeRefExpectation>,
}

/// The state a Recipe ref must have before an atomic commit may move it.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub enum RecipeRefExpectation {
    /// The ref must not exist yet.
    Missing,
    /// The ref must currently name this immutable commit.
    At(RecipeCommitId),
}

/// Writes one already-validated domain commit for a photo.
#[derive(Debug, Clone, PartialEq)]
pub struct CommitRecipe {
    pub photo_id: PhotoId,
    pub commit: RecipeCommit,
    pub update_refs: Vec<RecipeRefTarget>,
}

/// An immutable commit plus its durable content identity and owner.
#[derive(Debug, Clone, PartialEq)]
pub struct RecipeCommitRecord {
    pub photo_id: PhotoId,
    pub commit: RecipeCommit,
    pub snapshot_digest: [u8; 32],
}

/// A durable movable name. Moving it never mutates or deletes a commit.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RecipeRefRecord {
    pub photo_id: PhotoId,
    pub name: String,
    pub kind: RecipeRefKind,
    pub commit_id: RecipeCommitId,
    pub updated_at_ms: i64,
}

/// Explicitly moves or creates one ref after validating commit ownership.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct SetRecipeRef {
    pub photo_id: PhotoId,
    pub name: String,
    pub kind: RecipeRefKind,
    pub commit_id: RecipeCommitId,
    pub updated_at_ms: i64,
}

impl Catalog {
    /// Atomically inserts an immutable Recipe commit, its ordered parent edges,
    /// and an optional movable ref.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the snapshot is invalid, a parent belongs
    /// to another photo, the commit id already exists, a guarded ref no longer
    /// has its expected head, or persistence fails. An expectation mismatch
    /// leaves both the immutable commit and every requested ref update absent.
    pub fn commit_recipe(
        &mut self,
        request: &CommitRecipe,
    ) -> Result<RecipeCommitRecord, CatalogError> {
        let transaction = self.connection.transaction()?;
        let record = commit_recipe_in_transaction(&transaction, request)?;
        transaction.commit()?;
        Ok(record)
    }

    /// Returns every immutable commit for a photo, newest author timestamp first.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when stored JSON, identity, or graph state is invalid.
    pub fn recipe_commits(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<RecipeCommitRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT id, recipe_id, commit_json, snapshot_digest
             FROM recipe_commits
             WHERE photo_id = ?1
             ORDER BY created_at_ms DESC, id DESC",
        )?;
        let rows = statement.query_map([photo_id.as_bytes().as_slice()], |row| {
            Ok((
                read_id::<RecipeCommitId>(row, 0)?,
                read_id::<RecipeId>(row, 1)?,
                row.get::<_, String>(2)?,
                digest(row.get(3)?, 3)?,
            ))
        })?;
        let mut records = Vec::new();
        for row in rows {
            let (stored_id, stored_recipe_id, json, snapshot_digest) = row?;
            records.push(decode_recipe_record(
                &self.connection,
                photo_id,
                stored_id,
                stored_recipe_id,
                &json,
                snapshot_digest,
            )?);
        }
        Ok(records)
    }

    /// Returns one immutable commit by owner and id without scanning the
    /// photo's history.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when stored JSON, normalized parents, content
    /// identity, or graph state is invalid.
    pub fn recipe_commit(
        &self,
        photo_id: PhotoId,
        commit_id: RecipeCommitId,
    ) -> Result<Option<RecipeCommitRecord>, CatalogError> {
        let stored = self
            .connection
            .query_row(
                "SELECT id, recipe_id, commit_json, snapshot_digest
                 FROM recipe_commits WHERE photo_id = ?1 AND id = ?2",
                params![
                    photo_id.as_bytes().as_slice(),
                    commit_id.as_bytes().as_slice()
                ],
                |row| {
                    Ok((
                        read_id::<RecipeCommitId>(row, 0)?,
                        read_id::<RecipeId>(row, 1)?,
                        row.get::<_, String>(2)?,
                        digest(row.get(3)?, 3)?,
                    ))
                },
            )
            .optional()?;
        stored
            .map(|(stored_id, stored_recipe_id, json, snapshot_digest)| {
                decode_recipe_record(
                    &self.connection,
                    photo_id,
                    stored_id,
                    stored_recipe_id,
                    &json,
                    snapshot_digest,
                )
            })
            .transpose()
    }

    /// Returns one ref and the immutable commit id it currently names.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid ref name or persisted value.
    pub fn recipe_ref(
        &self,
        photo_id: PhotoId,
        name: &str,
    ) -> Result<Option<RecipeRefRecord>, CatalogError> {
        validate_ref_name(name)?;
        self.connection
            .query_row(
                "SELECT kind, commit_id, updated_at_ms
                 FROM recipe_refs WHERE photo_id = ?1 AND name = ?2",
                params![photo_id.as_bytes().as_slice(), name],
                |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        read_id::<RecipeCommitId>(row, 1)?,
                        row.get::<_, i64>(2)?,
                    ))
                },
            )
            .optional()?
            .map(|(kind, commit_id, updated_at_ms)| {
                Ok(RecipeRefRecord {
                    photo_id,
                    name: name.to_owned(),
                    kind: parse_ref_kind(&kind)?,
                    commit_id,
                    updated_at_ms,
                })
            })
            .transpose()
    }

    /// Moves a ref to an existing commit owned by the same photo.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the name is invalid, the target is absent or
    /// belongs to another photo, or the transaction fails.
    pub fn set_recipe_ref(&mut self, request: &SetRecipeRef) -> Result<(), CatalogError> {
        validate_ref_name(&request.name)?;
        let transaction = self.connection.transaction()?;
        ensure_commit_owner(&transaction, request.photo_id, request.commit_id)?;
        upsert_ref(
            &transaction,
            request.photo_id,
            &request.name,
            request.kind,
            request.commit_id,
            request.updated_at_ms,
        )?;
        transaction.commit()?;
        Ok(())
    }

    /// Removes every persisted Recipe commit and ref owned by one photo.
    ///
    /// This deliberately narrow escape hatch is for development-only Recipe
    /// contract breaks. It never touches the photo, its original bytes, review
    /// decisions, cached visuals, or Library organization. Production schema
    /// migrations must use an explicit migration instead.
    pub fn discard_recipe_history(&mut self, photo_id: PhotoId) -> Result<usize, CatalogError> {
        let transaction = self.connection.transaction()?;
        transaction.execute(
            "DELETE FROM recipe_refs WHERE photo_id = ?1",
            [photo_id.as_bytes().as_slice()],
        )?;
        // `recipe_commit_parents` contains a RESTRICT edge to the parent commit.
        // Deleting all commits in one statement is therefore not sufficient for
        // a history with more than one commit: SQLite may inspect the child edge
        // before its CASCADE edge has removed that row. Remove the photo-owned
        // graph edges explicitly before deleting its vertices.
        transaction.execute(
            "DELETE FROM recipe_commit_parents WHERE photo_id = ?1",
            [photo_id.as_bytes().as_slice()],
        )?;
        let removed = transaction.execute(
            "DELETE FROM recipe_commits WHERE photo_id = ?1",
            [photo_id.as_bytes().as_slice()],
        )?;
        transaction.commit()?;
        Ok(removed)
    }
}

pub(crate) fn commit_recipe_in_transaction(
    transaction: &Transaction<'_>,
    request: &CommitRecipe,
) -> Result<RecipeCommitRecord, CatalogError> {
    request
        .commit
        .validate()
        .map_err(|error| CatalogError::InvalidRecipe(error.to_string()))?;
    let mut ref_names = std::collections::BTreeSet::new();
    for target in &request.update_refs {
        validate_ref_name(&target.name)?;
        if !ref_names.insert(&target.name) {
            return Err(CatalogError::DuplicateRecipeRefName(target.name.clone()));
        }
    }
    let commit_json = serde_json::to_string(&request.commit).map_err(CatalogError::RecipeJson)?;
    let snapshot_digest = canonical_recipe_snapshot_digest(request.commit.snapshot())
        .map_err(CatalogError::RecipeJson)?;
    ensure_photo(transaction, request.photo_id)?;
    ensure_commit_absent(transaction, request.commit.id())?;
    for parent in request.commit.parents() {
        ensure_commit_owner(transaction, request.photo_id, *parent)?;
    }
    for target in &request.update_refs {
        ensure_ref_expectation(transaction, request.photo_id, target)?;
    }
    transaction.execute(
        "INSERT INTO recipe_commits(
             id, photo_id, recipe_id, commit_json, snapshot_digest, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![
            request.commit.id().as_bytes().as_slice(),
            request.photo_id.as_bytes().as_slice(),
            request.commit.recipe_id().as_bytes().as_slice(),
            commit_json,
            snapshot_digest.as_slice(),
            request.commit.created_at_ms(),
        ],
    )?;
    for (position, parent) in request.commit.parents().iter().enumerate() {
        let position = i64::try_from(position).map_err(|error| {
            CatalogError::Sqlite(rusqlite::Error::ToSqlConversionFailure(Box::new(error)))
        })?;
        transaction.execute(
            "INSERT INTO recipe_commit_parents(commit_id, parent_id, photo_id, position)
             VALUES (?1, ?2, ?3, ?4)",
            params![
                request.commit.id().as_bytes().as_slice(),
                parent.as_bytes().as_slice(),
                request.photo_id.as_bytes().as_slice(),
                position,
            ],
        )?;
    }
    for target in &request.update_refs {
        upsert_ref(
            transaction,
            request.photo_id,
            &target.name,
            target.kind,
            request.commit.id(),
            request.commit.created_at_ms(),
        )?;
    }
    Ok(RecipeCommitRecord {
        photo_id: request.photo_id,
        commit: request.commit.clone(),
        snapshot_digest,
    })
}

fn decode_recipe_record(
    connection: &rusqlite::Connection,
    photo_id: PhotoId,
    stored_id: RecipeCommitId,
    stored_recipe_id: RecipeId,
    json: &str,
    snapshot_digest: [u8; 32],
) -> Result<RecipeCommitRecord, CatalogError> {
    let commit: RecipeCommit = serde_json::from_str(json).map_err(CatalogError::RecipeJson)?;
    commit
        .validate()
        .map_err(|error| CatalogError::InvalidRecipe(error.to_string()))?;
    if commit.id() != stored_id || commit.recipe_id() != stored_recipe_id {
        return Err(CatalogError::InvalidRecipe(
            "stored Recipe JSON identity disagrees with its indexed columns".into(),
        ));
    }
    if commit.parents() != stored_parents(connection, stored_id)? {
        return Err(CatalogError::InvalidRecipe(
            "stored Recipe JSON parents disagree with normalized parent edges".into(),
        ));
    }
    let canonical_digest =
        canonical_recipe_snapshot_digest(commit.snapshot()).map_err(CatalogError::RecipeJson)?;
    if canonical_digest != snapshot_digest {
        // The digest is a cache identity for the semantic Recipe snapshot, not
        // a checksum of one incidental JSON object-key order. Older v1 writers
        // hashed the direct struct serialization; a deserialize/serialize
        // round trip could reorder nested flattened maps without changing a
        // single Recipe value. Repair that redundant identity in place after
        // the full Recipe and normalized parent edges have validated.
        connection.execute(
            "UPDATE recipe_commits
             SET snapshot_digest = ?1
             WHERE photo_id = ?2 AND id = ?3 AND snapshot_digest = ?4",
            params![
                canonical_digest.as_slice(),
                photo_id.as_bytes().as_slice(),
                stored_id.as_bytes().as_slice(),
                snapshot_digest.as_slice(),
            ],
        )?;
    }
    Ok(RecipeCommitRecord {
        photo_id,
        commit,
        snapshot_digest: canonical_digest,
    })
}

fn ensure_photo(transaction: &Transaction<'_>, photo_id: PhotoId) -> Result<(), CatalogError> {
    let present = transaction.query_row(
        "SELECT EXISTS(SELECT 1 FROM photos WHERE id = ?1)",
        [photo_id.as_bytes().as_slice()],
        |row| row.get::<_, bool>(0),
    )?;
    if present {
        Ok(())
    } else {
        Err(CatalogError::PhotoNotFound(photo_id))
    }
}

fn ensure_commit_absent(
    transaction: &Transaction<'_>,
    commit_id: RecipeCommitId,
) -> Result<(), CatalogError> {
    let present = transaction.query_row(
        "SELECT EXISTS(SELECT 1 FROM recipe_commits WHERE id = ?1)",
        [commit_id.as_bytes().as_slice()],
        |row| row.get::<_, bool>(0),
    )?;
    if present {
        Err(CatalogError::RecipeCommitAlreadyExists(commit_id))
    } else {
        Ok(())
    }
}

fn ensure_commit_owner(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
    commit_id: RecipeCommitId,
) -> Result<(), CatalogError> {
    let owner = transaction
        .query_row(
            "SELECT photo_id FROM recipe_commits WHERE id = ?1",
            [commit_id.as_bytes().as_slice()],
            |row| read_id::<PhotoId>(row, 0),
        )
        .optional()?;
    match owner {
        Some(owner) if owner == photo_id => Ok(()),
        Some(_) => Err(CatalogError::RecipeCommitOwnerMismatch {
            photo_id,
            commit_id,
        }),
        None => Err(CatalogError::RecipeCommitNotFound(commit_id)),
    }
}

fn ensure_ref_expectation(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
    target: &RecipeRefTarget,
) -> Result<(), CatalogError> {
    let Some(expected) = target.expectation else {
        return Ok(());
    };
    let actual = transaction
        .query_row(
            "SELECT commit_id FROM recipe_refs WHERE photo_id = ?1 AND name = ?2",
            params![photo_id.as_bytes().as_slice(), &target.name],
            |row| read_id::<RecipeCommitId>(row, 0),
        )
        .optional()?;
    let matches = match expected {
        RecipeRefExpectation::Missing => actual.is_none(),
        RecipeRefExpectation::At(commit_id) => actual == Some(commit_id),
    };
    if matches {
        Ok(())
    } else {
        Err(CatalogError::RecipeRefExpectationMismatch {
            photo_id,
            name: target.name.clone(),
            expected,
            actual,
        })
    }
}

fn upsert_ref(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
    name: &str,
    kind: RecipeRefKind,
    commit_id: RecipeCommitId,
    updated_at_ms: i64,
) -> Result<(), CatalogError> {
    transaction.execute(
        "INSERT INTO recipe_refs(photo_id, name, kind, commit_id, updated_at_ms)
         VALUES (?1, ?2, ?3, ?4, ?5)
         ON CONFLICT(photo_id, name) DO UPDATE SET
             kind = excluded.kind,
             commit_id = excluded.commit_id,
             updated_at_ms = excluded.updated_at_ms",
        params![
            photo_id.as_bytes().as_slice(),
            name,
            kind.as_str(),
            commit_id.as_bytes().as_slice(),
            updated_at_ms,
        ],
    )?;
    Ok(())
}

fn stored_parents(
    connection: &rusqlite::Connection,
    commit_id: RecipeCommitId,
) -> Result<Vec<RecipeCommitId>, CatalogError> {
    let mut statement = connection.prepare(
        "SELECT parent_id FROM recipe_commit_parents
         WHERE commit_id = ?1 ORDER BY position",
    )?;
    statement
        .query_map([commit_id.as_bytes().as_slice()], |row| {
            read_id::<RecipeCommitId>(row, 0)
        })?
        .collect::<rusqlite::Result<Vec<_>>>()
        .map_err(Into::into)
}

fn validate_ref_name(name: &str) -> Result<(), CatalogError> {
    if name.is_empty() || name.len() > 255 || name.chars().any(char::is_control) {
        return Err(CatalogError::InvalidRecipeRefName(name.to_owned()));
    }
    Ok(())
}

fn parse_ref_kind(value: &str) -> Result<RecipeRefKind, CatalogError> {
    match value {
        "working" => Ok(RecipeRefKind::Working),
        "branch" => Ok(RecipeRefKind::Branch),
        "named_version" => Ok(RecipeRefKind::NamedVersion),
        "tag" => Ok(RecipeRefKind::Tag),
        _ => Err(CatalogError::UnknownRecipeRefKind(value.to_owned())),
    }
}

#[cfg(test)]
mod tests {
    use shadow_domain::{AssetLocation, EntityId, Platform, RecipeSnapshot, RepresentationKind};

    use super::*;
    use crate::RegisterAsset;

    #[test]
    fn immutable_commits_branch_without_overwriting_the_previous_head() {
        let (mut catalog, photo_id) = catalog_with_photo("/photos/edit.dng");
        let recipe_id = RecipeId::new_v7();
        let root = commit(recipe_id, Vec::new(), "Natural base", 100);
        catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: root.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: None,
                }],
            })
            .expect("commit root");

        let warm = commit(recipe_id, vec![root.id()], "Warm editorial", 200);
        catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: warm.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: None,
                }],
            })
            .expect("commit child");
        assert_eq!(
            catalog
                .recipe_ref(photo_id, "working")
                .expect("read working ref")
                .expect("working ref")
                .commit_id,
            warm.id()
        );

        catalog
            .set_recipe_ref(&SetRecipeRef {
                photo_id,
                name: "versions/natural-base".into(),
                kind: RecipeRefKind::NamedVersion,
                commit_id: root.id(),
                updated_at_ms: 250,
            })
            .expect("name old version");
        let commits = catalog.recipe_commits(photo_id).expect("list history");
        assert_eq!(commits.len(), 2);
        assert_eq!(commits[0].commit.id(), warm.id());
        assert_eq!(commits[1].commit.id(), root.id());
        assert_eq!(commits[1].commit.message(), Some("Natural base"));
        assert_eq!(commits[0].snapshot_digest, commits[1].snapshot_digest);
    }

    #[test]
    fn development_reset_discards_only_one_photos_recipe_history() {
        let (mut catalog, photo_id) = catalog_with_photo("/photos/reset-me.dng");
        let recipe_id = RecipeId::new_v7();
        let root = commit(recipe_id, Vec::new(), "Old development edit", 100);
        catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: root.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::Missing),
                }],
            })
            .expect("persist old working edit");
        let child = commit(recipe_id, vec![root.id()], "Newer development edit", 200);
        catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: child,
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::At(root.id())),
                }],
            })
            .expect("persist child working edit");

        assert_eq!(
            catalog
                .discard_recipe_history(photo_id)
                .expect("discard development history"),
            2
        );
        assert!(
            catalog
                .recipe_commits(photo_id)
                .expect("list discarded history")
                .is_empty()
        );
        assert!(
            catalog
                .recipe_ref(photo_id, "working")
                .expect("read discarded working ref")
                .is_none()
        );
    }

    #[test]
    fn commit_can_require_a_recipe_ref_to_be_missing() {
        let (mut catalog, photo_id) = catalog_with_photo("/photos/missing-ref.dng");
        let root = commit(RecipeId::new_v7(), Vec::new(), "Root", 100);

        catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: root.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::Missing),
                }],
            })
            .expect("create missing ref atomically");

        assert_eq!(
            catalog
                .recipe_ref(photo_id, "working")
                .expect("read working ref")
                .expect("working ref")
                .commit_id,
            root.id()
        );
    }

    #[test]
    fn commit_can_compare_and_swap_a_recipe_ref_at_its_expected_head() {
        let (mut catalog, photo_id) = catalog_with_photo("/photos/expected-head.dng");
        let recipe_id = RecipeId::new_v7();
        let root = commit(recipe_id, Vec::new(), "Root", 100);
        catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: root.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::Missing),
                }],
            })
            .expect("commit root");
        let child = commit(recipe_id, vec![root.id()], "Child", 200);

        catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: child.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::At(root.id())),
                }],
            })
            .expect("advance expected head");

        assert_eq!(
            catalog
                .recipe_ref(photo_id, "working")
                .expect("read working ref")
                .expect("working ref")
                .commit_id,
            child.id()
        );
    }

    #[test]
    fn stale_recipe_ref_expectation_rolls_back_commit_and_every_ref_move() {
        let (mut catalog, photo_id) = catalog_with_photo("/photos/stale-head.dng");
        let recipe_id = RecipeId::new_v7();
        let root = commit(recipe_id, Vec::new(), "Root", 100);
        catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: root.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::Missing),
                }],
            })
            .expect("commit root");
        let current = commit(recipe_id, vec![root.id()], "Current", 200);
        catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: current.clone(),
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::At(root.id())),
                }],
            })
            .expect("advance working ref");
        let stale = commit(recipe_id, vec![root.id()], "Stale", 300);

        assert!(matches!(
            catalog.commit_recipe(&CommitRecipe {
                photo_id,
                commit: stale.clone(),
                update_refs: vec![
                    RecipeRefTarget {
                        name: "working".into(),
                        kind: RecipeRefKind::Working,
                        expectation: Some(RecipeRefExpectation::At(root.id())),
                    },
                    RecipeRefTarget {
                        name: "versions/stale".into(),
                        kind: RecipeRefKind::NamedVersion,
                        expectation: None,
                    },
                ],
            }),
            Err(CatalogError::RecipeRefExpectationMismatch {
                photo_id: error_photo_id,
                ref name,
                expected: RecipeRefExpectation::At(expected),
                actual: Some(actual),
            }) if error_photo_id == photo_id
                && name == "working"
                && expected == root.id()
                && actual == current.id()
        ));
        assert!(
            catalog
                .recipe_commit(photo_id, stale.id())
                .expect("query stale commit")
                .is_none()
        );
        assert_eq!(
            catalog
                .recipe_ref(photo_id, "working")
                .expect("read working ref")
                .expect("working ref")
                .commit_id,
            current.id()
        );
        assert!(
            catalog
                .recipe_ref(photo_id, "versions/stale")
                .expect("read untouched secondary ref")
                .is_none()
        );
        assert_eq!(
            catalog
                .recipe_commits(photo_id)
                .expect("list committed history")
                .len(),
            2
        );
    }

    #[test]
    fn commits_cannot_be_replaced_or_parented_across_photos() {
        let (mut catalog, first_photo) = catalog_with_photo("/photos/one.dng");
        let second = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/two.dng".to_vec(),
                    "/photos/two.dng",
                ),
                byte_len: 2,
                modified_at_ms: Some(2),
                now_ms: 2,
            })
            .expect("register second photo");
        let recipe_id = RecipeId::new_v7();
        let root = commit(recipe_id, Vec::new(), "Root", 100);
        let request = CommitRecipe {
            photo_id: first_photo,
            commit: root.clone(),
            update_refs: Vec::new(),
        };
        catalog.commit_recipe(&request).expect("commit root");
        assert!(matches!(
            catalog.commit_recipe(&request),
            Err(CatalogError::RecipeCommitAlreadyExists(id)) if id == root.id()
        ));

        let invalid_child = commit(recipe_id, vec![root.id()], "Wrong owner", 200);
        assert!(matches!(
            catalog.commit_recipe(&CommitRecipe {
                photo_id: second.photo_id,
                commit: invalid_child,
                update_refs: Vec::new(),
            }),
            Err(CatalogError::RecipeCommitOwnerMismatch { .. })
        ));
    }

    #[test]
    fn recipe_history_survives_catalog_close_and_reopen() {
        let root = std::env::temp_dir().join(format!(
            "shadow-recipe-catalog-{}-{}",
            std::process::id(),
            RecipeCommitId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create test root");
        let path = root.join("catalog.sqlite");
        let (photo_id, commit_id, digest) = {
            let mut catalog = Catalog::open(&path).expect("open catalog");
            let registered = catalog
                .register_asset(&RegisterAsset {
                    kind: RepresentationKind::OriginalRaw,
                    location: AssetLocation::new(
                        Platform::MacOs,
                        b"/photos/reopen.dng".to_vec(),
                        "/photos/reopen.dng",
                    ),
                    byte_len: 10,
                    modified_at_ms: Some(10),
                    now_ms: 10,
                })
                .expect("register photo");
            let commit = commit(RecipeId::new_v7(), Vec::new(), "Persistent", 20);
            let record = catalog
                .commit_recipe(&CommitRecipe {
                    photo_id: registered.photo_id,
                    commit: commit.clone(),
                    update_refs: vec![RecipeRefTarget {
                        name: "working".into(),
                        kind: RecipeRefKind::Working,
                        expectation: None,
                    }],
                })
                .expect("commit Recipe");
            (registered.photo_id, commit.id(), record.snapshot_digest)
        };

        let connection = rusqlite::Connection::open(&path).expect("open persisted recipe directly");
        connection
            .execute(
                "UPDATE recipe_commits SET snapshot_digest = zeroblob(32) WHERE id = ?1",
                [commit_id.as_bytes().as_slice()],
            )
            .expect("simulate a stale non-canonical v1 digest");
        drop(connection);

        let catalog = Catalog::open(&path).expect("reopen catalog");
        let records = catalog.recipe_commits(photo_id).expect("reload commits");
        assert_eq!(records.len(), 1);
        assert_eq!(records[0].commit.id(), commit_id);
        assert_eq!(records[0].snapshot_digest, digest);
        let repaired_digest: Vec<u8> = catalog
            .connection
            .query_row(
                "SELECT snapshot_digest FROM recipe_commits WHERE id = ?1",
                [commit_id.as_bytes().as_slice()],
                |row| row.get(0),
            )
            .expect("read repaired digest");
        assert_eq!(repaired_digest, digest);
        let direct = catalog
            .recipe_commit(photo_id, commit_id)
            .expect("load one commit")
            .expect("commit exists");
        assert_eq!(direct, records[0]);
        assert!(
            catalog
                .recipe_commit(photo_id, RecipeCommitId::new_v7())
                .expect("query absent commit")
                .is_none()
        );
        assert_eq!(
            catalog
                .recipe_ref(photo_id, "working")
                .expect("read ref")
                .expect("working ref")
                .commit_id,
            commit_id
        );
        drop(catalog);
        std::fs::remove_dir_all(root).expect("remove test root");
    }

    #[test]
    #[allow(clippy::too_many_lines)]
    #[cfg(any())]
    fn v9_recipe_row_remains_byte_exact_after_appending_a_child_commit() {
        const ROOT_COMMIT_JSON: &str = r#"{
  "id": "00000000-0000-7000-8000-000000000101",
  "recipe_id": "00000000-0000-7000-8000-000000000102",
  "parents": [],
  "snapshot": { "schema_version": 1, "layers": [] },
  "message": "v9 byte-preservation golden",
  "created_at_ms": 1721500000100
}"#;
        const SNAPSHOT_JSON: &[u8] = br#"{"schema_version":1,"layers":[]}"#;
        const SNAPSHOT_DIGEST: [u8; 32] = [
            0xa1, 0x3b, 0xc3, 0x3a, 0x18, 0x2b, 0x9f, 0xe3, 0x47, 0x6e, 0x1c, 0xb0, 0xdc, 0x1c,
            0x3b, 0x93, 0x9b, 0xdf, 0xb0, 0x3f, 0x72, 0x58, 0x47, 0xdb, 0x81, 0xa5, 0x1d, 0xd5,
            0x33, 0xa0, 0x55, 0x40,
        ];

        let root_dir = std::env::temp_dir().join(format!(
            "shadow-recipe-v9-golden-{}-{}",
            std::process::id(),
            RecipeCommitId::new_v7()
        ));
        std::fs::create_dir_all(&root_dir).expect("create v9 fixture root");
        let path = root_dir.join("catalog.sqlite");
        let photo_id = "00000000-0000-7000-8000-000000000100"
            .parse::<PhotoId>()
            .expect("fixed photo id");
        assert_eq!(
            blake3::hash(SNAPSHOT_JSON).as_bytes(),
            &SNAPSHOT_DIGEST,
            "Recipe v1 snapshot digest changed"
        );
        let root: RecipeCommit =
            serde_json::from_str(ROOT_COMMIT_JSON).expect("parse raw v9 commit JSON");
        root.validate().expect("raw v9 commit remains valid");
        {
            let mut connection = rusqlite::Connection::open(&path).expect("open v9 fixture");
            connection
                .execute_batch(
                    "PRAGMA foreign_keys = ON;
                     CREATE TABLE schema_migrations (
                         version       INTEGER PRIMARY KEY NOT NULL,
                         applied_at_ms INTEGER NOT NULL
                     ) STRICT;",
                )
                .expect("initialize v9 fixture");
            for (version, sql) in [
                (1_i64, crate::MIGRATION_V1),
                (2, crate::MIGRATION_V2),
                (3, crate::MIGRATION_V3),
                (4, crate::MIGRATION_V4),
                (5, crate::MIGRATION_V5),
                (6, crate::MIGRATION_V6),
                (7, crate::MIGRATION_V7),
                (8, crate::MIGRATION_V8),
                (9, crate::MIGRATION_V9),
            ] {
                let transaction = connection.transaction().expect("start v9 migration");
                transaction
                    .execute_batch(sql)
                    .expect("apply v9 fixture migration");
                transaction
                    .execute(
                        "INSERT INTO schema_migrations(version, applied_at_ms)
                         VALUES (?1, ?1)",
                        [version],
                    )
                    .expect("record v9 fixture migration");
                transaction.commit().expect("commit v9 fixture migration");
            }
            connection
                .execute(
                    "INSERT INTO photos(id, created_at_ms) VALUES (?1, ?2)",
                    params![photo_id.as_bytes().as_slice(), 1_721_500_000_000_i64],
                )
                .expect("insert v9 fixture photo");
            connection
                .execute(
                    "INSERT INTO recipe_commits(
                     id, photo_id, recipe_id, commit_json, snapshot_digest, created_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                    params![
                        root.id().as_bytes().as_slice(),
                        photo_id.as_bytes().as_slice(),
                        root.recipe_id().as_bytes().as_slice(),
                        ROOT_COMMIT_JSON,
                        SNAPSHOT_DIGEST.as_slice(),
                        root.created_at_ms(),
                    ],
                )
                .expect("insert raw v9 Recipe row");
        }

        let mut catalog = Catalog::open(&path).expect("open and migrate v9 Recipe fixture");
        assert!(
            catalog.schema_version().expect("current schema") >= 9,
            "v9 fixture migrated backwards"
        );

        let loaded = catalog
            .recipe_commit(photo_id, root.id())
            .expect("read raw v9 Recipe row")
            .expect("raw v9 Recipe row exists");
        assert_eq!(loaded.commit, root);
        assert_eq!(loaded.snapshot_digest, SNAPSHOT_DIGEST);

        let read_raw_root = |catalog: &Catalog| {
            catalog
                .connection
                .query_row(
                    "SELECT CAST(commit_json AS BLOB), snapshot_digest
                     FROM recipe_commits WHERE id = ?1",
                    [root.id().as_bytes().as_slice()],
                    |row| Ok((row.get::<_, Vec<u8>>(0)?, row.get::<_, Vec<u8>>(1)?)),
                )
                .expect("read raw Recipe bytes")
        };
        let before = read_raw_root(&catalog);
        assert_eq!(before.0.as_slice(), ROOT_COMMIT_JSON.as_bytes());
        assert_eq!(before.1.as_slice(), SNAPSHOT_DIGEST.as_slice());

        let child = RecipeCommit::new(
            "00000000-0000-7000-8000-000000000103"
                .parse::<RecipeCommitId>()
                .expect("fixed child commit id"),
            root.recipe_id(),
            vec![root.id()],
            RecipeSnapshot::empty(),
            Some("child of raw v9 commit".into()),
            1_721_500_000_200,
        )
        .expect("valid child commit");
        catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: child.clone(),
                update_refs: Vec::new(),
            })
            .expect("append child to raw v9 commit");

        let after = read_raw_root(&catalog);
        assert_eq!(after, before, "appending a child rewrote its v9 parent row");
        assert_eq!(
            catalog
                .recipe_commit(photo_id, child.id())
                .expect("read appended child")
                .expect("appended child exists")
                .commit
                .parents(),
            &[root.id()]
        );
        drop(catalog);
        std::fs::remove_dir_all(root_dir).expect("remove v9 fixture root");
    }

    fn catalog_with_photo(path: &str) -> (Catalog, PhotoId) {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
                byte_len: 1,
                modified_at_ms: Some(1),
                now_ms: 1,
            })
            .expect("register photo");
        (catalog, registered.photo_id)
    }

    fn commit(
        recipe_id: RecipeId,
        parents: Vec<RecipeCommitId>,
        message: &str,
        created_at_ms: i64,
    ) -> RecipeCommit {
        RecipeCommit::new(
            RecipeCommitId::new_v7(),
            recipe_id,
            parents,
            RecipeSnapshot::empty(),
            Some(message.into()),
            created_at_ms,
        )
        .expect("valid commit")
    }
}
