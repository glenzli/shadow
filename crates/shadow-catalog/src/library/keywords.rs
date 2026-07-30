//! Hierarchical keyword taxonomy, committed photo assignments, and bulk mutation.
//!
//! Keywords are user-owned durable organization. Rebuildable model suggestions stay outside this
//! table until a user accepts them; accepted suggestions retain their provenance.

use std::collections::BTreeSet;

use rusqlite::{OptionalExtension, params};
use shadow_domain::{EntityId, KeywordId, PhotoId};

use crate::{Catalog, CatalogError, row_codec::read_id};

use super::{
    LibraryKeywordAssignmentOrigin, LibraryKeywordDeletionReceipt, LibraryKeywordMutationReceipt,
    LibraryKeywordRecord, LibraryPhotoKeyword, MAX_LIBRARY_KEYWORD_MUTATION_PHOTOS,
};

const MAX_LIBRARY_KEYWORD_DEPTH: u16 = 32;

impl Catalog {
    /// Creates one keyword beneath an optional parent.
    pub fn create_library_keyword(
        &mut self,
        parent_id: Option<KeywordId>,
        name: &str,
        now_ms: i64,
    ) -> Result<LibraryKeywordRecord, CatalogError> {
        let (name, normalized_name) = validate_keyword_name(name)?;
        if let Some(parent_id) = parent_id {
            let parent_depth = keyword_depth(&self.connection, parent_id)?;
            if parent_depth + 1 >= MAX_LIBRARY_KEYWORD_DEPTH {
                return Err(CatalogError::InvalidLibraryKeyword(format!(
                    "keyword hierarchy must not exceed {MAX_LIBRARY_KEYWORD_DEPTH} levels"
                )));
            }
        }
        ensure_sibling_name_available(&self.connection, parent_id, &normalized_name, None)?;
        let id = KeywordId::new_v7();
        self.connection.execute(
            "INSERT INTO library_keywords(
                 id, parent_id, name, normalized_name, created_at_ms, updated_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?5)",
            params![
                id.as_bytes().as_slice(),
                parent_id.map(|value| value.as_bytes().to_vec()),
                name,
                normalized_name,
                now_ms,
            ],
        )?;
        self.library_keyword(id)
    }

    /// Renames a keyword without changing its descendants or assignments.
    pub fn rename_library_keyword(
        &mut self,
        keyword_id: KeywordId,
        name: &str,
        now_ms: i64,
    ) -> Result<LibraryKeywordRecord, CatalogError> {
        let existing = self.library_keyword(keyword_id)?;
        let (name, normalized_name) = validate_keyword_name(name)?;
        ensure_sibling_name_available(
            &self.connection,
            existing.parent_id,
            &normalized_name,
            Some(keyword_id),
        )?;
        self.connection.execute(
            "UPDATE library_keywords
             SET name = ?2, normalized_name = ?3, updated_at_ms = ?4
             WHERE id = ?1",
            params![
                keyword_id.as_bytes().as_slice(),
                name,
                normalized_name,
                now_ms,
            ],
        )?;
        self.library_keyword(keyword_id)
    }

    /// Moves a complete keyword subtree beneath a new parent.
    pub fn move_library_keyword(
        &mut self,
        keyword_id: KeywordId,
        parent_id: Option<KeywordId>,
        now_ms: i64,
    ) -> Result<LibraryKeywordRecord, CatalogError> {
        let existing = self.library_keyword(keyword_id)?;
        if parent_id == Some(keyword_id) {
            return Err(CatalogError::InvalidLibraryKeyword(
                "a keyword cannot be its own parent".into(),
            ));
        }
        if let Some(parent_id) = parent_id {
            keyword_depth(&self.connection, parent_id)?;
            let parent_is_descendant: bool = self.connection.query_row(
                "WITH RECURSIVE descendants(id) AS (
                     SELECT id FROM library_keywords WHERE parent_id = ?1
                     UNION ALL
                     SELECT child.id
                     FROM library_keywords child
                     JOIN descendants parent ON child.parent_id = parent.id
                 )
                 SELECT EXISTS(SELECT 1 FROM descendants WHERE id = ?2)",
                params![
                    keyword_id.as_bytes().as_slice(),
                    parent_id.as_bytes().as_slice(),
                ],
                |row| row.get(0),
            )?;
            if parent_is_descendant {
                return Err(CatalogError::InvalidLibraryKeyword(
                    "a keyword cannot move beneath its own descendant".into(),
                ));
            }
            let parent_depth = keyword_depth(&self.connection, parent_id)?;
            let subtree_depth = keyword_subtree_depth(&self.connection, keyword_id)?;
            if parent_depth + 1 + subtree_depth >= MAX_LIBRARY_KEYWORD_DEPTH {
                return Err(CatalogError::InvalidLibraryKeyword(format!(
                    "keyword hierarchy must not exceed {MAX_LIBRARY_KEYWORD_DEPTH} levels"
                )));
            }
        }
        ensure_sibling_name_available(
            &self.connection,
            parent_id,
            &normalize_keyword_name(&existing.name),
            Some(keyword_id),
        )?;
        self.connection.execute(
            "UPDATE library_keywords SET parent_id = ?2, updated_at_ms = ?3 WHERE id = ?1",
            params![
                keyword_id.as_bytes().as_slice(),
                parent_id.map(|value| value.as_bytes().to_vec()),
                now_ms,
            ],
        )?;
        self.library_keyword(keyword_id)
    }

    /// Deletes one explicit subtree and all of its photo assignments.
    ///
    /// The destructive scope is returned so the caller can present and verify
    /// the exact result. No photo or album is deleted.
    pub fn delete_library_keyword_subtree(
        &mut self,
        keyword_id: KeywordId,
    ) -> Result<LibraryKeywordDeletionReceipt, CatalogError> {
        self.library_keyword(keyword_id)?;
        let (keyword_count, assignment_count): (i64, i64) = self.connection.query_row(
            "WITH RECURSIVE descendants(id) AS (
                 SELECT id FROM library_keywords WHERE id = ?1
                 UNION ALL
                 SELECT child.id
                 FROM library_keywords child
                 JOIN descendants parent ON child.parent_id = parent.id
             )
             SELECT COUNT(*),
                    (SELECT COUNT(*) FROM library_photo_keywords assignment
                     WHERE assignment.keyword_id IN (SELECT id FROM descendants))
             FROM descendants",
            [keyword_id.as_bytes().as_slice()],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )?;
        self.connection.execute(
            "DELETE FROM library_keywords WHERE id = ?1",
            [keyword_id.as_bytes().as_slice()],
        )?;
        Ok(LibraryKeywordDeletionReceipt {
            deleted_keyword_count: count_to_u64(keyword_count, "keyword")?,
            deleted_assignment_count: count_to_u64(assignment_count, "keyword assignment")?,
        })
    }

    /// Returns the complete taxonomy in stable parent-before-child order.
    pub fn library_keyword_tree(&self) -> Result<Vec<LibraryKeywordRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "WITH RECURSIVE
             tree(id, parent_id, name, normalized_name, depth, sort_path, created_at_ms, updated_at_ms) AS (
                 SELECT id, parent_id, name, normalized_name, 0,
                        normalized_name || ':' || hex(id), created_at_ms, updated_at_ms
                 FROM library_keywords
                 WHERE parent_id IS NULL
                 UNION ALL
                 SELECT child.id, child.parent_id, child.name, child.normalized_name,
                        parent.depth + 1,
                        parent.sort_path || '/' || child.normalized_name || ':' || hex(child.id),
                        child.created_at_ms, child.updated_at_ms
                 FROM library_keywords child
                 JOIN tree parent ON child.parent_id = parent.id
             ),
             descendants(root_id, keyword_id) AS (
                 SELECT id, id FROM library_keywords
                 UNION ALL
                 SELECT parent.root_id, child.id
                 FROM descendants parent
                 JOIN library_keywords child ON child.parent_id = parent.keyword_id
             )
             SELECT tree.id, tree.parent_id, tree.name, tree.depth,
                    COUNT(DISTINCT assignment.photo_id),
                    tree.created_at_ms, tree.updated_at_ms
             FROM tree
             LEFT JOIN descendants ON descendants.root_id = tree.id
             LEFT JOIN library_photo_keywords assignment
                    ON assignment.keyword_id = descendants.keyword_id
             GROUP BY tree.id, tree.parent_id, tree.name, tree.depth,
                      tree.sort_path, tree.created_at_ms, tree.updated_at_ms
             ORDER BY tree.sort_path",
        )?;
        let rows = statement.query_map([], read_keyword_record)?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Returns the committed direct keyword assignments for one photo.
    pub fn library_keywords_for_photo(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<LibraryPhotoKeyword>, CatalogError> {
        super::integrity::ensure_photo_exists_connection(&self.connection, photo_id)?;
        let mut statement = self.connection.prepare(
            "WITH RECURSIVE
             tree(id, parent_id, name, depth, sort_path, created_at_ms, updated_at_ms) AS (
                 SELECT id, parent_id, name, 0,
                        normalized_name || ':' || hex(id), created_at_ms, updated_at_ms
                 FROM library_keywords
                 WHERE parent_id IS NULL
                 UNION ALL
                 SELECT child.id, child.parent_id, child.name, parent.depth + 1,
                        parent.sort_path || '/' || child.normalized_name || ':' || hex(child.id),
                        child.created_at_ms, child.updated_at_ms
                 FROM library_keywords child
                 JOIN tree parent ON child.parent_id = parent.id
             ),
             descendants(root_id, keyword_id) AS (
                 SELECT id, id FROM library_keywords
                 UNION ALL
                 SELECT parent.root_id, child.id
                 FROM descendants parent
                 JOIN library_keywords child ON child.parent_id = parent.keyword_id
             ),
             counts(keyword_id, photo_count) AS (
                 SELECT descendants.root_id, COUNT(DISTINCT assignment.photo_id)
                 FROM descendants
                 LEFT JOIN library_photo_keywords assignment
                        ON assignment.keyword_id = descendants.keyword_id
                 GROUP BY descendants.root_id
             )
             SELECT tree.id, tree.parent_id, tree.name, tree.depth,
                    COALESCE(counts.photo_count, 0),
                    tree.created_at_ms, tree.updated_at_ms,
                    assignment.origin, assignment.source_label,
                    assignment.confidence_milli, assignment.assigned_at_ms
             FROM library_photo_keywords assignment
             JOIN tree ON tree.id = assignment.keyword_id
             LEFT JOIN counts ON counts.keyword_id = tree.id
             WHERE assignment.photo_id = ?1
             ORDER BY tree.sort_path",
        )?;
        let rows = statement.query_map([photo_id.as_bytes().as_slice()], |row| {
            let origin = LibraryKeywordAssignmentOrigin::parse(&row.get::<_, String>(7)?)
                .map_err(|error| invalid_keyword_row(7, error.to_string()))?;
            let confidence_milli: Option<i64> = row.get(9)?;
            Ok(LibraryPhotoKeyword {
                keyword: read_keyword_record(row)?,
                origin,
                source_label: row.get(8)?,
                confidence_milli: confidence_milli
                    .map(|value| {
                        u16::try_from(value)
                            .map_err(|error| invalid_keyword_row(9, error.to_string()))
                    })
                    .transpose()?,
                assigned_at_ms: row.get(10)?,
            })
        })?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Assigns one keyword to a bounded, deduplicated photo batch atomically.
    pub fn assign_library_keyword_to_photos(
        &mut self,
        keyword_id: KeywordId,
        photo_ids: &[PhotoId],
        origin: LibraryKeywordAssignmentOrigin,
        source_label: &str,
        confidence_milli: Option<u16>,
        now_ms: i64,
    ) -> Result<LibraryKeywordMutationReceipt, CatalogError> {
        let photo_ids =
            validate_keyword_mutation(photo_ids, origin, source_label, confidence_milli)?;
        let transaction = self.connection.transaction()?;
        ensure_keyword_exists(&transaction, keyword_id)?;
        let mut changed = 0_u64;
        for photo_id in &photo_ids {
            super::integrity::ensure_photo_exists(&transaction, *photo_id)?;
            changed += u64::try_from(transaction.execute(
                "INSERT INTO library_photo_keywords(
                     keyword_id, photo_id, origin, source_label, confidence_milli, assigned_at_ms
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)
                 ON CONFLICT(keyword_id, photo_id) DO UPDATE SET
                     origin = excluded.origin,
                     source_label = excluded.source_label,
                     confidence_milli = excluded.confidence_milli,
                     assigned_at_ms = excluded.assigned_at_ms
                 WHERE library_photo_keywords.origin <> excluded.origin
                    OR library_photo_keywords.source_label <> excluded.source_label
                    OR library_photo_keywords.confidence_milli IS NOT excluded.confidence_milli",
                params![
                    keyword_id.as_bytes().as_slice(),
                    photo_id.as_bytes().as_slice(),
                    origin.as_str(),
                    source_label.trim(),
                    confidence_milli.map(i64::from),
                    now_ms,
                ],
            )?)
            .map_err(|error| CatalogError::InvalidLibraryKeyword(error.to_string()))?;
        }
        transaction.commit()?;
        Ok(LibraryKeywordMutationReceipt {
            keyword_id,
            requested_photo_count: photo_ids.len() as u64,
            changed_photo_count: changed,
        })
    }

    /// Removes one keyword from a bounded, deduplicated photo batch atomically.
    pub fn remove_library_keyword_from_photos(
        &mut self,
        keyword_id: KeywordId,
        photo_ids: &[PhotoId],
    ) -> Result<LibraryKeywordMutationReceipt, CatalogError> {
        let photo_ids = normalize_photo_ids(photo_ids)?;
        let transaction = self.connection.transaction()?;
        ensure_keyword_exists(&transaction, keyword_id)?;
        let mut changed = 0_u64;
        for photo_id in &photo_ids {
            super::integrity::ensure_photo_exists(&transaction, *photo_id)?;
            changed += u64::try_from(transaction.execute(
                "DELETE FROM library_photo_keywords WHERE keyword_id = ?1 AND photo_id = ?2",
                params![
                    keyword_id.as_bytes().as_slice(),
                    photo_id.as_bytes().as_slice()
                ],
            )?)
            .map_err(|error| CatalogError::InvalidLibraryKeyword(error.to_string()))?;
        }
        transaction.commit()?;
        Ok(LibraryKeywordMutationReceipt {
            keyword_id,
            requested_photo_count: photo_ids.len() as u64,
            changed_photo_count: changed,
        })
    }

    fn library_keyword(&self, keyword_id: KeywordId) -> Result<LibraryKeywordRecord, CatalogError> {
        let records = self.library_keyword_tree()?;
        records
            .into_iter()
            .find(|record| record.id == keyword_id)
            .ok_or(CatalogError::LibraryKeywordNotFound(keyword_id))
    }
}

fn validate_keyword_name(name: &str) -> Result<(String, String), CatalogError> {
    let name = name.trim();
    let normalized = normalize_keyword_name(name);
    if name.is_empty() || name.len() > 256 || normalized.is_empty() || normalized.len() > 256 {
        return Err(CatalogError::InvalidLibraryKeyword(
            "keyword names must contain 1 through 256 bytes".into(),
        ));
    }
    if name.chars().any(char::is_control) {
        return Err(CatalogError::InvalidLibraryKeyword(
            "keyword names must not contain control characters".into(),
        ));
    }
    Ok((name.to_owned(), normalized))
}

fn normalize_keyword_name(name: &str) -> String {
    name.trim().to_lowercase()
}

fn ensure_sibling_name_available(
    connection: &rusqlite::Connection,
    parent_id: Option<KeywordId>,
    normalized_name: &str,
    excluding: Option<KeywordId>,
) -> Result<(), CatalogError> {
    let existing: Option<KeywordId> = connection
        .query_row(
            "SELECT id FROM library_keywords
             WHERE parent_id IS ?1 AND normalized_name = ?2
             LIMIT 1",
            params![
                parent_id.map(|value| value.as_bytes().to_vec()),
                normalized_name
            ],
            |row| read_id(row, 0),
        )
        .optional()?;
    if existing.is_some_and(|id| Some(id) != excluding) {
        return Err(CatalogError::InvalidLibraryKeyword(
            "a sibling keyword already uses that name".into(),
        ));
    }
    Ok(())
}

fn ensure_keyword_exists(
    connection: &rusqlite::Connection,
    keyword_id: KeywordId,
) -> Result<(), CatalogError> {
    let exists: bool = connection.query_row(
        "SELECT EXISTS(SELECT 1 FROM library_keywords WHERE id = ?1)",
        [keyword_id.as_bytes().as_slice()],
        |row| row.get(0),
    )?;
    if exists {
        Ok(())
    } else {
        Err(CatalogError::LibraryKeywordNotFound(keyword_id))
    }
}

fn keyword_depth(
    connection: &rusqlite::Connection,
    keyword_id: KeywordId,
) -> Result<u16, CatalogError> {
    let depth: Option<i64> = connection
        .query_row(
            "WITH RECURSIVE ancestors(id, parent_id, depth) AS (
                 SELECT id, parent_id, 0 FROM library_keywords WHERE id = ?1
                 UNION ALL
                 SELECT parent.id, parent.parent_id, child.depth + 1
                 FROM library_keywords parent
                 JOIN ancestors child ON child.parent_id = parent.id
             )
             SELECT MAX(depth) FROM ancestors",
            [keyword_id.as_bytes().as_slice()],
            |row| row.get(0),
        )
        .optional()?
        .flatten();
    depth
        .map(|value| {
            u16::try_from(value)
                .map_err(|error| CatalogError::InvalidLibraryKeyword(error.to_string()))
        })
        .transpose()?
        .ok_or(CatalogError::LibraryKeywordNotFound(keyword_id))
}

fn keyword_subtree_depth(
    connection: &rusqlite::Connection,
    keyword_id: KeywordId,
) -> Result<u16, CatalogError> {
    ensure_keyword_exists(connection, keyword_id)?;
    let depth: i64 = connection.query_row(
        "WITH RECURSIVE descendants(id, depth) AS (
             SELECT id, 0 FROM library_keywords WHERE id = ?1
             UNION ALL
             SELECT child.id, parent.depth + 1
             FROM library_keywords child
             JOIN descendants parent ON child.parent_id = parent.id
         )
         SELECT MAX(depth) FROM descendants",
        [keyword_id.as_bytes().as_slice()],
        |row| row.get(0),
    )?;
    u16::try_from(depth).map_err(|error| CatalogError::InvalidLibraryKeyword(error.to_string()))
}

fn validate_keyword_mutation(
    photo_ids: &[PhotoId],
    origin: LibraryKeywordAssignmentOrigin,
    source_label: &str,
    confidence_milli: Option<u16>,
) -> Result<Vec<PhotoId>, CatalogError> {
    if source_label.trim().len() > 512 {
        return Err(CatalogError::InvalidLibraryKeyword(
            "keyword assignment source labels must not exceed 512 bytes".into(),
        ));
    }
    if confidence_milli.is_some_and(|value| value > 1_000) {
        return Err(CatalogError::InvalidLibraryKeyword(
            "keyword assignment confidence must be between 0 and 1000".into(),
        ));
    }
    match origin {
        LibraryKeywordAssignmentOrigin::Manual if confidence_milli.is_some() => {
            return Err(CatalogError::InvalidLibraryKeyword(
                "manual keyword assignments must not claim model confidence".into(),
            ));
        }
        LibraryKeywordAssignmentOrigin::AiAccepted if source_label.trim().is_empty() => {
            return Err(CatalogError::InvalidLibraryKeyword(
                "accepted AI keyword assignments require a model source label".into(),
            ));
        }
        _ => {}
    }
    normalize_photo_ids(photo_ids)
}

fn normalize_photo_ids(photo_ids: &[PhotoId]) -> Result<Vec<PhotoId>, CatalogError> {
    if photo_ids.is_empty() || photo_ids.len() > MAX_LIBRARY_KEYWORD_MUTATION_PHOTOS {
        return Err(CatalogError::InvalidLibraryKeyword(format!(
            "keyword mutations require 1 through {MAX_LIBRARY_KEYWORD_MUTATION_PHOTOS} photos"
        )));
    }
    Ok(photo_ids
        .iter()
        .copied()
        .collect::<BTreeSet<_>>()
        .into_iter()
        .collect())
}

fn read_keyword_record(row: &rusqlite::Row<'_>) -> rusqlite::Result<LibraryKeywordRecord> {
    let depth: i64 = row.get(3)?;
    let photo_count: i64 = row.get(4)?;
    Ok(LibraryKeywordRecord {
        id: read_id(row, 0)?,
        parent_id: row
            .get::<_, Option<Vec<u8>>>(1)?
            .map(|bytes| {
                let uuid = uuid::Uuid::from_slice(&bytes)
                    .map_err(|error| invalid_keyword_row(1, error.to_string()))?;
                Ok::<KeywordId, rusqlite::Error>(KeywordId::from_uuid(uuid))
            })
            .transpose()?,
        name: row.get(2)?,
        depth: u16::try_from(depth).map_err(|error| invalid_keyword_row(3, error.to_string()))?,
        subtree_photo_count: count_to_u64_sql(photo_count, 4)?,
        created_at_ms: row.get(5)?,
        updated_at_ms: row.get(6)?,
    })
}

fn count_to_u64(value: i64, kind: &str) -> Result<u64, CatalogError> {
    u64::try_from(value)
        .map_err(|error| CatalogError::InvalidLibraryKeyword(format!("{kind} count: {error}")))
}

fn count_to_u64_sql(value: i64, column: usize) -> rusqlite::Result<u64> {
    u64::try_from(value).map_err(|error| invalid_keyword_row(column, error.to_string()))
}

fn invalid_keyword_row(column: usize, message: String) -> rusqlite::Error {
    rusqlite::Error::FromSqlConversionFailure(
        column,
        rusqlite::types::Type::Text,
        Box::new(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            message,
        )),
    )
}
