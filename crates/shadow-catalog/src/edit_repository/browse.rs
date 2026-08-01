//! Stable, bounded browsing of the Library-wide immutable edit repository.

use rusqlite::params;
use shadow_domain::{EditCommitId, EditRepositoryRefKind};

use crate::{Catalog, CatalogError};

use super::{
    EditRepositoryCommitRecord, EditRepositoryRefRecord, read_commit_id, validate_ref_name,
};

pub const MAX_EDIT_REPOSITORY_HISTORY_PAGE_SIZE: usize = 256;
pub const MAX_EDIT_REPOSITORY_REF_PAGE_SIZE: usize = 256;

/// Exclusive keyset cursor for newest-first Library commit traversal.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct EditRepositoryHistoryCursor {
    pub created_at_ms: i64,
    pub commit_id: EditCommitId,
}

/// One global commit decorated with the durable refs that currently point to it.
#[derive(Debug, Clone, PartialEq)]
pub struct EditRepositoryHistoryEntry {
    pub record: EditRepositoryCommitRecord,
    pub refs: Vec<EditRepositoryRefRecord>,
}

/// One bounded newest-first page of Library-wide edit commits.
#[derive(Debug, Clone, PartialEq)]
pub struct EditRepositoryHistoryPage {
    pub entries: Vec<EditRepositoryHistoryEntry>,
    pub next_cursor: Option<EditRepositoryHistoryCursor>,
}

/// One bounded alphabetical page of branch, named-version, and tag refs.
#[derive(Debug, Clone, PartialEq)]
pub struct EditRepositoryRefPage {
    pub refs: Vec<EditRepositoryRefRecord>,
    pub next_cursor: Option<String>,
}

impl Catalog {
    /// Returns one stable newest-first page of Library-wide immutable commits.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid limit or malformed persisted history.
    pub fn edit_repository_history_page(
        &self,
        after: Option<&EditRepositoryHistoryCursor>,
        limit: usize,
    ) -> Result<EditRepositoryHistoryPage, CatalogError> {
        validate_history_limit(limit)?;
        let query_limit = i64::try_from(limit + 1).map_err(|error| {
            CatalogError::InvalidEditRepositoryCommit(format!(
                "edit repository history page limit overflow: {error}"
            ))
        })?;
        let cursor_time = after.map(|cursor| cursor.created_at_ms);
        let cursor_id = after.map(|cursor| cursor.commit_id.as_bytes().as_slice());
        let mut statement = self.connection.prepare(
            "SELECT id
             FROM edit_repository_commits
             WHERE ?1 IS NULL
                OR created_at_ms < ?1
                OR (created_at_ms = ?1 AND id < ?2)
             ORDER BY created_at_ms DESC, id DESC
             LIMIT ?3",
        )?;
        let ids = statement.query_map(params![cursor_time, cursor_id, query_limit], |row| {
            read_commit_id(row, 0)
        })?;
        let mut records = Vec::with_capacity(limit + 1);
        for id in ids {
            let id = id?;
            let record = self
                .edit_repository_commit(id)?
                .ok_or(CatalogError::EditRepositoryCommitNotFound(id))?;
            records.push(record);
        }
        let has_more = records.len() > limit;
        if has_more {
            records.pop();
        }
        let next_cursor = if has_more {
            records.last().map(|record| EditRepositoryHistoryCursor {
                created_at_ms: record.commit.payload().created_at_ms,
                commit_id: record.commit.id(),
            })
        } else {
            None
        };
        let entries = records
            .into_iter()
            .map(|record| {
                Ok(EditRepositoryHistoryEntry {
                    refs: edit_repository_refs_for_commit(self, record.commit.id())?,
                    record,
                })
            })
            .collect::<Result<Vec<_>, CatalogError>>()?;
        Ok(EditRepositoryHistoryPage {
            entries,
            next_cursor,
        })
    }

    /// Returns one stable alphabetical page of Library-wide refs.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid cursor, limit, or persisted ref.
    pub fn edit_repository_ref_page(
        &self,
        after_name: Option<&str>,
        limit: usize,
    ) -> Result<EditRepositoryRefPage, CatalogError> {
        if !(1..=MAX_EDIT_REPOSITORY_REF_PAGE_SIZE).contains(&limit) {
            return Err(CatalogError::InvalidEditRepositoryRefName(format!(
                "edit repository ref page limit {limit} is outside 1 through {MAX_EDIT_REPOSITORY_REF_PAGE_SIZE}"
            )));
        }
        if let Some(name) = after_name {
            validate_ref_name(name)?;
        }
        let query_limit = i64::try_from(limit + 1).map_err(|error| {
            CatalogError::InvalidEditRepositoryRefName(format!(
                "edit repository ref page limit overflow: {error}"
            ))
        })?;
        let mut statement = self.connection.prepare(
            "SELECT name, kind, commit_id, updated_at_ms
             FROM edit_repository_refs
             WHERE ?1 IS NULL OR name > ?1
             ORDER BY name
             LIMIT ?2",
        )?;
        let rows = statement.query_map(params![after_name, query_limit], decode_ref_row)?;
        let mut refs = rows.collect::<Result<Vec<_>, _>>()?;
        let has_more = refs.len() > limit;
        if has_more {
            refs.pop();
        }
        let next_cursor = has_more
            .then(|| refs.last().map(|reference| reference.name.clone()))
            .flatten();
        Ok(EditRepositoryRefPage { refs, next_cursor })
    }
}

fn validate_history_limit(limit: usize) -> Result<(), CatalogError> {
    if (1..=MAX_EDIT_REPOSITORY_HISTORY_PAGE_SIZE).contains(&limit) {
        Ok(())
    } else {
        Err(CatalogError::InvalidEditRepositoryCommit(format!(
            "edit repository history page limit {limit} is outside 1 through {MAX_EDIT_REPOSITORY_HISTORY_PAGE_SIZE}"
        )))
    }
}

fn edit_repository_refs_for_commit(
    catalog: &Catalog,
    commit_id: EditCommitId,
) -> Result<Vec<EditRepositoryRefRecord>, CatalogError> {
    let mut statement = catalog.connection.prepare(
        "SELECT name, kind, commit_id, updated_at_ms
         FROM edit_repository_refs
         WHERE commit_id = ?1
         ORDER BY name",
    )?;
    statement
        .query_map([commit_id.as_bytes().as_slice()], decode_ref_row)?
        .collect::<Result<Vec<_>, _>>()
        .map_err(Into::into)
}

fn decode_ref_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<EditRepositoryRefRecord> {
    let kind_text: String = row.get(1)?;
    let kind = kind_text.parse::<EditRepositoryRefKind>().map_err(|_| {
        rusqlite::Error::FromSqlConversionFailure(
            1,
            rusqlite::types::Type::Text,
            Box::new(PersistedRefKindError(kind_text)),
        )
    })?;
    Ok(EditRepositoryRefRecord {
        name: row.get(0)?,
        kind,
        commit_id: read_commit_id(row, 2)?,
        updated_at_ms: row.get(3)?,
    })
}

#[derive(Debug)]
struct PersistedRefKindError(String);

impl std::fmt::Display for PersistedRefKindError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "unknown edit repository ref kind {}", self.0)
    }
}

impl std::error::Error for PersistedRefKindError {}
