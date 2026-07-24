use rusqlite::OptionalExtension;
use shadow_domain::{
    AssetLocation, EntityId, PhotoDecisionState, PhotoId, Platform, RawMetadataSnapshot,
    RepresentationId,
};

use crate::{
    CachedArtifact, CachedArtifactGeneratorIdentity, CachedArtifactRecord, Catalog, CatalogError,
    RepresentationFingerprint, TechnicalObservationRevision, TechnicalObservationSummary,
    cache_artifact::{
        digest, non_negative_u16, non_negative_u32, non_negative_u64, optional_usize,
        parse_byte_order, parse_codec, parse_role,
    },
    decision::photo_decision_state_from_columns,
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
    pub metadata: Option<RawMetadataSnapshot>,
    pub technical: Option<TechnicalObservationSummary>,
    pub decision: PhotoDecisionState,
    /// Whether this photo has a durable working development recipe.
    ///
    /// This is intentionally independent from the currently selected visual:
    /// a generated proxy can represent an untouched photo, while an edited
    /// photo may still be waiting for its recipe preview to render.
    pub has_development_edits: bool,
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

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
enum SourceSelection {
    RawOnly,
    RawPreferredWithRasterFallback,
}

impl SourceSelection {
    const fn includes_original_raster(self) -> bool {
        matches!(self, Self::RawPreferredWithRasterFallback)
    }
}

#[derive(Debug)]
struct StoredArtifact {
    role: String,
    variant_key: String,
    generator_id: String,
    generator_version: String,
    recipe_snapshot_digest: Option<[u8; 32]>,
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
struct StoredReviewItem {
    photo_id: PhotoId,
    representation_id: RepresentationId,
    platform: String,
    native_path: Vec<u8>,
    display_path: String,
    source: RepresentationFingerprint,
    artifact: Option<StoredArtifact>,
    metadata_json: Option<String>,
    technical: Option<StoredTechnicalObservation>,
    decision_head_sequence: Option<i64>,
    decision_flag: Option<String>,
    decision_rating: Option<i64>,
    has_development_edits: bool,
}

#[derive(Debug)]
struct StoredTechnicalObservation {
    json: String,
    digest: [u8; 32],
}

#[derive(serde::Deserialize)]
struct MetadataEnvelope {
    metadata: RawMetadataSnapshot,
}

fn decode_review_metadata(json: &str) -> Option<RawMetadataSnapshot> {
    // The Review index consumes only metadata. Decoder snapshots also contain
    // provider capabilities, preview descriptors, and RAW-development
    // contracts which evolve independently. Deserializing the complete
    // `DecoderSnapshot` here made an unrelated capability rename hide valid
    // EXIF from the Library until every source had been re-inspected.
    serde_json::from_str::<MetadataEnvelope>(json)
        .ok()
        .map(|envelope| envelope.metadata)
}

impl Catalog {
    /// Returns the online original-RAW source currently associated with a photo.
    ///
    /// This is deliberately the identity boundary used by legacy RAW
    /// detail/edit surfaces: callers may show a display path, but the catalog
    /// remains authoritative for which RAW representation and location belong
    /// to the photo. Source-neutral consumers must use [`Self::photo_source`]
    /// instead.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for unknown persisted values or a failed query.
    pub fn review_source(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.source_inner(photo_id, None, SourceSelection::RawOnly)
    }

    /// Returns the online original source for a photo, preferring RAW and
    /// falling back to an original raster.
    ///
    /// This is the source-neutral selection used by an editor that can route
    /// both kinds. The catalog chooses only an online source representation;
    /// the caller remains responsible for ensuring that its active provider
    /// can actually decode the returned path. [`Self::review_source`] remains
    /// RAW-only for legacy callers that require that narrower contract.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for unknown persisted values or a failed query.
    pub fn photo_source(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.source_inner(
            photo_id,
            None,
            SourceSelection::RawPreferredWithRasterFallback,
        )
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
        self.source_inner(photo_id, Some(revision), SourceSelection::RawOnly)
    }

    fn source_inner(
        &self,
        photo_id: PhotoId,
        revision: Option<&TechnicalObservationRevision>,
        selection: SourceSelection,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        let revision = supported_revision(revision);
        let mut statement = self.connection.prepare(
            "SELECT r.photo_id, r.id, l.platform, l.native_path, l.display_path,
                    r.byte_len, r.modified_at_ms,
                    a.role, a.variant_key, a.generator_id, a.generator_version,
                    a.recipe_snapshot_digest, a.provider_preview_id, a.blob_algorithm,
                    a.blob_digest, a.blob_byte_len, a.codec, a.byte_order, a.width,
                    a.height, a.bits_per_channel, a.channels, a.created_at_ms,
                    t.observation_json, t.observation_digest,
                    s.snapshot_json,
                    dc.head_sequence, de.after_flag, de.after_rating,
                    EXISTS (
                        SELECT 1 FROM recipe_refs edit_ref
                        WHERE edit_ref.photo_id = r.photo_id
                          AND edit_ref.name = 'working'
                    )
             FROM representations r
             JOIN locations l ON l.id = (
                 SELECT l2.id FROM locations l2
                 WHERE l2.representation_id = r.id AND l2.status = 'online'
                 ORDER BY l2.created_at_ms DESC, l2.id DESC
                 LIMIT 1
             )
             LEFT JOIN representation_cached_artifacts a ON a.rowid = (
                 SELECT a2.rowid FROM representation_cached_artifacts a2
                 WHERE a2.representation_id = r.id
                   AND a2.source_byte_len = r.byte_len
                   AND a2.source_modified_at_ms IS r.modified_at_ms
                   AND (
                       a2.role != 'recipe_preview'
                       OR EXISTS (
                           SELECT 1 FROM recipe_refs rr
                           JOIN recipe_commits rc
                             ON rc.id = rr.commit_id AND rc.photo_id = rr.photo_id
                           WHERE rr.photo_id = r.photo_id
                             AND rr.name = 'working'
                             AND rc.snapshot_digest = a2.recipe_snapshot_digest
                       )
                   )
                 ORDER BY CASE a2.role
                              WHEN 'recipe_preview' THEN 0
                              WHEN 'generated_proxy' THEN 1
                              ELSE 2
                          END,
                          (a2.width * a2.height) DESC,
                          a2.created_at_ms DESC,
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
             LEFT JOIN representation_decode_snapshots s ON s.rowid = (
                 SELECT s2.rowid FROM representation_decode_snapshots s2
                 WHERE s2.representation_id = r.id
                   AND s2.source_byte_len = r.byte_len
                   AND s2.source_modified_at_ms IS r.modified_at_ms
                   AND s2.has_metadata = 1
                 ORDER BY CASE s2.provider_id WHEN 'libraw' THEN 0 ELSE 1 END,
                          s2.inspected_at_ms DESC
                 LIMIT 1
             )
             LEFT JOIN photo_decision_current dc ON dc.photo_id = r.photo_id
             LEFT JOIN photo_decision_events de
               ON de.sequence = dc.head_sequence AND de.photo_id = r.photo_id
             WHERE r.photo_id = ?1
               AND (r.kind = 'original_raw'
                    OR (?6 != 0 AND r.kind = 'original_raster'))
             ORDER BY CASE r.kind WHEN 'original_raw' THEN 0 ELSE 1 END,
                      r.created_at_ms, r.id
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
                    i64::from(selection.includes_original_raster()),
                ],
                read_review_item,
            )
            .optional()?;
        item.map(|stored| review_item_from_stored(stored, revision))
            .transpose()
    }

    /// Returns a bounded page containing one online original source location
    /// (RAW or raster) per representation together with its preferred current
    /// grid visual.
    ///
    /// Shadow-generated proxies win over camera-embedded previews. The
    /// embedded image remains an immediate placeholder while the generated
    /// proxy is prepared. Stale artifact rows whose source fingerprint no
    /// longer matches the representation are excluded.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for unknown persisted values or a failed query.
    pub fn review_page(
        &self,
        after: Option<&ReviewCursor>,
        requested_limit: usize,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.review_page_inner(after, requested_limit, None, None)
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
        self.review_page_inner(after, requested_limit, Some(revision), None)
    }

    /// Returns a Review page whose edited visuals were produced by the exact
    /// Recipe-preview implementation expected by the caller.
    ///
    /// A matching working Recipe with only an older generator's preview is
    /// reported as edited but falls back to another current source visual. The
    /// older cache row is retained so cache policy remains non-destructive.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for unknown persisted values, corrupt matching
    /// observation payloads, or a failed query.
    pub fn review_page_with_technical_and_recipe_preview_generator(
        &self,
        after: Option<&ReviewCursor>,
        requested_limit: usize,
        revision: &TechnicalObservationRevision,
        recipe_preview_generator: &CachedArtifactGeneratorIdentity,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.review_page_inner(
            after,
            requested_limit,
            Some(revision),
            Some(recipe_preview_generator),
        )
    }

    // Keeping the positional SQL projection beside its row decoder makes schema drift auditable.
    #[allow(clippy::too_many_lines)]
    fn review_page_inner(
        &self,
        after: Option<&ReviewCursor>,
        requested_limit: usize,
        revision: Option<&TechnicalObservationRevision>,
        recipe_preview_generator: Option<&CachedArtifactGeneratorIdentity>,
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
                    a.recipe_snapshot_digest, a.provider_preview_id, a.blob_algorithm,
                    a.blob_digest, a.blob_byte_len, a.codec, a.byte_order, a.width,
                    a.height, a.bits_per_channel, a.channels, a.created_at_ms,
                    t.observation_json, t.observation_digest,
                    s.snapshot_json,
                    dc.head_sequence, de.after_flag, de.after_rating,
                    EXISTS (
                        SELECT 1 FROM recipe_refs edit_ref
                        WHERE edit_ref.photo_id = r.photo_id
                          AND edit_ref.name = 'working'
                    )
             FROM representations r
             JOIN locations l ON l.id = (
                 SELECT l2.id FROM locations l2
                 WHERE l2.representation_id = r.id AND l2.status = 'online'
                 ORDER BY l2.created_at_ms DESC, l2.id DESC
                 LIMIT 1
             )
             LEFT JOIN representation_cached_artifacts a ON a.rowid = (
                 SELECT a2.rowid FROM representation_cached_artifacts a2
                 WHERE a2.representation_id = r.id
                   AND a2.source_byte_len = r.byte_len
                   AND a2.source_modified_at_ms IS r.modified_at_ms
                   AND (
                       a2.role != 'recipe_preview'
                       OR (
                           (?8 IS NULL OR (
                               a2.generator_id = ?8
                               AND a2.generator_version = ?9
                           ))
                           AND EXISTS (
                               SELECT 1 FROM recipe_refs rr
                               JOIN recipe_commits rc
                                 ON rc.id = rr.commit_id AND rc.photo_id = rr.photo_id
                               WHERE rr.photo_id = r.photo_id
                                 AND rr.name = 'working'
                                 AND rc.snapshot_digest = a2.recipe_snapshot_digest
                           )
                       )
                   )
                 ORDER BY CASE a2.role
                              WHEN 'recipe_preview' THEN 0
                              WHEN 'generated_proxy' THEN 1
                              ELSE 2
                          END,
                          (a2.width * a2.height) DESC,
                          a2.created_at_ms DESC,
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
             LEFT JOIN representation_decode_snapshots s ON s.rowid = (
                 SELECT s2.rowid FROM representation_decode_snapshots s2
                 WHERE s2.representation_id = r.id
                   AND s2.source_byte_len = r.byte_len
                   AND s2.source_modified_at_ms IS r.modified_at_ms
                   AND s2.has_metadata = 1
                 ORDER BY CASE s2.provider_id WHEN 'libraw' THEN 0 ELSE 1 END,
                          s2.inspected_at_ms DESC
                 LIMIT 1
             )
             LEFT JOIN photo_decision_current dc ON dc.photo_id = r.photo_id
             LEFT JOIN photo_decision_events de
               ON de.sequence = dc.head_sequence AND de.photo_id = r.photo_id
             WHERE r.kind IN ('original_raw', 'original_raster')
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
                recipe_preview_generator.map(|identity| identity.generator_id.as_str()),
                recipe_preview_generator.map(|identity| identity.generator_version.as_str()),
            ],
            read_review_item,
        )?;

        let mut items = Vec::new();
        for row in rows {
            items.push(review_item_from_stored(row?, revision)?);
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

fn review_item_from_stored(
    stored: StoredReviewItem,
    revision: Option<&TechnicalObservationRevision>,
) -> Result<ReviewItemRecord, CatalogError> {
    let StoredReviewItem {
        photo_id,
        representation_id,
        platform,
        native_path,
        display_path,
        source,
        artifact,
        metadata_json,
        technical,
        decision_head_sequence,
        decision_flag,
        decision_rating,
        has_development_edits,
    } = stored;
    let location = AssetLocation::new(parse_platform(&platform)?, native_path, display_path);
    let visual = artifact
        .map(|artifact| cached_artifact(representation_id, source, artifact))
        .transpose()?;
    // Decode snapshots are replaceable caches. A corrupt optional metadata payload must not
    // make the Review grid unusable; the inspection queue can rebuild it independently.
    let metadata = metadata_json.and_then(|json| decode_review_metadata(&json));
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
    let decision =
        photo_decision_state_from_columns(decision_head_sequence, decision_flag, decision_rating)?;
    Ok(ReviewItemRecord {
        photo_id,
        representation_id,
        location,
        source,
        visual,
        metadata,
        technical,
        decision,
        has_development_edits,
    })
}

fn read_review_item(row: &rusqlite::Row<'_>) -> rusqlite::Result<StoredReviewItem> {
    let byte_len = non_negative_u64(row.get(5)?, 5)?;
    let role = row.get::<_, Option<String>>(7)?;
    let artifact = if let Some(role) = role {
        Some(StoredArtifact {
            role,
            variant_key: row.get(8)?,
            generator_id: row.get(9)?,
            generator_version: row.get(10)?,
            recipe_snapshot_digest: row
                .get::<_, Option<Vec<u8>>>(11)?
                .map(|value| digest(value, 11))
                .transpose()?,
            provider_preview_id: optional_usize(row.get(12)?, 12)?,
            blob_algorithm: row.get(13)?,
            blob_digest: digest(row.get(14)?, 14)?,
            blob_byte_len: non_negative_u64(row.get(15)?, 15)?,
            codec: row.get(16)?,
            byte_order: row.get(17)?,
            width: non_negative_u32(row.get(18)?, 18)?,
            height: non_negative_u32(row.get(19)?, 19)?,
            bits_per_channel: non_negative_u16(row.get(20)?, 20)?,
            channels: non_negative_u16(row.get(21)?, 21)?,
            created_at_ms: row.get(22)?,
        })
    } else {
        None
    };
    let technical = if let Some(json) = row.get::<_, Option<String>>(23)? {
        Some(StoredTechnicalObservation {
            json,
            digest: digest(row.get(24)?, 24)?,
        })
    } else {
        None
    };
    Ok(StoredReviewItem {
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
        metadata_json: row.get(25)?,
        technical,
        decision_head_sequence: row.get(26)?,
        decision_flag: row.get(27)?,
        decision_rating: row.get(28)?,
        has_development_edits: row.get(29)?,
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
         WHERE r.kind IN ('original_raw', 'original_raster')
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
    artifact: StoredArtifact,
) -> Result<CachedArtifactRecord, CatalogError> {
    Ok(CachedArtifactRecord {
        representation_id,
        source,
        artifact: CachedArtifact {
            role: parse_role(artifact.role)?,
            variant_key: artifact.variant_key,
            generator_id: artifact.generator_id,
            generator_version: artifact.generator_version,
            recipe_snapshot_digest: artifact.recipe_snapshot_digest,
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
    use shadow_domain::{
        EntityId, ImageDimensions, NewPhotoDecisionEvent, PhotoDecisionOrigin, PhotoDecisionState,
        PhotoFlag, PreviewByteOrder, PreviewCodec, RecipeCommit, RecipeId, RecipeSnapshot,
        RepresentationKind,
    };

    use super::*;
    use crate::{
        CachedArtifactRole, CommitRecipe, RecipeRefKind, RecipeRefTarget, RecordCachedArtifact,
        RecordTechnicalObservation, RegisterAsset, technical_observation::artifact_content_hash,
    };

    #[test]
    fn review_metadata_survives_unrelated_decoder_snapshot_schema_changes() {
        let json = r#"{
            "provider":{"id":"legacy-provider"},
            "metadata":{
                "make":"Canon","model":"EOS R",
                "normalized_make":"Canon","normalized_model":"EOS R",
                "dng_version":null,"raw_count":1,
                "raw_dimensions":{"width":6888,"height":4546},
                "image_dimensions":{"width":6742,"height":4498},
                "margins":{"left":146,"top":48,"right":0,"bottom":0},
                "orientation":0,"cfa_pattern":"RGGB","sensor_colors":3,
                "sensor_bits":14,"black_level":0,"white_level":16383,
                "as_shot_neutral":[0.0,0.0,0.0,0.0],
                "baseline_exposure":-999.0,"iso_speed":100.0,
                "exposure_time_seconds":1.6,"aperture_f_number":9.0,
                "focal_length_mm":50.0,"captured_at_unix_seconds":1551392920,
                "lens_make":"","lens_model":"RF50mm F1.2 L USM",
                "focal_length_35mm":0.0
            },
            "capabilities":{"removed_legacy_shape":"must not affect EXIF"},
            "previews":"also deliberately incompatible"
        }"#;

        let metadata = decode_review_metadata(json).expect("decode metadata-only envelope");
        assert_eq!(metadata.make, "Canon");
        assert_eq!(metadata.model, "EOS R");
        assert_eq!(metadata.sensor_bits, 14);
        assert_eq!(metadata.raw_dimensions.width, 6_888);
        assert_eq!(metadata.lens_model, "RF50mm F1.2 L USM");
    }

    #[test]
    fn review_source_and_page_join_the_pointer_only_decision_projection() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/decision-join.dng".to_vec(),
                    "/photos/decision-join.dng",
                ),
                byte_len: 4_096,
                modified_at_ms: Some(123),
                now_ms: 100,
            })
            .expect("register decision Review source");
        assert_eq!(
            catalog
                .review_source(registered.photo_id)
                .expect("read default Review source")
                .expect("default Review source")
                .decision,
            PhotoDecisionState::default()
        );
        assert_eq!(
            catalog
                .review_page(None, 10)
                .expect("read default Review page")
                .items[0]
                .decision,
            PhotoDecisionState::default()
        );
        assert!(
            !catalog
                .review_source(registered.photo_id)
                .expect("read untouched Review source")
                .expect("untouched Review source")
                .has_development_edits
        );

        let recipe = RecipeCommit::new(
            shadow_domain::RecipeCommitId::new_v7(),
            RecipeId::new_v7(),
            Vec::new(),
            RecipeSnapshot::empty(),
            Some("working edit".into()),
            200,
        )
        .expect("build working Recipe");
        catalog
            .commit_recipe(&CommitRecipe {
                photo_id: registered.photo_id,
                commit: recipe,
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: None,
                }],
            })
            .expect("persist working Recipe");
        assert!(
            catalog
                .review_page(None, 10)
                .expect("read edited Review page")
                .items[0]
                .has_development_edits
        );

        let event = catalog
            .append_photo_decision_event(&NewPhotoDecisionEvent {
                event_id: "review-decision-join".into(),
                photo_id: registered.photo_id,
                occurred_at_unix_ms: 1_700_000_001_000,
                origin: PhotoDecisionOrigin::Human,
                expected_head_sequence: 0,
                before_flag: PhotoFlag::Unflagged,
                before_rating: 0,
                after_flag: PhotoFlag::Picked,
                after_rating: 4,
            })
            .expect("append Review decision");
        let expected = event.after_state().expect("decision state");
        assert_eq!(
            catalog
                .review_source(registered.photo_id)
                .expect("read decided Review source")
                .expect("decided Review source")
                .decision,
            expected
        );
        assert_eq!(
            catalog
                .review_page(None, 10)
                .expect("read decided Review page")
                .items[0]
                .decision,
            expected
        );
    }

    #[test]
    fn review_page_treats_an_old_recipe_preview_generator_as_a_cache_miss() {
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
                    b"/photos/generator-aware.dng".to_vec(),
                    "/photos/generator-aware.dng",
                ),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 100,
            })
            .expect("register RAW");
        let generated_proxy = CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: "proxy-v1".into(),
            generator_id: "libraw".into(),
            generator_version: "1".into(),
            recipe_snapshot_digest: None,
            provider_preview_id: None,
            blob_algorithm: "blake3-256".into(),
            blob_digest: [1; 32],
            blob_byte_len: 1_024,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 1_600,
                height: 1_200,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 150,
        };
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: registered.representation_id,
                expected_source: source,
                artifact: generated_proxy.clone(),
            })
            .expect("record generated fallback");

        let recipe = RecipeCommit::new(
            shadow_domain::RecipeCommitId::new_v7(),
            RecipeId::new_v7(),
            Vec::new(),
            RecipeSnapshot::empty(),
            Some("working edit".into()),
            200,
        )
        .expect("build working Recipe");
        let recipe = catalog
            .commit_recipe(&CommitRecipe {
                photo_id: registered.photo_id,
                commit: recipe,
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: None,
                }],
            })
            .expect("persist working Recipe");
        let stale_preview = CachedArtifact {
            role: CachedArtifactRole::RecipePreview,
            variant_key: "recipe-old-environment".into(),
            generator_id: "shadow-edit-preview".into(),
            generator_version: "shadow-edit-preview-v1;environment=old".into(),
            recipe_snapshot_digest: Some(recipe.snapshot_digest),
            blob_digest: [2; 32],
            dimensions: ImageDimensions {
                width: 2_400,
                height: 1_800,
            },
            created_at_ms: 250,
            ..generated_proxy.clone()
        };
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: registered.representation_id,
                expected_source: source,
                artifact: stale_preview.clone(),
            })
            .expect("record old-environment Recipe preview");

        let revision = TechnicalObservationRevision::current("generator-aware-review");
        let current_generator = CachedArtifactGeneratorIdentity {
            generator_id: "shadow-edit-preview".into(),
            generator_version: "shadow-edit-preview-v1;environment=current".into(),
        };
        let page = catalog
            .review_page_with_technical_and_recipe_preview_generator(
                None,
                10,
                &revision,
                &current_generator,
            )
            .expect("read generator-aware Review page");
        assert!(page.items[0].has_development_edits);
        assert_eq!(
            page.items[0]
                .visual
                .as_ref()
                .expect("fallback visual")
                .artifact,
            generated_proxy
        );
        assert!(
            catalog
                .cached_artifacts(registered.representation_id)
                .expect("list retained cache rows")
                .iter()
                .any(|record| record.artifact == stale_preview)
        );

        let current_preview = CachedArtifact {
            variant_key: "recipe-current-environment".into(),
            generator_version: current_generator.generator_version.clone(),
            blob_digest: [3; 32],
            dimensions: ImageDimensions {
                width: 1_200,
                height: 900,
            },
            created_at_ms: 300,
            ..stale_preview
        };
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: registered.representation_id,
                expected_source: source,
                artifact: current_preview.clone(),
            })
            .expect("record current-environment Recipe preview");
        let refreshed = catalog
            .review_page_with_technical_and_recipe_preview_generator(
                None,
                10,
                &revision,
                &current_generator,
            )
            .expect("read refreshed Review page");
        assert_eq!(
            refreshed.items[0]
                .visual
                .as_ref()
                .expect("current Recipe preview")
                .artifact,
            current_preview
        );
    }

    #[test]
    fn review_page_includes_original_rasters_while_raw_edit_source_stays_raw_only() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let raw = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/source.nef".to_vec(),
                    "/photos/source.nef",
                ),
                byte_len: 4_096,
                modified_at_ms: Some(123),
                now_ms: 100,
            })
            .expect("register RAW review source");
        let raster = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaster,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/source.jpg".to_vec(),
                    "/photos/source.jpg",
                ),
                byte_len: 2_048,
                modified_at_ms: Some(124),
                now_ms: 101,
            })
            .expect("register raster review source");

        let page = catalog.review_page(None, 16).expect("read Review page");
        assert_eq!(page.total_items, 2);
        assert!(page.items.iter().any(|item| {
            item.representation_id == raw.representation_id
                && item.location.display_path == "/photos/source.nef"
        }));
        assert!(page.items.iter().any(|item| {
            item.representation_id == raster.representation_id
                && item.location.display_path == "/photos/source.jpg"
        }));

        assert_eq!(
            catalog
                .review_source(raw.photo_id)
                .expect("read RAW edit source")
                .expect("RAW edit source exists")
                .representation_id,
            raw.representation_id
        );
        assert!(
            catalog
                .review_source(raster.photo_id)
                .expect("read raster edit source")
                .is_none(),
            "a raster must not be handed to the current RAW-only editor"
        );
        assert_eq!(
            catalog
                .photo_source(raster.photo_id)
                .expect("read source-neutral raster fallback")
                .expect("original raster is an editable source")
                .representation_id,
            raster.representation_id
        );

        // A photo can own multiple source representations. Production code
        // creates that relationship through import/linking work; the query
        // contract itself is verified directly here.
        catalog
            .connection
            .execute(
                "UPDATE representations SET photo_id = ?1 WHERE id = ?2",
                rusqlite::params![
                    raw.photo_id.as_bytes().as_slice(),
                    raster.representation_id.as_bytes().as_slice(),
                ],
            )
            .expect("attach raster representation to RAW photo");
        assert_eq!(
            catalog
                .photo_source(raw.photo_id)
                .expect("read RAW-preferred source")
                .expect("online source")
                .representation_id,
            raw.representation_id,
            "an online RAW remains the first choice when both representations exist"
        );

        catalog
            .connection
            .execute(
                "UPDATE locations SET status = 'offline' WHERE representation_id = ?1",
                [raw.representation_id.as_bytes().as_slice()],
            )
            .expect("mark RAW source offline");
        assert_eq!(
            catalog
                .photo_source(raw.photo_id)
                .expect("read raster fallback after RAW is offline")
                .expect("online raster fallback")
                .representation_id,
            raster.representation_id
        );
        assert!(
            catalog
                .review_source(raw.photo_id)
                .expect("read RAW-only source after RAW is offline")
                .is_none(),
            "the legacy RAW-only query must not silently become source-neutral"
        );
    }

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
                        recipe_snapshot_digest: None,
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
            CachedArtifactRole::GeneratedProxy
        );
        assert_eq!(preferred.artifact.variant_key, "proxy-v1");
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
                recipe_snapshot_digest: None,
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
