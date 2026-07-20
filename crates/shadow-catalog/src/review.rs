use rusqlite::OptionalExtension;
use shadow_domain::{AssetLocation, EntityId, PhotoId, Platform, RepresentationId};

use crate::{
    CachedArtifact, CachedArtifactRecord, Catalog, CatalogError, RepresentationFingerprint,
    TechnicalObservationRevision, TechnicalObservationSummary,
    cache_artifact::{
        digest, non_negative_u16, non_negative_u32, non_negative_u64, optional_usize,
        parse_byte_order, parse_codec, parse_role,
    },
    read_id,
    technical_observation::decode_observation,
};

/// One immutable row for the Review grid.
#[derive(Debug, Clone, PartialEq)]
pub struct ReviewItemRecord {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub location: AssetLocation,
    pub source: RepresentationFingerprint,
    pub visual: Option<CachedArtifactRecord>,
    pub technical: Option<TechnicalObservationSummary>,
}

/// Stable keyset cursor for the Review grid's path/id ordering.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ReviewCursor {
    pub display_path: String,
    pub representation_id: RepresentationId,
}

/// One bounded Review-grid page and the cursor needed to continue it.
#[derive(Debug, Clone, PartialEq)]
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
    technical: Option<RawTechnicalObservation>,
}

#[derive(Debug)]
struct RawTechnicalObservation {
    json: String,
    digest: [u8; 32],
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
        self.review_source_inner(photo_id, None)
    }

    /// Returns the source together with a technical summary only when the
    /// preferred visual has the caller's exact supported observation revision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for unknown persisted values, corrupt matching
    /// observation payloads, or a failed query.
    pub fn review_source_with_technical(
        &self,
        photo_id: PhotoId,
        revision: &TechnicalObservationRevision,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.review_source_inner(photo_id, Some(revision))
    }

    fn review_source_inner(
        &self,
        photo_id: PhotoId,
        revision: Option<&TechnicalObservationRevision>,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        let revision = supported_revision(revision);
        let mut statement = self.connection.prepare(
            "SELECT r.photo_id, r.id, l.platform, l.native_path, l.display_path,
                    r.byte_len, r.modified_at_ms,
                    a.role, a.variant_key, a.generator_id, a.generator_version,
                    a.provider_preview_id, a.blob_algorithm, a.blob_digest,
                    a.blob_byte_len, a.codec, a.byte_order, a.width, a.height,
                    a.bits_per_channel, a.channels, a.created_at_ms,
                    t.observation_json, t.observation_digest
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
             LEFT JOIN representation_technical_observations t ON t.rowid = (
                 SELECT t2.rowid FROM representation_technical_observations t2
                 WHERE ?2 IS NOT NULL
                   AND t2.representation_id = r.id
                   AND t2.source_role = a.role
                   AND t2.source_variant_key = a.variant_key
                   AND t2.source_generator_id = a.generator_id
                   AND t2.source_generator_version = a.generator_version
                   AND t2.source_provider_preview_id = COALESCE(a.provider_preview_id, -1)
                   AND t2.source_blob_algorithm = a.blob_algorithm
                   AND t2.source_blob_digest = a.blob_digest
                   AND t2.source_blob_byte_len = a.blob_byte_len
                   AND t2.source_codec = a.codec AND t2.source_byte_order = a.byte_order
                   AND t2.source_width = a.width AND t2.source_height = a.height
                   AND t2.source_bits_per_channel = a.bits_per_channel
                   AND t2.source_channels = a.channels
                   AND t2.source_created_at_ms = a.created_at_ms
                   AND t2.source_byte_len = r.byte_len
                   AND t2.source_modified_at_ms IS r.modified_at_ms
                   AND t2.observation_schema = ?2
                   AND t2.implementation_version = ?3
                   AND t2.display_luma_contract_version = ?4
                   AND t2.preprocessing_version = ?5
                 LIMIT 1
             )
             WHERE r.photo_id = ?1 AND r.kind = 'original_raw'
             ORDER BY r.created_at_ms, r.id
             LIMIT 1",
        )?;
        let item = statement
            .query_row(
                rusqlite::params![
                    photo_id.as_bytes().as_slice(),
                    revision.map(|value| i64::from(value.observation_schema)),
                    revision.map(|value| value.implementation_version.as_str()),
                    revision.map(|value| i64::from(value.display_luma_contract_version)),
                    revision.map(|value| value.preprocessing_version.as_str()),
                ],
                read_raw_review_item,
            )
            .optional()?;
        item.map(|raw| review_item_from_raw(raw, revision))
            .transpose()
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
        self.review_page_inner(after, requested_limit, None)
    }

    /// Returns a Review page with technical summaries for exactly one explicit
    /// preprocessing/algorithm revision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for unknown persisted values, corrupt matching
    /// observation payloads, or a failed query.
    pub fn review_page_with_technical(
        &self,
        after: Option<&ReviewCursor>,
        requested_limit: usize,
        revision: &TechnicalObservationRevision,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.review_page_inner(after, requested_limit, Some(revision))
    }

    fn review_page_inner(
        &self,
        after: Option<&ReviewCursor>,
        requested_limit: usize,
        revision: Option<&TechnicalObservationRevision>,
    ) -> Result<ReviewPageRecord, CatalogError> {
        let revision = supported_revision(revision);
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
                    a.bits_per_channel, a.channels, a.created_at_ms,
                    t.observation_json, t.observation_digest
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
             LEFT JOIN representation_technical_observations t ON t.rowid = (
                 SELECT t2.rowid FROM representation_technical_observations t2
                 WHERE ?4 IS NOT NULL
                   AND t2.representation_id = r.id
                   AND t2.source_role = a.role
                   AND t2.source_variant_key = a.variant_key
                   AND t2.source_generator_id = a.generator_id
                   AND t2.source_generator_version = a.generator_version
                   AND t2.source_provider_preview_id = COALESCE(a.provider_preview_id, -1)
                   AND t2.source_blob_algorithm = a.blob_algorithm
                   AND t2.source_blob_digest = a.blob_digest
                   AND t2.source_blob_byte_len = a.blob_byte_len
                   AND t2.source_codec = a.codec AND t2.source_byte_order = a.byte_order
                   AND t2.source_width = a.width AND t2.source_height = a.height
                   AND t2.source_bits_per_channel = a.bits_per_channel
                   AND t2.source_channels = a.channels
                   AND t2.source_created_at_ms = a.created_at_ms
                   AND t2.source_byte_len = r.byte_len
                   AND t2.source_modified_at_ms IS r.modified_at_ms
                   AND t2.observation_schema = ?4
                   AND t2.implementation_version = ?5
                   AND t2.display_luma_contract_version = ?6
                   AND t2.preprocessing_version = ?7
                 LIMIT 1
             )
             WHERE r.kind = 'original_raw'
               AND (?1 IS NULL OR l.display_path > ?1
                    OR (l.display_path = ?1 AND r.id > ?2))
             ORDER BY l.display_path, r.id
             LIMIT ?3",
        )?;
        let rows = statement.query_map(
            rusqlite::params![
                cursor_path,
                cursor_id,
                fetch_limit,
                revision.map(|value| i64::from(value.observation_schema)),
                revision.map(|value| value.implementation_version.as_str()),
                revision.map(|value| i64::from(value.display_luma_contract_version)),
                revision.map(|value| value.preprocessing_version.as_str()),
            ],
            read_raw_review_item,
        )?;

        let mut items = Vec::new();
        for row in rows {
            items.push(review_item_from_raw(row?, revision)?);
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

fn review_item_from_raw(
    raw: RawReviewItem,
    revision: Option<&TechnicalObservationRevision>,
) -> Result<ReviewItemRecord, CatalogError> {
    let RawReviewItem {
        photo_id,
        representation_id,
        platform,
        native_path,
        display_path,
        source,
        artifact,
        technical,
    } = raw;
    let location = AssetLocation::new(parse_platform(&platform)?, native_path, display_path);
    let visual = artifact
        .map(|artifact| cached_artifact(representation_id, source, artifact))
        .transpose()?;
    let technical = match (technical, visual.as_ref(), revision) {
        (Some(technical), Some(visual), Some(revision)) => {
            let observation = decode_observation(
                &technical.json,
                technical.digest,
                revision,
                &visual.artifact,
            );
            match observation {
                Ok(observation) => Some(TechnicalObservationSummary::from(&observation)),
                Err(
                    CatalogError::InvalidPersistedTechnicalObservation(_)
                    | CatalogError::TechnicalObservationJson(_),
                ) => None,
                Err(error) => return Err(error),
            }
        }
        _ => None,
    };
    Ok(ReviewItemRecord {
        photo_id,
        representation_id,
        location,
        source,
        visual,
        technical,
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
    let technical = if let Some(json) = row.get::<_, Option<String>>(22)? {
        Some(RawTechnicalObservation {
            json,
            digest: digest(row.get(23)?, 23)?,
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
        technical,
    })
}

fn supported_revision(
    revision: Option<&TechnicalObservationRevision>,
) -> Option<&TechnicalObservationRevision> {
    revision.filter(|revision| revision.is_supported_by_this_build())
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
    use shadow_ai::{DISPLAY_LUMA_CONTRACT_VERSION, DisplayLumaPlane, observe_display_luma};
    use shadow_domain::{ImageDimensions, PreviewByteOrder, PreviewCodec, RepresentationKind};

    use super::*;
    use crate::{
        CachedArtifactRole, RecordCachedArtifact, RecordTechnicalObservation, RegisterAsset,
        technical_observation::artifact_content_hash,
    };

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
        for (role, key, digest, dimensions) in [
            (
                CachedArtifactRole::GeneratedProxy,
                "proxy-v1",
                [1; 32],
                ImageDimensions {
                    width: 4_000,
                    height: 3_000,
                },
            ),
            (
                CachedArtifactRole::EmbeddedPreview,
                "z-small",
                [2; 32],
                ImageDimensions {
                    width: 1_600,
                    height: 1_200,
                },
            ),
            (
                CachedArtifactRole::EmbeddedPreview,
                "b-large",
                [3; 32],
                ImageDimensions {
                    width: 2_000,
                    height: 1_000,
                },
            ),
            (
                CachedArtifactRole::EmbeddedPreview,
                "a-large",
                [4; 32],
                ImageDimensions {
                    width: 2_000,
                    height: 1_000,
                },
            ),
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
                        dimensions,
                        bits_per_channel: 8,
                        channels: 3,
                        created_at_ms: 456,
                    },
                })
                .expect("record visual");
        }

        let preferred = catalog
            .preferred_cached_artifact(registered.representation_id)
            .expect("select shared preferred visual")
            .expect("preferred visual exists");
        let page = catalog.review_page(None, 128).expect("query Review page");
        assert_eq!(page.items.len(), 1);
        assert_eq!(page.total_items, 1);
        assert!(page.next_cursor.is_none());
        assert_eq!(page.items[0].photo_id, registered.photo_id);
        assert_eq!(page.items[0].location.display_path, "/photos/input.dng");
        assert_eq!(page.items[0].visual.as_ref(), Some(&preferred));
        assert_eq!(
            page.items[0]
                .visual
                .as_ref()
                .expect("preferred visual")
                .artifact
                .role,
            CachedArtifactRole::EmbeddedPreview
        );
        assert_eq!(preferred.artifact.variant_key, "a-large");
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

    #[test]
    fn explicit_technical_revision_keeps_pagination_to_one_row_per_representation() {
        let mut catalog = technical_review_catalog();
        let revision = TechnicalObservationRevision::current("jpeg-luma-v2");
        let first = catalog
            .review_page_with_technical(None, 2, &revision)
            .expect("first Review page");
        assert_eq!(first.total_items, 3);
        assert_eq!(first.items.len(), 2);
        assert!(first.items.iter().all(|item| {
            item.technical
                .as_ref()
                .is_some_and(|summary| summary.preprocessing_version == "jpeg-luma-v2")
        }));
        let second = catalog
            .review_page_with_technical(first.next_cursor.as_ref(), 2, &revision)
            .expect("second Review page");
        assert_eq!(second.items.len(), 1);
        assert!(second.items[0].technical.is_some());
        assert!(second.next_cursor.is_none());

        assert_corrupt_observations_fail_soft(&mut catalog, &revision);
    }

    fn technical_review_catalog() -> Catalog {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        for (index, path) in ["/photos/c.dng", "/photos/a.dng", "/photos/b.dng"]
            .into_iter()
            .enumerate()
        {
            let source = RepresentationFingerprint {
                byte_len: 4_096,
                modified_at_ms: Some(123),
            };
            let registered = catalog
                .register_asset(&RegisterAsset {
                    kind: RepresentationKind::OriginalRaw,
                    location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
                    byte_len: source.byte_len,
                    modified_at_ms: source.modified_at_ms,
                    now_ms: 100,
                })
                .expect("register RAW");
            let artifact = CachedArtifact {
                role: CachedArtifactRole::GeneratedProxy,
                variant_key: "proxy-v1".into(),
                generator_id: "libraw".into(),
                generator_version: "1".into(),
                provider_preview_id: None,
                blob_algorithm: "blake3-256".into(),
                blob_digest: [u8::try_from(index + 1).expect("small index"); 32],
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
            };
            catalog
                .record_cached_artifact(&RecordCachedArtifact {
                    representation_id: registered.representation_id,
                    expected_source: source,
                    artifact: artifact.clone(),
                })
                .expect("record visual");
            for preprocessing_version in ["jpeg-luma-v1", "jpeg-luma-v2"] {
                let input_source_hash = artifact_content_hash(&artifact);
                let observation = observe_display_luma(DisplayLumaPlane {
                    contract_version: DISPLAY_LUMA_CONTRACT_VERSION,
                    width: 2,
                    height: 2,
                    stride: 2,
                    samples: &[0.0, 0.25, 0.75, 1.0],
                    preprocessing_version,
                    input_source_hash: &input_source_hash,
                })
                .expect("observe luma");
                catalog
                    .record_technical_observation(&RecordTechnicalObservation {
                        representation_id: registered.representation_id,
                        expected_source: source,
                        expected_artifact: artifact.clone(),
                        observation,
                        observed_at_ms: 500,
                    })
                    .expect("record observation");
            }
        }
        catalog
    }

    fn assert_corrupt_observations_fail_soft(
        catalog: &mut Catalog,
        revision: &TechnicalObservationRevision,
    ) {
        catalog
            .connection
            .execute(
                "UPDATE representation_technical_observations
                 SET observation_digest = zeroblob(32)
                 WHERE preprocessing_version = 'jpeg-luma-v2'",
                [],
            )
            .expect("corrupt rebuildable observations");
        let degraded = catalog
            .review_page_with_technical(None, 3, revision)
            .expect("corrupt optional observations do not break Review");
        assert_eq!(degraded.items.len(), 3);
        assert!(degraded.items.iter().all(|item| item.technical.is_none()));

        let without_revision = catalog.review_page(None, 3).expect("plain Review page");
        assert!(
            without_revision
                .items
                .iter()
                .all(|item| item.technical.is_none())
        );
    }
}
