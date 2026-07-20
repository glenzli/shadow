use rusqlite::OptionalExtension;
use shadow_domain::{AssetLocation, EntityId, PhotoId, Platform, RepresentationId};

use crate::{
    CachedArtifact, CachedArtifactRecord, Catalog, CatalogError, RepresentationFingerprint,
    cache_artifact::{
        digest, non_negative_u16, non_negative_u32, non_negative_u64, optional_usize,
        parse_byte_order, parse_codec, parse_role,
    },
    read_id,
};

/// One immutable row for the Review grid.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ReviewItemRecord {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub location: AssetLocation,
    pub source: RepresentationFingerprint,
    pub visual: Option<CachedArtifactRecord>,
}

/// Stable keyset cursor for the Review grid's path/id ordering.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ReviewCursor {
    pub display_path: String,
    pub representation_id: RepresentationId,
}

/// One bounded Review-grid page and the cursor needed to continue it.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ReviewPageRecord {
    pub items: Vec<ReviewItemRecord>,
    pub next_cursor: Option<ReviewCursor>,
    pub total_items: u64,
}

const MAX_REVIEW_PAGE_SIZE: usize = 512;

#[derive(Debug)]
struct RawArtifact {
    role: String,
    variant_key: String,
    generator_id: String,
    generator_version: String,
    provider_preview_id: Option<usize>,
    blob_algorithm: String,
    blob_digest: [u8; 32],
    blob_byte_len: u64,
    codec: String,
    byte_order: String,
    width: u32,
    height: u32,
    bits_per_channel: u16,
    channels: u16,
    created_at_ms: i64,
}

#[derive(Debug)]
struct RawReviewItem {
    photo_id: PhotoId,
    representation_id: RepresentationId,
    platform: String,
    native_path: Vec<u8>,
    display_path: String,
    source: RepresentationFingerprint,
    artifact: Option<RawArtifact>,
}

impl Catalog {
    /// Returns the online original-RAW source currently associated with a photo.
    ///
    /// This is the identity boundary used by detail/edit surfaces: callers may
    /// show a display path, but the catalog remains authoritative for which
    /// representation and location belong to the photo.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for unknown persisted values or a failed query.
    pub fn review_source(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT r.photo_id, r.id, l.platform, l.native_path, l.display_path,
                    r.byte_len, r.modified_at_ms,
                    a.role, a.variant_key, a.generator_id, a.generator_version,
                    a.provider_preview_id, a.blob_algorithm, a.blob_digest,
                    a.blob_byte_len, a.codec, a.byte_order, a.width, a.height,
                    a.bits_per_channel, a.channels, a.created_at_ms
             FROM representations r
             JOIN locations l ON l.id = (
                 SELECT l2.id FROM locations l2
                 WHERE l2.representation_id = r.id AND l2.status = 'online'
                 ORDER BY l2.created_at_ms, l2.id
                 LIMIT 1
             )
             LEFT JOIN representation_cached_artifacts a ON a.rowid = (
                 SELECT a2.rowid FROM representation_cached_artifacts a2
                 WHERE a2.representation_id = r.id
                   AND a2.source_byte_len = r.byte_len
                   AND a2.source_modified_at_ms IS r.modified_at_ms
                 ORDER BY CASE a2.role WHEN 'embedded_preview' THEN 0 ELSE 1 END,
                          (a2.width * a2.height) DESC,
                          a2.variant_key
                 LIMIT 1
             )
             WHERE r.photo_id = ?1 AND r.kind = 'original_raw'
             ORDER BY r.created_at_ms, r.id
             LIMIT 1",
        )?;
        let item = statement
            .query_row([photo_id.as_bytes().as_slice()], read_raw_review_item)
            .optional()?;
        item.map(review_item_from_raw).transpose()
    }

    /// Returns a bounded page containing one online original-RAW location per
    /// representation together with its preferred current grid visual.
    ///
    /// Embedded previews win over generated proxies. Stale artifact rows whose
    /// source fingerprint no longer matches the representation are excluded.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for unknown persisted values or a failed query.
    pub fn review_page(
        &self,
        after: Option<&ReviewCursor>,
        requested_limit: usize,
    ) -> Result<ReviewPageRecord, CatalogError> {
        let page_size = requested_limit.clamp(1, MAX_REVIEW_PAGE_SIZE);
        let fetch_limit = i64::try_from(page_size + 1).unwrap_or(i64::MAX);
        let cursor_path = after.map(|cursor| cursor.display_path.as_str());
        let cursor_id = after.map(|cursor| cursor.representation_id.as_bytes().as_slice());
        let mut statement = self.connection.prepare(
            "SELECT r.photo_id, r.id, l.platform, l.native_path, l.display_path,
                    r.byte_len, r.modified_at_ms,
                    a.role, a.variant_key, a.generator_id, a.generator_version,
                    a.provider_preview_id, a.blob_algorithm, a.blob_digest,
                    a.blob_byte_len, a.codec, a.byte_order, a.width, a.height,
                    a.bits_per_channel, a.channels, a.created_at_ms
             FROM representations r
             JOIN locations l ON l.id = (
                 SELECT l2.id FROM locations l2
                 WHERE l2.representation_id = r.id AND l2.status = 'online'
                 ORDER BY l2.created_at_ms, l2.id
                 LIMIT 1
             )
             LEFT JOIN representation_cached_artifacts a ON a.rowid = (
                 SELECT a2.rowid FROM representation_cached_artifacts a2
                 WHERE a2.representation_id = r.id
                   AND a2.source_byte_len = r.byte_len
                   AND a2.source_modified_at_ms IS r.modified_at_ms
                 ORDER BY CASE a2.role WHEN 'embedded_preview' THEN 0 ELSE 1 END,
                          (a2.width * a2.height) DESC,
                          a2.variant_key
                 LIMIT 1
             )
             WHERE r.kind = 'original_raw'
               AND (?1 IS NULL OR l.display_path > ?1
                    OR (l.display_path = ?1 AND r.id > ?2))
             ORDER BY l.display_path, r.id
             LIMIT ?3",
        )?;
        let rows = statement.query_map(
            rusqlite::params![cursor_path, cursor_id, fetch_limit],
            read_raw_review_item,
        )?;

        let mut items = Vec::new();
        for row in rows {
            items.push(review_item_from_raw(row?)?);
        }
        let has_more = items.len() > page_size;
        items.truncate(page_size);
        let next_cursor = if has_more {
            items.last().map(|last| ReviewCursor {
                display_path: last.location.display_path.clone(),
                representation_id: last.representation_id,
            })
        } else {
            None
        };
        let total_items = review_item_count(&self.connection)?;
        Ok(ReviewPageRecord {
            items,
            next_cursor,
            total_items,
        })
    }
}

fn review_item_from_raw(raw: RawReviewItem) -> Result<ReviewItemRecord, CatalogError> {
    let RawReviewItem {
        photo_id,
        representation_id,
        platform,
        native_path,
        display_path,
        source,
        artifact,
    } = raw;
    let location = AssetLocation::new(parse_platform(&platform)?, native_path, display_path);
    let visual = artifact
        .map(|artifact| cached_artifact(representation_id, source, artifact))
        .transpose()?;
    Ok(ReviewItemRecord {
        photo_id,
        representation_id,
        location,
        source,
        visual,
    })
}

fn read_raw_review_item(row: &rusqlite::Row<'_>) -> rusqlite::Result<RawReviewItem> {
    let byte_len = non_negative_u64(row.get(5)?, 5)?;
    let role = row.get::<_, Option<String>>(7)?;
    let artifact = if let Some(role) = role {
        Some(RawArtifact {
            role,
            variant_key: row.get(8)?,
            generator_id: row.get(9)?,
            generator_version: row.get(10)?,
            provider_preview_id: optional_usize(row.get(11)?, 11)?,
            blob_algorithm: row.get(12)?,
            blob_digest: digest(row.get(13)?, 13)?,
            blob_byte_len: non_negative_u64(row.get(14)?, 14)?,
            codec: row.get(15)?,
            byte_order: row.get(16)?,
            width: non_negative_u32(row.get(17)?, 17)?,
            height: non_negative_u32(row.get(18)?, 18)?,
            bits_per_channel: non_negative_u16(row.get(19)?, 19)?,
            channels: non_negative_u16(row.get(20)?, 20)?,
            created_at_ms: row.get(21)?,
        })
    } else {
        None
    };
    Ok(RawReviewItem {
        photo_id: read_id(row, 0)?,
        representation_id: read_id(row, 1)?,
        platform: row.get(2)?,
        native_path: row.get(3)?,
        display_path: row.get(4)?,
        source: RepresentationFingerprint {
            byte_len,
            modified_at_ms: row.get(6)?,
        },
        artifact,
    })
}

fn review_item_count(connection: &rusqlite::Connection) -> Result<u64, CatalogError> {
    let count = connection.query_row(
        "SELECT COUNT(*)
         FROM representations r
         WHERE r.kind = 'original_raw'
           AND EXISTS (
               SELECT 1 FROM locations l
               WHERE l.representation_id = r.id AND l.status = 'online'
           )",
        [],
        |row| row.get::<_, i64>(0),
    )?;
    u64::try_from(count).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(
            0,
            rusqlite::types::Type::Integer,
            Box::new(error),
        )
        .into()
    })
}

fn cached_artifact(
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    artifact: RawArtifact,
) -> Result<CachedArtifactRecord, CatalogError> {
    Ok(CachedArtifactRecord {
        representation_id,
        source,
        artifact: CachedArtifact {
            role: parse_role(artifact.role)?,
            variant_key: artifact.variant_key,
            generator_id: artifact.generator_id,
            generator_version: artifact.generator_version,
            provider_preview_id: artifact.provider_preview_id,
            blob_algorithm: artifact.blob_algorithm,
            blob_digest: artifact.blob_digest,
            blob_byte_len: artifact.blob_byte_len,
            codec: parse_codec(artifact.codec)?,
            byte_order: parse_byte_order(artifact.byte_order)?,
            dimensions: shadow_domain::ImageDimensions {
                width: artifact.width,
                height: artifact.height,
            },
            bits_per_channel: artifact.bits_per_channel,
            channels: artifact.channels,
            created_at_ms: artifact.created_at_ms,
        },
    })
}

fn parse_platform(value: &str) -> Result<Platform, CatalogError> {
    match value {
        "macos" => Ok(Platform::MacOs),
        "windows" => Ok(Platform::Windows),
        "other_unix" => Ok(Platform::OtherUnix),
        _ => Err(CatalogError::UnknownPlatform(value.to_owned())),
    }
}

#[cfg(test)]
mod tests {
    use shadow_domain::{ImageDimensions, PreviewByteOrder, PreviewCodec, RepresentationKind};

    use super::*;
    use crate::{CachedArtifactRole, RecordCachedArtifact, RegisterAsset};

    #[test]
    fn review_query_returns_one_source_with_preferred_current_visual() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let source = RepresentationFingerprint {
            byte_len: 4_096,
            modified_at_ms: Some(123),
        };
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/input.dng".to_vec(),
                    "/photos/input.dng",
                ),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 100,
            })
            .expect("register RAW");
        for (role, key, digest) in [
            (CachedArtifactRole::GeneratedProxy, "proxy-v1", [1; 32]),
            (CachedArtifactRole::EmbeddedPreview, "libraw", [2; 32]),
        ] {
            catalog
                .record_cached_artifact(&RecordCachedArtifact {
                    representation_id: registered.representation_id,
                    expected_source: source,
                    artifact: CachedArtifact {
                        role,
                        variant_key: key.into(),
                        generator_id: "libraw".into(),
                        generator_version: "1".into(),
                        provider_preview_id: None,
                        blob_algorithm: "blake3-256".into(),
                        blob_digest: digest,
                        blob_byte_len: 1_024,
                        codec: PreviewCodec::Jpeg,
                        byte_order: PreviewByteOrder::NotApplicable,
                        dimensions: ImageDimensions {
                            width: 1_600,
                            height: 1_200,
                        },
                        bits_per_channel: 8,
                        channels: 3,
                        created_at_ms: 456,
                    },
                })
                .expect("record visual");
        }

        let page = catalog.review_page(None, 128).expect("query Review page");
        assert_eq!(page.items.len(), 1);
        assert_eq!(page.total_items, 1);
        assert!(page.next_cursor.is_none());
        assert_eq!(page.items[0].photo_id, registered.photo_id);
        assert_eq!(page.items[0].location.display_path, "/photos/input.dng");
        assert_eq!(
            page.items[0]
                .visual
                .as_ref()
                .expect("preferred visual")
                .artifact
                .role,
            CachedArtifactRole::EmbeddedPreview
        );
    }

    #[test]
    fn review_query_pages_with_a_stable_path_and_id_cursor() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        for path in ["/photos/c.dng", "/photos/a.dng", "/photos/b.dng"] {
            catalog
                .register_asset(&RegisterAsset {
                    kind: RepresentationKind::OriginalRaw,
                    location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
                    byte_len: 4_096,
                    modified_at_ms: Some(123),
                    now_ms: 100,
                })
                .expect("register RAW");
        }

        let first = catalog.review_page(None, 2).expect("first Review page");
        assert_eq!(first.total_items, 3);
        assert_eq!(
            first
                .items
                .iter()
                .map(|item| item.location.display_path.as_str())
                .collect::<Vec<_>>(),
            ["/photos/a.dng", "/photos/b.dng"]
        );
        let second = catalog
            .review_page(first.next_cursor.as_ref(), 2)
            .expect("second Review page");
        assert_eq!(second.total_items, 3);
        assert_eq!(second.items.len(), 1);
        assert_eq!(second.items[0].location.display_path, "/photos/c.dng");
        assert!(second.next_cursor.is_none());
    }
}
