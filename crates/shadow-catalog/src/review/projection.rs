//! Shared `SQLite` row projection and decoding for Review source consumers.
//!
//! Exact-source queries and paginated Review queries deliberately share this
//! codec so artifact, metadata, decision, and technical-observation provenance
//! cannot drift between the two read paths.

use shadow_domain::{AssetLocation, PhotoId, Platform, RawMetadataSnapshot, RepresentationId};

use crate::{
    CachedArtifact, CachedArtifactRecord, CatalogError, RepresentationFingerprint,
    TechnicalObservationRevision, TechnicalObservationSummary,
    cache_artifact::{
        digest, non_negative_u16, non_negative_u32, non_negative_u64, optional_usize,
        parse_byte_order, parse_codec, parse_role,
    },
    decision::photo_decision_state_from_columns,
    row_codec::read_id,
    technical_observation::decode_observation,
};

use super::ReviewItemRecord;

#[derive(Debug)]
pub(super) struct StoredArtifact {
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
pub(super) struct StoredPhotoInspection {
    pub(super) photo_id: PhotoId,
    pub(super) representation_id: RepresentationId,
    platform: String,
    native_path: Vec<u8>,
    display_path: String,
    source: RepresentationFingerprint,
    artifact: Option<StoredArtifact>,
    metadata_json: Option<String>,
    technical: Option<StoredTechnicalObservation>,
}

#[derive(Debug)]
pub(super) struct StoredReviewItem {
    inspection: StoredPhotoInspection,
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

#[derive(Debug)]
pub(super) struct DecodedPhotoInspection {
    pub(super) photo_id: PhotoId,
    pub(super) representation_id: RepresentationId,
    pub(super) location: AssetLocation,
    pub(super) source: RepresentationFingerprint,
    pub(super) visual: Option<CachedArtifactRecord>,
    pub(super) metadata: Option<RawMetadataSnapshot>,
    pub(super) technical: Option<TechnicalObservationSummary>,
}

pub(super) const SOURCE_PROJECTION_SELECT: &str =
    "SELECT r.photo_id, r.id, l.platform, l.native_path, l.display_path,
            r.byte_len, r.modified_at_ms,
            a.role, a.variant_key, a.generator_id, a.generator_version,
            a.recipe_snapshot_digest, a.provider_preview_id, a.blob_algorithm,
            a.blob_digest, a.blob_byte_len, a.codec, a.byte_order, a.width,
            a.height, a.bits_per_channel, a.channels, a.created_at_ms,
            t.observation_json, t.observation_digest,
            s.snapshot_json";

/// Shared exact-source joins for low-frequency source/inspection reads.
///
/// Parameter identities are fixed so Review source queries and the dedicated
/// selected-photo query cannot drift in artifact or technical provenance:
/// `?1` photo, `?2..?5` technical revision.
pub(super) const SOURCE_PROJECTION_FROM: &str = "FROM representations r
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
     )";

#[derive(serde::Deserialize)]
struct MetadataEnvelope {
    metadata: RawMetadataSnapshot,
}

pub(super) fn decode_review_metadata(json: &str) -> Option<RawMetadataSnapshot> {
    // The Review index consumes only metadata. Decoder snapshots also contain
    // provider capabilities, preview descriptors, and RAW-development
    // contracts which evolve independently. Deserializing the complete
    // `DecoderSnapshot` here made an unrelated capability rename hide valid
    // EXIF from the Library until every source had been re-inspected.
    serde_json::from_str::<MetadataEnvelope>(json)
        .ok()
        .map(|envelope| envelope.metadata)
}

pub(super) fn review_item_from_stored(
    stored: StoredReviewItem,
    revision: Option<&TechnicalObservationRevision>,
) -> Result<ReviewItemRecord, CatalogError> {
    let StoredReviewItem {
        inspection,
        decision_head_sequence,
        decision_flag,
        decision_rating,
        has_development_edits,
    } = stored;
    let DecodedPhotoInspection {
        photo_id,
        representation_id,
        location,
        source,
        visual,
        metadata,
        technical,
    } = decode_photo_inspection(inspection, revision)?;
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

pub(super) fn decode_photo_inspection(
    stored: StoredPhotoInspection,
    revision: Option<&TechnicalObservationRevision>,
) -> Result<DecodedPhotoInspection, CatalogError> {
    let StoredPhotoInspection {
        photo_id,
        representation_id,
        platform,
        native_path,
        display_path,
        source,
        artifact,
        metadata_json,
        technical,
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
    Ok(DecodedPhotoInspection {
        photo_id,
        representation_id,
        location,
        source,
        visual,
        metadata,
        technical,
    })
}

pub(super) fn read_stored_photo_inspection(
    row: &rusqlite::Row<'_>,
) -> rusqlite::Result<StoredPhotoInspection> {
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
    Ok(StoredPhotoInspection {
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
    })
}

pub(super) fn read_review_item(row: &rusqlite::Row<'_>) -> rusqlite::Result<StoredReviewItem> {
    Ok(StoredReviewItem {
        inspection: read_stored_photo_inspection(row)?,
        decision_head_sequence: row.get(26)?,
        decision_flag: row.get(27)?,
        decision_rating: row.get(28)?,
        has_development_edits: row.get(29)?,
    })
}

pub(super) fn supported_revision(
    revision: Option<&TechnicalObservationRevision>,
) -> Option<&TechnicalObservationRevision> {
    revision.filter(|revision| revision.is_supported_by_this_build())
}

pub(super) fn review_item_count(connection: &rusqlite::Connection) -> Result<u64, CatalogError> {
    let count = connection.query_row(
        "SELECT COUNT(*)
         FROM photos p
         WHERE EXISTS (
             SELECT 1 FROM representations r
             WHERE r.photo_id = p.id
               AND r.kind IN ('original_raw', 'original_raster')
               AND EXISTS (
                   SELECT 1 FROM locations l
                   WHERE l.representation_id = r.id AND l.status = 'online'
               )
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
