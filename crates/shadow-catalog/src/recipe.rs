#![allow(clippy::missing_errors_doc)]

use rusqlite::{OptionalExtension, Transaction, TransactionBehavior, params};
use shadow_domain::{
    EntityId, PhotoId, RecipeCommit, RecipeCommitId, RecipeId, canonical_recipe_snapshot_digest,
};

use crate::{Catalog, CatalogError, cache_artifact::digest, row_codec::read_id};

mod history_browse;
mod variants;
pub use history_browse::{
    MAX_RECIPE_HISTORY_PAGE_SIZE, RecipeHistoryCursor, RecipeHistoryEntry, RecipeHistoryPage,
};
pub(crate) use variants::ensure_active_variant;
pub use variants::{
    ActivatePhotoVariant, CreatePhotoVariant, PhotoVariantRecord, RemovePhotoVariant,
    RenamePhotoVariant,
};

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

    /// Commits only while the photographer is still editing the expected Variant.
    pub fn commit_recipe_for_variant(
        &mut self,
        request: &CommitRecipe,
        expected_variant_id: shadow_domain::PhotoVariantId,
    ) -> Result<RecipeCommitRecord, CatalogError> {
        // Reserve the writer before reading Variant/head expectations. With a
        // deferred transaction, two sessions can both read the old head, then
        // one fails to upgrade its WAL snapshot with SQLITE_BUSY instead of
        // observing the winner and returning the semantic CAS conflict.
        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        variants::ensure_active_variant(&transaction, request.photo_id, expected_variant_id)?;
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
        if request.kind == RecipeRefKind::Working && request.name == "working" {
            variants::update_active_variant_head(
                &transaction,
                request.photo_id,
                request.commit_id,
                request.updated_at_ms,
            )?;
        }
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
            "UPDATE photo_variants
             SET head_commit_id = NULL, updated_at_ms = MAX(updated_at_ms, ?2)
             WHERE photo_id = ?1",
            params![photo_id.as_bytes().as_slice(), 0_i64],
        )?;
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
        if target.kind == RecipeRefKind::Working && target.name == "working" {
            variants::update_active_variant_head(
                transaction,
                request.photo_id,
                request.commit.id(),
                request.commit.created_at_ms(),
            )?;
        }
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
mod tests;
