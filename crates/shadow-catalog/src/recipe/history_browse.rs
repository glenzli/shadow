//! Stable, bounded browsing of one photo's immutable Recipe history.

use rusqlite::params;
use shadow_domain::{EntityId, PhotoId, RecipeCommitId, RecipeId};

use crate::{Catalog, CatalogError, cache_artifact::digest, row_codec::read_id};

use super::{RecipeCommitRecord, RecipeRefRecord, decode_recipe_record, parse_ref_kind};

pub const MAX_RECIPE_HISTORY_PAGE_SIZE: usize = 256;

/// Exclusive keyset cursor for newest-first Recipe history traversal.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct RecipeHistoryCursor {
    pub created_at_ms: i64,
    pub commit_id: RecipeCommitId,
}

/// One immutable Recipe commit decorated with every durable ref that points to it.
#[derive(Debug, Clone, PartialEq)]
pub struct RecipeHistoryEntry {
    pub record: RecipeCommitRecord,
    pub refs: Vec<RecipeRefRecord>,
}

/// One bounded newest-first page of a photo's durable edit timeline.
#[derive(Debug, Clone, PartialEq)]
pub struct RecipeHistoryPage {
    pub entries: Vec<RecipeHistoryEntry>,
    pub next_cursor: Option<RecipeHistoryCursor>,
}

impl Catalog {
    /// Returns a stable newest-first page of immutable Recipe commits for one photo.
    ///
    /// Refs are projected only for commits in this page, keeping the result bounded
    /// while still identifying the working head, named versions, branches, and tags.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid page limit or malformed persisted history.
    pub fn recipe_history_page(
        &self,
        photo_id: PhotoId,
        after: Option<&RecipeHistoryCursor>,
        limit: usize,
    ) -> Result<RecipeHistoryPage, CatalogError> {
        if !(1..=MAX_RECIPE_HISTORY_PAGE_SIZE).contains(&limit) {
            return Err(CatalogError::InvalidRecipe(format!(
                "Recipe history page limit {limit} is outside 1 through {MAX_RECIPE_HISTORY_PAGE_SIZE}"
            )));
        }
        let query_limit = i64::try_from(limit + 1).map_err(|error| {
            CatalogError::InvalidRecipe(format!("Recipe history page limit overflow: {error}"))
        })?;
        let cursor_time = after.map(|cursor| cursor.created_at_ms);
        let cursor_id = after.map(|cursor| cursor.commit_id.as_bytes().as_slice());
        let mut statement = self.connection.prepare(
            "SELECT id, recipe_id, commit_json, snapshot_digest
             FROM recipe_commits
             WHERE photo_id = ?1
               AND (
                    ?2 IS NULL
                    OR created_at_ms < ?2
                    OR (created_at_ms = ?2 AND id < ?3)
               )
             ORDER BY created_at_ms DESC, id DESC
             LIMIT ?4",
        )?;
        let rows = statement.query_map(
            params![
                photo_id.as_bytes().as_slice(),
                cursor_time,
                cursor_id,
                query_limit,
            ],
            |row| {
                Ok((
                    read_id::<RecipeCommitId>(row, 0)?,
                    read_id::<RecipeId>(row, 1)?,
                    row.get::<_, String>(2)?,
                    digest(row.get(3)?, 3)?,
                ))
            },
        )?;
        let mut records = Vec::with_capacity(limit + 1);
        for row in rows {
            let (stored_id, stored_recipe_id, json, snapshot_digest) = row?;
            records.push(decode_recipe_record(
                self,
                photo_id,
                stored_id,
                stored_recipe_id,
                &json,
                snapshot_digest,
            )?);
        }
        let has_more = records.len() > limit;
        if has_more {
            records.pop();
        }
        let next_cursor = if has_more {
            records.last().map(|record| RecipeHistoryCursor {
                created_at_ms: record.commit.created_at_ms(),
                commit_id: record.commit.id(),
            })
        } else {
            None
        };
        let entries = records
            .into_iter()
            .map(|record| {
                Ok(RecipeHistoryEntry {
                    refs: recipe_refs_for_commit(self, photo_id, record.commit.id())?,
                    record,
                })
            })
            .collect::<Result<Vec<_>, CatalogError>>()?;
        Ok(RecipeHistoryPage {
            entries,
            next_cursor,
        })
    }
}

fn recipe_refs_for_commit(
    catalog: &Catalog,
    photo_id: PhotoId,
    commit_id: RecipeCommitId,
) -> Result<Vec<RecipeRefRecord>, CatalogError> {
    let mut statement = catalog.connection.prepare(
        "SELECT name, kind, updated_at_ms
         FROM recipe_refs
         WHERE photo_id = ?1 AND commit_id = ?2
         ORDER BY name",
    )?;
    let rows = statement.query_map(
        params![
            photo_id.as_bytes().as_slice(),
            commit_id.as_bytes().as_slice()
        ],
        |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, i64>(2)?,
            ))
        },
    )?;
    rows.map(|row| {
        let (name, kind, updated_at_ms) = row?;
        Ok(RecipeRefRecord {
            photo_id,
            name,
            kind: parse_ref_kind(&kind)?,
            commit_id,
            updated_at_ms,
        })
    })
    .collect()
}
