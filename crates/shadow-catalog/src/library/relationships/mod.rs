//! Durable relationships between distinct photos.
//!
//! This is deliberately separate from the foundational
//! `Photo -> Representation -> Location` graph and from photo Variants. A group relates several
//! independently addressable photos without merging their identities, sources, or edit histories.

use std::collections::BTreeSet;

use rusqlite::{OptionalExtension, params};
use shadow_domain::{EntityId, GroupId, PhotoId};

use crate::{Catalog, CatalogError, row_codec::read_id};

use super::integrity::ensure_photo_exists;

/// Keeps one relationship mutation bounded for large burst or similarity sets.
pub const MAX_PHOTO_GROUP_MEMBERS: usize = 4_096;

/// The photographic reason several distinct photos are related.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum PhotoGroupKind {
    Burst,
    Bracket,
    Panorama,
    Similar,
}

impl PhotoGroupKind {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Burst => "burst",
            Self::Bracket => "bracket",
            Self::Panorama => "panorama",
            Self::Similar => "similar",
        }
    }

    fn from_persisted(value: &str) -> Result<Self, CatalogError> {
        match value {
            "burst" => Ok(Self::Burst),
            "bracket" => Ok(Self::Bracket),
            "panorama" => Ok(Self::Panorama),
            "similar" => Ok(Self::Similar),
            _ => Err(CatalogError::InvalidPhotoGroup(format!(
                "unknown persisted photo group kind {value:?}"
            ))),
        }
    }
}

/// Provenance of a distinct-photo relationship.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum PhotoGroupOrigin {
    User,
    CameraMetadata,
    VisualSimilarity,
}

impl PhotoGroupOrigin {
    const fn as_str(self) -> &'static str {
        match self {
            Self::User => "user",
            Self::CameraMetadata => "camera_metadata",
            Self::VisualSimilarity => "visual_similarity",
        }
    }

    fn from_persisted(value: &str) -> Result<Self, CatalogError> {
        match value {
            "user" => Ok(Self::User),
            "camera_metadata" => Ok(Self::CameraMetadata),
            "visual_similarity" => Ok(Self::VisualSimilarity),
            _ => Err(CatalogError::InvalidPhotoGroup(format!(
                "unknown persisted photo group origin {value:?}"
            ))),
        }
    }
}

/// One ordered member of a group. The anchor is the presentation and editing reference, not an
/// owner: every member remains an independent photo.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct PhotoGroupMember {
    pub photo_id: PhotoId,
    pub position: u32,
    pub is_anchor: bool,
}

/// A durable relationship among distinct photos.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct PhotoGroupRecord {
    pub id: GroupId,
    pub kind: PhotoGroupKind,
    pub origin: PhotoGroupOrigin,
    pub members: Vec<PhotoGroupMember>,
    pub created_at_ms: i64,
    pub updated_at_ms: i64,
}

/// Complete input for a newly detected or user-authored photo group.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CreatePhotoGroup {
    pub kind: PhotoGroupKind,
    pub origin: PhotoGroupOrigin,
    pub ordered_photo_ids: Vec<PhotoId>,
    pub anchor_photo_id: PhotoId,
    pub now_ms: i64,
}

impl Catalog {
    /// Creates a group of at least two distinct photos in one transaction.
    pub fn create_photo_group(
        &mut self,
        request: &CreatePhotoGroup,
    ) -> Result<PhotoGroupRecord, CatalogError> {
        validate_members(&request.ordered_photo_ids, request.anchor_photo_id)?;
        let id = GroupId::new_v7();
        let transaction = self.connection.transaction()?;
        for photo_id in &request.ordered_photo_ids {
            ensure_photo_exists(&transaction, *photo_id)?;
        }
        transaction.execute(
            "INSERT INTO photo_groups(id, kind, origin, created_at_ms, updated_at_ms)
             VALUES (?1, ?2, ?3, ?4, ?4)",
            params![
                id.as_bytes().as_slice(),
                request.kind.as_str(),
                request.origin.as_str(),
                request.now_ms,
            ],
        )?;
        insert_members(
            &transaction,
            id,
            &request.ordered_photo_ids,
            request.anchor_photo_id,
        )?;
        transaction.commit()?;
        self.photo_group(id)
    }

    /// Atomically replaces ordering and anchor while preserving group identity and provenance.
    pub fn replace_photo_group_members(
        &mut self,
        group_id: GroupId,
        ordered_photo_ids: &[PhotoId],
        anchor_photo_id: PhotoId,
        now_ms: i64,
    ) -> Result<PhotoGroupRecord, CatalogError> {
        validate_members(ordered_photo_ids, anchor_photo_id)?;
        let transaction = self.connection.transaction()?;
        ensure_group_exists(&transaction, group_id)?;
        for photo_id in ordered_photo_ids {
            ensure_photo_exists(&transaction, *photo_id)?;
        }
        transaction.execute(
            "DELETE FROM photo_group_members WHERE group_id = ?1",
            [group_id.as_bytes().as_slice()],
        )?;
        insert_members(&transaction, group_id, ordered_photo_ids, anchor_photo_id)?;
        transaction.execute(
            "UPDATE photo_groups SET updated_at_ms = ?2 WHERE id = ?1",
            params![group_id.as_bytes().as_slice(), now_ms],
        )?;
        transaction.commit()?;
        self.photo_group(group_id)
    }

    /// Reads one complete group in its stable photographic order.
    pub fn photo_group(&self, group_id: GroupId) -> Result<PhotoGroupRecord, CatalogError> {
        let header = self
            .connection
            .query_row(
                "SELECT kind, origin, created_at_ms, updated_at_ms
                 FROM photo_groups WHERE id = ?1",
                [group_id.as_bytes().as_slice()],
                |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        row.get::<_, String>(1)?,
                        row.get::<_, i64>(2)?,
                        row.get::<_, i64>(3)?,
                    ))
                },
            )
            .optional()?
            .ok_or(CatalogError::PhotoGroupNotFound(group_id))?;
        Ok(PhotoGroupRecord {
            id: group_id,
            kind: PhotoGroupKind::from_persisted(&header.0)?,
            origin: PhotoGroupOrigin::from_persisted(&header.1)?,
            members: read_members(&self.connection, group_id)?,
            created_at_ms: header.2,
            updated_at_ms: header.3,
        })
    }

    /// Lists every group containing one photo, newest relationship first.
    pub fn photo_groups_for_photo(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<PhotoGroupRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT g.id
             FROM photo_groups g
             JOIN photo_group_members m ON m.group_id = g.id
             WHERE m.photo_id = ?1
             ORDER BY g.updated_at_ms DESC, g.id",
        )?;
        let ids = statement
            .query_map([photo_id.as_bytes().as_slice()], |row| read_id(row, 0))?
            .collect::<rusqlite::Result<Vec<GroupId>>>()?;
        ids.into_iter().map(|id| self.photo_group(id)).collect()
    }

    /// Deletes only the relationship; member photos and their representations remain untouched.
    pub fn delete_photo_group(&mut self, group_id: GroupId) -> Result<bool, CatalogError> {
        Ok(self.connection.execute(
            "DELETE FROM photo_groups WHERE id = ?1",
            [group_id.as_bytes().as_slice()],
        )? != 0)
    }
}

fn validate_members(
    ordered_photo_ids: &[PhotoId],
    anchor_photo_id: PhotoId,
) -> Result<(), CatalogError> {
    if !(2..=MAX_PHOTO_GROUP_MEMBERS).contains(&ordered_photo_ids.len()) {
        return Err(CatalogError::InvalidPhotoGroup(format!(
            "a photo group must contain 2 through {MAX_PHOTO_GROUP_MEMBERS} photos"
        )));
    }
    let unique = ordered_photo_ids.iter().copied().collect::<BTreeSet<_>>();
    if unique.len() != ordered_photo_ids.len() {
        return Err(CatalogError::InvalidPhotoGroup(
            "a photo may appear only once in one group".into(),
        ));
    }
    if !unique.contains(&anchor_photo_id) {
        return Err(CatalogError::InvalidPhotoGroup(
            "the anchor must be one of the grouped photos".into(),
        ));
    }
    Ok(())
}

fn ensure_group_exists(
    transaction: &rusqlite::Transaction<'_>,
    group_id: GroupId,
) -> Result<(), CatalogError> {
    let exists = transaction
        .query_row(
            "SELECT 1 FROM photo_groups WHERE id = ?1",
            [group_id.as_bytes().as_slice()],
            |row| row.get::<_, i64>(0),
        )
        .optional()?;
    exists
        .map(|_| ())
        .ok_or(CatalogError::PhotoGroupNotFound(group_id))
}

fn insert_members(
    transaction: &rusqlite::Transaction<'_>,
    group_id: GroupId,
    ordered_photo_ids: &[PhotoId],
    anchor_photo_id: PhotoId,
) -> Result<(), CatalogError> {
    let mut statement = transaction.prepare(
        "INSERT INTO photo_group_members(group_id, photo_id, position, is_anchor)
         VALUES (?1, ?2, ?3, ?4)",
    )?;
    for (position, photo_id) in ordered_photo_ids.iter().enumerate() {
        statement.execute(params![
            group_id.as_bytes().as_slice(),
            photo_id.as_bytes().as_slice(),
            i64::try_from(position).map_err(|_| CatalogError::InvalidPhotoGroup(
                "photo group position exceeds SQLite integer range".into()
            ))?,
            i64::from(*photo_id == anchor_photo_id),
        ])?;
    }
    Ok(())
}

fn read_members(
    connection: &rusqlite::Connection,
    group_id: GroupId,
) -> Result<Vec<PhotoGroupMember>, CatalogError> {
    let mut statement = connection.prepare(
        "SELECT photo_id, position, is_anchor
         FROM photo_group_members WHERE group_id = ?1 ORDER BY position",
    )?;
    statement
        .query_map([group_id.as_bytes().as_slice()], |row| {
            let position = row.get::<_, u32>(1)?;
            Ok(PhotoGroupMember {
                photo_id: read_id(row, 0)?,
                position,
                is_anchor: row.get::<_, i64>(2)? != 0,
            })
        })?
        .collect::<rusqlite::Result<Vec<_>>>()
        .map_err(Into::into)
}

#[cfg(test)]
mod tests;
