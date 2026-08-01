use std::collections::BTreeSet;

use rusqlite::{Connection, OptionalExtension, Transaction, params, types::Type};
use shadow_domain::{
    EditCommitId, EditObject, EditObjectEdge, EditObjectId, EditObjectKind, EditObjectPack,
    EditRepositoryCommit, EditRepositoryError, EditRepositoryRefExpectation, EditRepositoryRefKind,
};

use crate::{
    Catalog, CatalogError, CommitRecipe, RecipeCommitRecord, recipe::commit_recipe_in_transaction,
};

mod browse;
pub use browse::{
    EditRepositoryHistoryCursor, EditRepositoryHistoryEntry, EditRepositoryHistoryPage,
    EditRepositoryRefPage, MAX_EDIT_REPOSITORY_HISTORY_PAGE_SIZE,
    MAX_EDIT_REPOSITORY_REF_PAGE_SIZE,
};

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditObjectRecord {
    pub object: EditObject,
    pub edges: Vec<EditObjectEdge>,
    pub created_at_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditObjectPackWrite {
    pub objects: Vec<EditObjectPack>,
    pub created_at_ms: i64,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Default)]
pub struct StoreEditObjectPackResult {
    pub inserted: usize,
    pub reused: usize,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditRepositoryRefUpdate {
    pub name: String,
    pub kind: EditRepositoryRefKind,
    pub expected: EditRepositoryRefExpectation,
    pub updated_at_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CommitEditRepository {
    pub commit: EditRepositoryCommit,
    pub update_refs: Vec<EditRepositoryRefUpdate>,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditRepositoryCommitRecord {
    pub commit: EditRepositoryCommit,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditRepositoryRefRecord {
    pub name: String,
    pub kind: EditRepositoryRefKind,
    pub commit_id: EditCommitId,
    pub updated_at_ms: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct CommitRecipeAndEditRepository {
    pub recipe: CommitRecipe,
    pub repository: CommitEditRepository,
}

#[derive(Debug, Clone, PartialEq)]
pub struct CommitRecipeAndEditRepositoryResult {
    pub recipe: RecipeCommitRecord,
    pub repository: EditRepositoryCommitRecord,
}

impl Catalog {
    /// Atomically stores one topologically unordered pack of immutable edit
    /// objects. Every edge target must already exist or appear in this pack.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an empty/invalid pack, missing targets,
    /// content collisions, or a failed `SQLite` transaction.
    pub fn store_edit_object_pack(
        &mut self,
        request: &EditObjectPackWrite,
    ) -> Result<StoreEditObjectPackResult, CatalogError> {
        if request.objects.is_empty() {
            return Err(CatalogError::InvalidEditObject(
                "an edit object pack cannot be empty".into(),
            ));
        }
        let mut object_ids = BTreeSet::new();
        for pack in &request.objects {
            if !object_ids.insert(pack.object().id()) {
                return Err(CatalogError::InvalidEditObject(format!(
                    "object {} appears more than once in one pack",
                    pack.object().id()
                )));
            }
        }

        let transaction = self.connection.transaction()?;
        let mut result = StoreEditObjectPackResult::default();
        let mut inserted = BTreeSet::new();

        for pack in &request.objects {
            let object = pack.object();
            if let Some(existing) = raw_object(&transaction, object.id())? {
                if existing.kind != object.kind()
                    || existing.format_version != object.format_version()
                    || existing.payload.as_bytes() != object.canonical_json()
                {
                    return Err(CatalogError::EditObjectCollision(object.id()));
                }
                result.reused += 1;
            } else {
                let payload = canonical_json_text(object.canonical_json())?;
                transaction.execute(
                    "INSERT INTO edit_objects(
                         digest, hash_algorithm, kind, format_version,
                         payload_codec, payload, created_at_ms
                     ) VALUES (?1, 'blake3-256', ?2, ?3, 'canonical-json', ?4, ?5)",
                    params![
                        object.id().as_bytes().as_slice(),
                        object.kind().as_str(),
                        i64::from(object.format_version()),
                        payload,
                        request.created_at_ms,
                    ],
                )?;
                inserted.insert(object.id());
                result.inserted += 1;
            }
        }

        for pack in &request.objects {
            for edge in pack.edges() {
                if !object_exists(&transaction, edge.target())? {
                    return Err(CatalogError::EditObjectNotFound(edge.target()));
                }
                if pack.object().kind() == EditObjectKind::LibraryRoot {
                    let target = raw_object(&transaction, edge.target())?
                        .ok_or(CatalogError::EditObjectNotFound(edge.target()))?;
                    if target.kind != EditObjectKind::EntityMap {
                        return Err(CatalogError::InvalidEditObject(format!(
                            "Library root edge {:?} must target an EntityMap, not {:?}",
                            edge.role(),
                            target.kind
                        )));
                    }
                }
            }
            if inserted.contains(&pack.object().id()) {
                insert_edges(&transaction, pack.object().id(), pack.edges())?;
            } else if stored_edges(&transaction, pack.object().id())? != pack.edges() {
                return Err(CatalogError::EditObjectCollision(pack.object().id()));
            }
        }

        transaction.commit()?;
        Ok(result)
    }

    /// Reads and integrity-checks one immutable edit object and its edge index.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when `SQLite` fails or persisted content, its
    /// digest, or its typed edge index is inconsistent.
    pub fn edit_object(&self, id: EditObjectId) -> Result<Option<EditObjectRecord>, CatalogError> {
        checked_object(&self.connection, id)
    }

    /// Atomically writes one content-addressed global commit and advances zero
    /// or more Library refs using mandatory compare-and-swap expectations.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for invalid roots/parents, stale refs, content
    /// collisions, or a failed `SQLite` transaction.
    pub fn commit_edit_repository(
        &mut self,
        request: &CommitEditRepository,
    ) -> Result<EditRepositoryCommitRecord, CatalogError> {
        let transaction = self.connection.transaction()?;
        let record = commit_edit_repository_in_transaction(&transaction, request)?;
        transaction.commit()?;
        Ok(record)
    }

    /// Atomically publishes the legacy per-photo compatibility commit and the
    /// corresponding Library-wide commit/ref movement.
    ///
    /// Immutable edit objects may be stored before this call; if CAS fails,
    /// they remain unreachable and safe for later garbage collection, while
    /// neither commit nor either family of refs advances.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when either commit is invalid, either expected
    /// ref is stale, an owner/root/parent is missing, or `SQLite` fails.
    pub fn commit_recipe_and_edit_repository(
        &mut self,
        request: &CommitRecipeAndEditRepository,
    ) -> Result<CommitRecipeAndEditRepositoryResult, CatalogError> {
        let transaction = self.connection.transaction()?;
        let recipe = commit_recipe_in_transaction(&transaction, &request.recipe)?;
        let repository = commit_edit_repository_in_transaction(&transaction, &request.repository)?;
        transaction.commit()?;
        Ok(CommitRecipeAndEditRepositoryResult { recipe, repository })
    }

    /// Reads and integrity-checks a global edit repository commit.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when `SQLite` fails or persisted canonical JSON,
    /// indexed fields, parents, or the content digest disagree.
    pub fn edit_repository_commit(
        &self,
        id: EditCommitId,
    ) -> Result<Option<EditRepositoryCommitRecord>, CatalogError> {
        let Some(raw) = raw_commit(&self.connection, id)? else {
            return Ok(None);
        };
        let commit = EditRepositoryCommit::from_stored_parts(id, raw.canonical_json.into_bytes())
            .map_err(|error| invalid_commit(&error))?;
        if raw.format_version != 1
            || raw.root != commit.payload().root
            || raw.created_at_ms != commit.payload().created_at_ms
            || stored_parents(&self.connection, id)? != commit.payload().parents
        {
            return Err(CatalogError::InvalidEditRepositoryCommit(format!(
                "indexed fields disagree with commit {id}"
            )));
        }
        Ok(Some(EditRepositoryCommitRecord { commit }))
    }

    /// Resolves one Library-level ref without crossing into legacy photo refs.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid name, unavailable `SQLite` data,
    /// or a malformed persisted kind or commit identifier.
    pub fn edit_repository_ref(
        &self,
        name: &str,
    ) -> Result<Option<EditRepositoryRefRecord>, CatalogError> {
        validate_ref_name(name)?;
        self.connection
            .query_row(
                "SELECT kind, commit_id, updated_at_ms
                 FROM edit_repository_refs WHERE name = ?1",
                [name],
                |row| {
                    let kind: String = row.get(0)?;
                    let kind = kind.parse().map_err(|_| {
                        rusqlite::Error::FromSqlConversionFailure(
                            0,
                            Type::Text,
                            Box::new(PersistedRefKindError(kind.clone())),
                        )
                    })?;
                    Ok(EditRepositoryRefRecord {
                        name: name.to_owned(),
                        kind,
                        commit_id: read_commit_id(row, 1)?,
                        updated_at_ms: row.get(2)?,
                    })
                },
            )
            .optional()
            .map_err(Into::into)
    }
}

fn commit_edit_repository_in_transaction(
    transaction: &Transaction<'_>,
    request: &CommitEditRepository,
) -> Result<EditRepositoryCommitRecord, CatalogError> {
    validate_ref_updates(&request.update_refs)?;
    for update in &request.update_refs {
        let actual = current_ref_target(transaction, &update.name)?;
        let matches = match update.expected {
            EditRepositoryRefExpectation::Missing => actual.is_none(),
            EditRepositoryRefExpectation::At(expected) => actual == Some(expected),
        };
        if !matches {
            return Err(CatalogError::EditRepositoryRefExpectationMismatch {
                name: update.name.clone(),
                expected: update.expected,
                actual,
            });
        }
    }

    let commit = &request.commit;
    let root = checked_object(transaction, commit.payload().root)?
        .ok_or(CatalogError::EditObjectNotFound(commit.payload().root))?;
    if root.object.kind() != EditObjectKind::LibraryRoot {
        return Err(CatalogError::InvalidEditRepositoryCommit(format!(
            "commit root {} is {:?}, expected LibraryRoot",
            commit.payload().root,
            root.object.kind()
        )));
    }
    for parent in &commit.payload().parents {
        if !commit_exists(transaction, *parent)? {
            return Err(CatalogError::EditRepositoryCommitNotFound(*parent));
        }
    }

    if let Some(existing) = raw_commit(transaction, commit.id())? {
        if existing.root != commit.payload().root
            || existing.format_version != 1
            || existing.canonical_json.as_bytes() != commit.canonical_json()
            || existing.created_at_ms != commit.payload().created_at_ms
            || stored_parents(transaction, commit.id())? != commit.payload().parents
        {
            return Err(CatalogError::EditRepositoryCommitCollision(commit.id()));
        }
    } else {
        let commit_json = canonical_json_text(commit.canonical_json())?;
        transaction.execute(
            "INSERT INTO edit_repository_commits(
                 id, root_digest, format_version, commit_json, created_at_ms
             ) VALUES (?1, ?2, 1, ?3, ?4)",
            params![
                commit.id().as_bytes().as_slice(),
                commit.payload().root.as_bytes().as_slice(),
                commit_json,
                commit.payload().created_at_ms,
            ],
        )?;
        for (position, parent) in commit.payload().parents.iter().enumerate() {
            transaction.execute(
                "INSERT INTO edit_repository_commit_parents(
                     commit_id, parent_id, position
                 ) VALUES (?1, ?2, ?3)",
                params![
                    commit.id().as_bytes().as_slice(),
                    parent.as_bytes().as_slice(),
                    index_i64(position)?,
                ],
            )?;
        }
    }

    for update in &request.update_refs {
        transaction.execute(
            "INSERT INTO edit_repository_refs(name, kind, commit_id, updated_at_ms)
             VALUES (?1, ?2, ?3, ?4)
             ON CONFLICT(name) DO UPDATE SET
                 kind = excluded.kind,
                 commit_id = excluded.commit_id,
                 updated_at_ms = excluded.updated_at_ms",
            params![
                update.name,
                update.kind.as_str(),
                commit.id().as_bytes().as_slice(),
                update.updated_at_ms,
            ],
        )?;
    }
    Ok(EditRepositoryCommitRecord {
        commit: commit.clone(),
    })
}

#[derive(Debug)]
struct RawObject {
    kind: EditObjectKind,
    format_version: u32,
    payload: String,
    created_at_ms: i64,
}

fn checked_object(
    connection: &Connection,
    id: EditObjectId,
) -> Result<Option<EditObjectRecord>, CatalogError> {
    let Some(raw) = raw_object(connection, id)? else {
        return Ok(None);
    };
    let object =
        EditObject::from_stored_parts(id, raw.kind, raw.format_version, raw.payload.into_bytes())
            .map_err(|error| invalid_object(&error))?;
    let edges = stored_edges(connection, id)?;
    EditObjectPack::new(object.clone(), edges.clone()).map_err(|error| invalid_object(&error))?;
    Ok(Some(EditObjectRecord {
        object,
        edges,
        created_at_ms: raw.created_at_ms,
    }))
}

fn raw_object(
    connection: &Connection,
    id: EditObjectId,
) -> Result<Option<RawObject>, CatalogError> {
    connection
        .query_row(
            "SELECT kind, format_version, payload, created_at_ms
             FROM edit_objects WHERE digest = ?1",
            [id.as_bytes().as_slice()],
            |row| {
                let kind_text: String = row.get(0)?;
                let kind = kind_text.parse().map_err(|error: EditRepositoryError| {
                    rusqlite::Error::FromSqlConversionFailure(0, Type::Text, Box::new(error))
                })?;
                let format_version: i64 = row.get(1)?;
                Ok(RawObject {
                    kind,
                    format_version: u32::try_from(format_version).map_err(|error| {
                        rusqlite::Error::FromSqlConversionFailure(1, Type::Integer, Box::new(error))
                    })?,
                    payload: row.get(2)?,
                    created_at_ms: row.get(3)?,
                })
            },
        )
        .optional()
        .map_err(Into::into)
}

fn object_exists(connection: &Connection, id: EditObjectId) -> rusqlite::Result<bool> {
    connection.query_row(
        "SELECT EXISTS(SELECT 1 FROM edit_objects WHERE digest = ?1)",
        [id.as_bytes().as_slice()],
        |row| row.get(0),
    )
}

fn insert_edges(
    transaction: &Transaction<'_>,
    source: EditObjectId,
    edges: &[EditObjectEdge],
) -> Result<(), CatalogError> {
    for edge in edges {
        transaction.execute(
            "INSERT INTO edit_object_edges(source_digest, role, position, target_digest)
             VALUES (?1, ?2, ?3, ?4)",
            params![
                source.as_bytes().as_slice(),
                edge.role(),
                i64::from(edge.position()),
                edge.target().as_bytes().as_slice(),
            ],
        )?;
    }
    Ok(())
}

fn stored_edges(
    connection: &Connection,
    source: EditObjectId,
) -> Result<Vec<EditObjectEdge>, CatalogError> {
    let mut statement = connection.prepare(
        "SELECT role, position, target_digest
         FROM edit_object_edges
         WHERE source_digest = ?1
         ORDER BY role, position",
    )?;
    let rows = statement.query_map([source.as_bytes().as_slice()], |row| {
        let role: String = row.get(0)?;
        let position: i64 = row.get(1)?;
        let position = u32::try_from(position).map_err(|error| {
            rusqlite::Error::FromSqlConversionFailure(1, Type::Integer, Box::new(error))
        })?;
        EditObjectEdge::new(role, position, read_object_id(row, 2)?).map_err(|error| {
            rusqlite::Error::FromSqlConversionFailure(0, Type::Text, Box::new(error))
        })
    })?;
    rows.collect::<Result<Vec<_>, _>>().map_err(Into::into)
}

#[derive(Debug)]
struct RawCommit {
    root: EditObjectId,
    format_version: u32,
    canonical_json: String,
    created_at_ms: i64,
}

fn raw_commit(
    connection: &Connection,
    id: EditCommitId,
) -> Result<Option<RawCommit>, CatalogError> {
    connection
        .query_row(
            "SELECT root_digest, format_version, commit_json, created_at_ms
             FROM edit_repository_commits WHERE id = ?1",
            [id.as_bytes().as_slice()],
            |row| {
                let format_version: i64 = row.get(1)?;
                Ok(RawCommit {
                    root: read_object_id(row, 0)?,
                    format_version: u32::try_from(format_version).map_err(|error| {
                        rusqlite::Error::FromSqlConversionFailure(1, Type::Integer, Box::new(error))
                    })?,
                    canonical_json: row.get(2)?,
                    created_at_ms: row.get(3)?,
                })
            },
        )
        .optional()
        .map_err(Into::into)
}

fn commit_exists(connection: &Connection, id: EditCommitId) -> rusqlite::Result<bool> {
    connection.query_row(
        "SELECT EXISTS(SELECT 1 FROM edit_repository_commits WHERE id = ?1)",
        [id.as_bytes().as_slice()],
        |row| row.get(0),
    )
}

fn stored_parents(
    connection: &Connection,
    commit_id: EditCommitId,
) -> Result<Vec<EditCommitId>, CatalogError> {
    let mut statement = connection.prepare(
        "SELECT parent_id FROM edit_repository_commit_parents
         WHERE commit_id = ?1 ORDER BY position",
    )?;
    let rows = statement.query_map([commit_id.as_bytes().as_slice()], |row| {
        read_commit_id(row, 0)
    })?;
    rows.collect::<Result<Vec<_>, _>>().map_err(Into::into)
}

fn current_ref_target(
    connection: &Connection,
    name: &str,
) -> Result<Option<EditCommitId>, CatalogError> {
    connection
        .query_row(
            "SELECT commit_id FROM edit_repository_refs WHERE name = ?1",
            [name],
            |row| read_commit_id(row, 0),
        )
        .optional()
        .map_err(Into::into)
}

fn validate_ref_updates(updates: &[EditRepositoryRefUpdate]) -> Result<(), CatalogError> {
    let mut names = BTreeSet::new();
    for update in updates {
        validate_ref_name(&update.name)?;
        if !names.insert(&update.name) {
            return Err(CatalogError::DuplicateEditRepositoryRefName(
                update.name.clone(),
            ));
        }
    }
    Ok(())
}

fn validate_ref_name(name: &str) -> Result<(), CatalogError> {
    if name.is_empty()
        || name.len() > 256
        || name.starts_with('/')
        || name.ends_with('/')
        || name.contains("//")
        || !name
            .bytes()
            .all(|value| value.is_ascii_alphanumeric() || b"/._-".contains(&value))
    {
        return Err(CatalogError::InvalidEditRepositoryRefName(name.to_owned()));
    }
    Ok(())
}

fn canonical_json_text(bytes: &[u8]) -> Result<&str, CatalogError> {
    std::str::from_utf8(bytes).map_err(|error| CatalogError::InvalidEditObject(error.to_string()))
}

fn index_i64(index: usize) -> Result<i64, CatalogError> {
    i64::try_from(index)
        .map_err(|error| CatalogError::InvalidEditRepositoryCommit(error.to_string()))
}

fn read_object_id(row: &rusqlite::Row<'_>, index: usize) -> rusqlite::Result<EditObjectId> {
    let value: Vec<u8> = row.get(index)?;
    let bytes: [u8; 32] = value.try_into().map_err(|value: Vec<u8>| {
        rusqlite::Error::FromSqlConversionFailure(
            index,
            Type::Blob,
            format!("expected 32-byte edit object id, got {}", value.len()).into(),
        )
    })?;
    Ok(EditObjectId::from_bytes(bytes))
}

fn read_commit_id(row: &rusqlite::Row<'_>, index: usize) -> rusqlite::Result<EditCommitId> {
    let value: Vec<u8> = row.get(index)?;
    let bytes: [u8; 32] = value.try_into().map_err(|value: Vec<u8>| {
        rusqlite::Error::FromSqlConversionFailure(
            index,
            Type::Blob,
            format!("expected 32-byte edit commit id, got {}", value.len()).into(),
        )
    })?;
    Ok(EditCommitId::from_bytes(bytes))
}

fn invalid_object(error: &EditRepositoryError) -> CatalogError {
    CatalogError::InvalidEditObject(error.to_string())
}

fn invalid_commit(error: &EditRepositoryError) -> CatalogError {
    CatalogError::InvalidEditRepositoryCommit(error.to_string())
}

#[derive(Debug)]
struct PersistedRefKindError(String);

impl std::fmt::Display for PersistedRefKindError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            formatter,
            "unknown persisted edit repository ref kind: {}",
            self.0
        )
    }
}

impl std::error::Error for PersistedRefKindError {}

#[cfg(test)]
mod tests;
