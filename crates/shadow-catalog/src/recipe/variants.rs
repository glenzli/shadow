//! Stable per-photo edit Variants and their active Recipe projection.

use rusqlite::{OptionalExtension, Transaction, params, types::Type};
use shadow_domain::{EntityId, PhotoId, PhotoVariantId, RecipeCommitId};
use uuid::Uuid;

use super::{RecipeRefKind, upsert_ref};
use crate::{Catalog, CatalogError, row_codec::read_id};

const WORKING_RECIPE_REF: &str = "working";
const MAX_VARIANT_NAME_BYTES: usize = 256;

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct PhotoVariantRecord {
    pub id: PhotoVariantId,
    pub photo_id: PhotoId,
    /// Empty only for the built-in original Variant so presentation can be localized.
    pub name: String,
    pub head_commit_id: Option<RecipeCommitId>,
    pub is_default: bool,
    pub is_active: bool,
    pub created_at_ms: i64,
    pub updated_at_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CreatePhotoVariant {
    pub id: PhotoVariantId,
    pub photo_id: PhotoId,
    pub name: String,
    /// Clones an immutable Recipe head without copying source bytes.
    pub source_commit_id: Option<RecipeCommitId>,
    pub activate: bool,
    pub created_at_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RenamePhotoVariant {
    pub id: PhotoVariantId,
    pub photo_id: PhotoId,
    pub name: String,
    pub updated_at_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ActivatePhotoVariant {
    pub id: PhotoVariantId,
    pub photo_id: PhotoId,
    pub updated_at_ms: i64,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RemovePhotoVariant {
    pub id: PhotoVariantId,
    pub photo_id: PhotoId,
}

impl Catalog {
    pub fn photo_variants(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<PhotoVariantRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT v.id, v.photo_id, v.name, v.head_commit_id, v.is_default,
                    v.id = s.active_variant_id, v.created_at_ms, v.updated_at_ms
             FROM photo_variants v
             JOIN photo_variant_state s ON s.photo_id = v.photo_id
             WHERE v.photo_id = ?1
             ORDER BY v.is_default DESC, v.created_at_ms, v.id",
        )?;
        let rows = statement.query_map([photo_id.as_bytes().as_slice()], |row| {
            Ok(PhotoVariantRecord {
                id: read_id(row, 0)?,
                photo_id: read_id(row, 1)?,
                name: row.get(2)?,
                head_commit_id: optional_id(row, 3)?,
                is_default: row.get(4)?,
                is_active: row.get(5)?,
                created_at_ms: row.get(6)?,
                updated_at_ms: row.get(7)?,
            })
        })?;
        let records = rows.collect::<Result<Vec<_>, _>>()?;
        if records.is_empty() {
            return Err(CatalogError::PhotoNotFound(photo_id));
        }
        Ok(records)
    }

    pub fn create_photo_variant(
        &mut self,
        request: &CreatePhotoVariant,
    ) -> Result<PhotoVariantRecord, CatalogError> {
        validate_name(&request.name)?;
        let transaction = self.connection.transaction()?;
        ensure_photo(&transaction, request.photo_id)?;
        if let Some(commit_id) = request.source_commit_id {
            ensure_commit_owner(&transaction, request.photo_id, commit_id)?;
        }
        transaction.execute(
            "INSERT INTO photo_variants(
                 id, photo_id, name, head_commit_id, is_default, created_at_ms, updated_at_ms
             ) VALUES (?1, ?2, ?3, ?4, 0, ?5, ?5)",
            params![
                request.id.as_bytes().as_slice(),
                request.photo_id.as_bytes().as_slice(),
                request.name,
                request.source_commit_id.map(|id| id.as_bytes().to_vec()),
                request.created_at_ms,
            ],
        )?;
        if request.activate {
            activate_variant(
                &transaction,
                request.photo_id,
                request.id,
                request.created_at_ms,
            )?;
        }
        transaction.commit()?;
        self.photo_variants(request.photo_id)?
            .into_iter()
            .find(|variant| variant.id == request.id)
            .ok_or(CatalogError::PhotoVariantNotFound(request.id))
    }

    pub fn rename_photo_variant(
        &mut self,
        request: &RenamePhotoVariant,
    ) -> Result<(), CatalogError> {
        validate_name(&request.name)?;
        let changed = self.connection.execute(
            "UPDATE photo_variants
             SET name = ?3, updated_at_ms = ?4
             WHERE id = ?1 AND photo_id = ?2",
            params![
                request.id.as_bytes().as_slice(),
                request.photo_id.as_bytes().as_slice(),
                request.name,
                request.updated_at_ms,
            ],
        )?;
        if changed == 1 {
            Ok(())
        } else {
            Err(CatalogError::PhotoVariantNotFound(request.id))
        }
    }

    pub fn activate_photo_variant(
        &mut self,
        request: &ActivatePhotoVariant,
    ) -> Result<(), CatalogError> {
        let transaction = self.connection.transaction()?;
        activate_variant(
            &transaction,
            request.photo_id,
            request.id,
            request.updated_at_ms,
        )?;
        transaction.commit()?;
        Ok(())
    }

    pub fn remove_photo_variant(
        &mut self,
        request: &RemovePhotoVariant,
    ) -> Result<(), CatalogError> {
        let transaction = self.connection.transaction()?;
        let state = transaction
            .query_row(
                "SELECT v.is_default, v.id = s.active_variant_id
                 FROM photo_variants v
                 JOIN photo_variant_state s ON s.photo_id = v.photo_id
                 WHERE v.id = ?1 AND v.photo_id = ?2",
                params![
                    request.id.as_bytes().as_slice(),
                    request.photo_id.as_bytes().as_slice(),
                ],
                |row| Ok((row.get::<_, bool>(0)?, row.get::<_, bool>(1)?)),
            )
            .optional()?
            .ok_or(CatalogError::PhotoVariantNotFound(request.id))?;
        if state.0 {
            return Err(CatalogError::CannotRemoveDefaultPhotoVariant);
        }
        if state.1 {
            return Err(CatalogError::CannotRemoveActivePhotoVariant);
        }
        transaction.execute(
            "DELETE FROM photo_variants WHERE id = ?1 AND photo_id = ?2",
            params![
                request.id.as_bytes().as_slice(),
                request.photo_id.as_bytes().as_slice(),
            ],
        )?;
        transaction.commit()?;
        Ok(())
    }
}

pub(super) fn update_active_variant_head(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
    commit_id: RecipeCommitId,
    updated_at_ms: i64,
) -> Result<(), CatalogError> {
    let changed = transaction.execute(
        "UPDATE photo_variants
         SET head_commit_id = ?2, updated_at_ms = ?3
         WHERE photo_id = ?1 AND id = (
             SELECT active_variant_id FROM photo_variant_state WHERE photo_id = ?1
         )",
        params![
            photo_id.as_bytes().as_slice(),
            commit_id.as_bytes().as_slice(),
            updated_at_ms,
        ],
    )?;
    if changed == 1 {
        Ok(())
    } else {
        Err(CatalogError::InvalidPhotoVariant(
            "photo has no active Variant".into(),
        ))
    }
}

pub(crate) fn ensure_active_variant(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
    expected: PhotoVariantId,
) -> Result<(), CatalogError> {
    let actual = transaction
        .query_row(
            "SELECT active_variant_id FROM photo_variant_state WHERE photo_id = ?1",
            [photo_id.as_bytes().as_slice()],
            |row| read_id::<PhotoVariantId>(row, 0),
        )
        .optional()?
        .ok_or(CatalogError::PhotoNotFound(photo_id))?;
    if actual == expected {
        Ok(())
    } else {
        Err(CatalogError::PhotoVariantExpectationMismatch {
            photo_id,
            expected,
            actual,
        })
    }
}

fn activate_variant(
    transaction: &Transaction<'_>,
    photo_id: PhotoId,
    variant_id: PhotoVariantId,
    updated_at_ms: i64,
) -> Result<(), CatalogError> {
    let head = transaction
        .query_row(
            "SELECT head_commit_id FROM photo_variants WHERE id = ?1 AND photo_id = ?2",
            params![
                variant_id.as_bytes().as_slice(),
                photo_id.as_bytes().as_slice(),
            ],
            |row| optional_id::<RecipeCommitId>(row, 0),
        )
        .optional()?
        .ok_or(CatalogError::PhotoVariantNotFound(variant_id))?;
    transaction.execute(
        "UPDATE photo_variant_state
         SET active_variant_id = ?2, updated_at_ms = ?3
         WHERE photo_id = ?1",
        params![
            photo_id.as_bytes().as_slice(),
            variant_id.as_bytes().as_slice(),
            updated_at_ms,
        ],
    )?;
    if let Some(commit_id) = head {
        upsert_ref(
            transaction,
            photo_id,
            WORKING_RECIPE_REF,
            RecipeRefKind::Working,
            commit_id,
            updated_at_ms,
        )?;
    } else {
        transaction.execute(
            "DELETE FROM recipe_refs WHERE photo_id = ?1 AND name = ?2",
            params![photo_id.as_bytes().as_slice(), WORKING_RECIPE_REF],
        )?;
    }
    Ok(())
}

fn validate_name(name: &str) -> Result<(), CatalogError> {
    if name.is_empty()
        || name.trim() != name
        || name.len() > MAX_VARIANT_NAME_BYTES
        || name.chars().any(|character| character == '\0')
    {
        return Err(CatalogError::InvalidPhotoVariant(
            "name must be trimmed, non-empty, and at most 256 UTF-8 bytes".into(),
        ));
    }
    Ok(())
}

fn ensure_photo(transaction: &Transaction<'_>, photo_id: PhotoId) -> Result<(), CatalogError> {
    let present = transaction.query_row(
        "SELECT EXISTS(SELECT 1 FROM photos WHERE id = ?1)",
        [photo_id.as_bytes().as_slice()],
        |row| row.get::<_, bool>(0),
    )?;
    present
        .then_some(())
        .ok_or(CatalogError::PhotoNotFound(photo_id))
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

fn optional_id<I: EntityId>(row: &rusqlite::Row<'_>, index: usize) -> rusqlite::Result<Option<I>> {
    let bytes: Option<Vec<u8>> = row.get(index)?;
    bytes
        .map(|bytes| {
            Uuid::from_slice(&bytes).map(I::from_uuid).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(index, Type::Blob, Box::new(error))
            })
        })
        .transpose()
}
