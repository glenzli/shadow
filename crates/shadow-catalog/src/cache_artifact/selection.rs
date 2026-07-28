//! Current artifact selection, reachability, and persisted-row projection.

use std::{cmp::Ordering, collections::HashMap};

use rusqlite::{params_from_iter, types::Value};
use shadow_domain::{EntityId, ImageDimensions, RepresentationId};

use crate::{Catalog, CatalogError, RepresentationFingerprint, row_codec::read_id};

use super::{
    CachedArtifact, CachedArtifactRecord, CachedArtifactRole, LiveCachedArtifactBlob,
    codec::{
        digest, non_negative_u16, non_negative_u32, non_negative_u64, optional_usize,
        parse_byte_order, parse_codec, parse_role,
    },
};

/// Library pages are capped at 512 records. Keeping the visual-selection
/// batch at the same bound prevents a single UI request from growing an
/// unbounded SQL `IN` clause, while still ensuring a grid page never falls
/// back to one Catalog request per thumbnail.
const MAX_PREFERRED_ARTIFACT_BATCH_SIZE: usize = 512;

/// Current source and working-recipe identity required to decide whether a
/// rebuildable artifact can still be presented.
#[derive(Debug, Copy, Clone)]
struct CurrentArtifactContext {
    source: RepresentationFingerprint,
    working_recipe_snapshot: Option<[u8; 32]>,
}

/// `SQLite` row form kept separate from [`CachedArtifactRecord`] so persisted
/// enum validation can retain the domain-specific [`CatalogError`] that the
/// single-item API has always returned.
#[derive(Debug)]
struct StoredCachedArtifactRow {
    representation_id: RepresentationId,
    role: String,
    variant_key: String,
    generator_id: String,
    generator_version: String,
    recipe_snapshot_digest: Option<[u8; 32]>,
    provider_preview_id: Option<usize>,
    source: RepresentationFingerprint,
    blob_algorithm: String,
    blob_digest: [u8; 32],
    blob_byte_len: u64,
    codec: String,
    byte_order: String,
    dimensions: ImageDimensions,
    bits_per_channel: u16,
    channels: u16,
    created_at_ms: i64,
}

impl Catalog {
    /// Lists cached artifacts for a representation in stable role/key order.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the representation is absent, persisted
    /// enum text is unknown, numeric data is invalid, or the query fails.
    pub fn cached_artifacts(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Vec<CachedArtifactRecord>, CatalogError> {
        self.representation_fingerprint(representation_id)?;
        let mut statement = self.connection.prepare(
            "SELECT representation_id, role, variant_key, generator_id, generator_version,
                    recipe_snapshot_digest, provider_preview_id, source_byte_len,
                    source_modified_at_ms, blob_algorithm, blob_digest, blob_byte_len, codec,
                    byte_order, width, height, bits_per_channel, channels, created_at_ms
             FROM representation_cached_artifacts
             WHERE representation_id = ?1
             ORDER BY role, variant_key",
        )?;
        let rows = statement.query_map(
            [representation_id.as_bytes().as_slice()],
            read_stored_cached_artifact,
        )?;
        let mut artifacts = Vec::new();
        for row in rows {
            artifacts.push(cached_artifact_record(row?)?);
        }
        Ok(artifacts)
    }

    /// Returns the exact current visual selected by the shared Review ordering.
    ///
    /// A current Recipe preview precedes Shadow-generated proxies, which
    /// precede camera-embedded placeholders. Within a role the largest and
    /// newest image wins, followed by the stable variant key. Keeping this
    /// selection in Catalog prevents background analysis and Review from
    /// targeting different provider/variant artifacts.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the representation is absent or persisted
    /// artifact metadata is invalid.
    pub fn preferred_cached_artifact(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Option<CachedArtifactRecord>, CatalogError> {
        // Keep the single-image API on the same selection path as Library
        // pages, so analysis, Review, and the photo-first grid cannot diverge
        // when cache validity rules evolve.
        self.preferred_cached_artifacts(&[representation_id])?
            .into_iter()
            .next()
            .ok_or_else(|| {
                CatalogError::InvalidLibraryQuery(
                    "cached artifact selection omitted its requested slot".into(),
                )
            })
    }

    /// Selects the current grid artifact for every requested representation in
    /// a bounded batch.
    ///
    /// This is intentionally not a general cache listing API. It performs the
    /// same source and working-Recipe validation as
    /// [`Self::preferred_cached_artifact`], but resolves all source identities,
    /// Recipe refs, and candidate artifacts in three bounded queries instead
    /// of turning one 512-photo Library page into 512 actor round-trips.
    /// Result order always matches `representation_ids`, including duplicates.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError::RepresentationNotFound`] if any requested id is
    /// absent, [`CatalogError::InvalidLibraryQuery`] for an oversized batch, or
    /// a catalog error if persisted artifact metadata is invalid.
    pub fn preferred_cached_artifacts(
        &self,
        representation_ids: &[RepresentationId],
    ) -> Result<Vec<Option<CachedArtifactRecord>>, CatalogError> {
        if representation_ids.is_empty() {
            return Ok(Vec::new());
        }
        if representation_ids.len() > MAX_PREFERRED_ARTIFACT_BATCH_SIZE {
            return Err(CatalogError::InvalidLibraryQuery(format!(
                "cached artifact selection batch {} exceeds {MAX_PREFERRED_ARTIFACT_BATCH_SIZE}",
                representation_ids.len()
            )));
        }
        let unique_ids = unique_representation_ids(representation_ids);
        let contexts = current_artifact_contexts(&self.connection, &unique_ids)?;
        for representation_id in &unique_ids {
            if !contexts.contains_key(representation_id) {
                return Err(CatalogError::RepresentationNotFound(*representation_id));
            }
        }
        let candidates = cached_artifacts_for_representations(&self.connection, &unique_ids)?;
        let mut selected = HashMap::with_capacity(unique_ids.len());
        for representation_id in unique_ids {
            let Some(context) = contexts.get(&representation_id) else {
                return Err(CatalogError::InvalidLibraryQuery(
                    "cached artifact context disappeared during selection".into(),
                ));
            };
            let mut current = candidates
                .get(&representation_id)
                .cloned()
                .unwrap_or_default();
            current.retain(|record| {
                record.source == context.source
                    && (record.artifact.role != CachedArtifactRole::RecipePreview
                        || record.artifact.recipe_snapshot_digest
                            == context.working_recipe_snapshot)
            });
            current.sort_by(preferred_artifact_ordering);
            selected.insert(representation_id, current.into_iter().next());
        }
        representation_ids
            .iter()
            .map(|representation_id| {
                selected.get(representation_id).cloned().ok_or_else(|| {
                    CatalogError::InvalidLibraryQuery(
                        "cached artifact selection omitted a requested representation".into(),
                    )
                })
            })
            .collect()
    }

    /// Lists the content-addressed cache blobs still reachable from current
    /// source identities and the current working Recipe refs.
    ///
    /// This is the Catalog half of conservative cache garbage collection. It
    /// deliberately retains every valid size variant, rather than only the
    /// current grid choice, so zoom/detail previews do not get silently
    /// discarded. Stale source revisions and old Recipe-preview revisions are
    /// excluded and may be reclaimed by the cache layer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when one persisted digest is malformed or the
    /// catalog query fails.
    pub fn live_cached_artifact_blobs(&self) -> Result<Vec<LiveCachedArtifactBlob>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT DISTINCT a.blob_algorithm, a.blob_digest
             FROM representation_cached_artifacts a
             JOIN representations r ON r.id = a.representation_id
             LEFT JOIN recipe_refs rr
               ON rr.photo_id = r.photo_id AND rr.name = 'working'
             LEFT JOIN recipe_commits c
               ON c.id = rr.commit_id AND c.photo_id = rr.photo_id
             WHERE a.source_byte_len = r.byte_len
               AND a.source_modified_at_ms IS r.modified_at_ms
               AND (
                    a.role != 'recipe_preview'
                    OR a.recipe_snapshot_digest = c.snapshot_digest
               )
             ORDER BY a.blob_algorithm, a.blob_digest",
        )?;
        let rows = statement.query_map([], |row| {
            Ok(LiveCachedArtifactBlob {
                algorithm: row.get(0)?,
                digest: digest(row.get(1)?, 1)?,
            })
        })?;
        rows.collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }
}

fn unique_representation_ids(representation_ids: &[RepresentationId]) -> Vec<RepresentationId> {
    let mut unique = Vec::with_capacity(representation_ids.len());
    for representation_id in representation_ids {
        if !unique.contains(representation_id) {
            unique.push(*representation_id);
        }
    }
    unique
}

fn current_artifact_contexts(
    connection: &rusqlite::Connection,
    representation_ids: &[RepresentationId],
) -> Result<HashMap<RepresentationId, CurrentArtifactContext>, CatalogError> {
    let (placeholders, values) = representation_id_query_parts(representation_ids);
    let sql = format!(
        "SELECT r.id, r.byte_len, r.modified_at_ms, c.snapshot_digest
         FROM representations r
         LEFT JOIN recipe_refs rr
           ON rr.photo_id = r.photo_id AND rr.name = 'working'
         LEFT JOIN recipe_commits c
           ON c.id = rr.commit_id AND c.photo_id = rr.photo_id
         WHERE r.id IN ({placeholders})"
    );
    let mut statement = connection.prepare(&sql)?;
    let rows = statement.query_map(params_from_iter(values.iter()), |row| {
        let byte_len = non_negative_u64(row.get(1)?, 1)?;
        Ok((
            read_id(row, 0)?,
            CurrentArtifactContext {
                source: RepresentationFingerprint {
                    byte_len,
                    modified_at_ms: row.get(2)?,
                },
                working_recipe_snapshot: row
                    .get::<_, Option<Vec<u8>>>(3)?
                    .map(|value| digest(value, 3))
                    .transpose()?,
            },
        ))
    })?;
    let contexts = rows.collect::<rusqlite::Result<HashMap<_, _>>>()?;
    Ok(contexts)
}

fn cached_artifacts_for_representations(
    connection: &rusqlite::Connection,
    representation_ids: &[RepresentationId],
) -> Result<HashMap<RepresentationId, Vec<CachedArtifactRecord>>, CatalogError> {
    let (placeholders, values) = representation_id_query_parts(representation_ids);
    let sql = format!(
        "SELECT representation_id, role, variant_key, generator_id, generator_version,
                recipe_snapshot_digest, provider_preview_id, source_byte_len,
                source_modified_at_ms, blob_algorithm, blob_digest, blob_byte_len, codec,
                byte_order, width, height, bits_per_channel, channels, created_at_ms
         FROM representation_cached_artifacts
         WHERE representation_id IN ({placeholders})
         ORDER BY representation_id, role, variant_key"
    );
    let mut statement = connection.prepare(&sql)?;
    let rows = statement.query_map(params_from_iter(values.iter()), read_stored_cached_artifact)?;
    let mut grouped = HashMap::<RepresentationId, Vec<CachedArtifactRecord>>::new();
    for record in rows {
        let record = cached_artifact_record(record?)?;
        grouped
            .entry(record.representation_id)
            .or_default()
            .push(record);
    }
    Ok(grouped)
}

fn representation_id_query_parts(representation_ids: &[RepresentationId]) -> (String, Vec<Value>) {
    debug_assert!(!representation_ids.is_empty());
    let placeholders = (0..representation_ids.len())
        .map(|_| "?")
        .collect::<Vec<_>>()
        .join(", ");
    let values = representation_ids
        .iter()
        .map(|representation_id| Value::Blob(representation_id.as_bytes().to_vec()))
        .collect();
    (placeholders, values)
}

fn read_stored_cached_artifact(
    row: &rusqlite::Row<'_>,
) -> rusqlite::Result<StoredCachedArtifactRow> {
    Ok(StoredCachedArtifactRow {
        representation_id: read_id(row, 0)?,
        source: RepresentationFingerprint {
            byte_len: non_negative_u64(row.get(7)?, 7)?,
            modified_at_ms: row.get(8)?,
        },
        role: row.get(1)?,
        variant_key: row.get(2)?,
        generator_id: row.get(3)?,
        generator_version: row.get(4)?,
        recipe_snapshot_digest: row
            .get::<_, Option<Vec<u8>>>(5)?
            .map(|value| digest(value, 5))
            .transpose()?,
        provider_preview_id: optional_usize(row.get::<_, Option<i64>>(6)?, 6)?,
        blob_algorithm: row.get(9)?,
        blob_digest: digest(row.get(10)?, 10)?,
        blob_byte_len: non_negative_u64(row.get(11)?, 11)?,
        codec: row.get(12)?,
        byte_order: row.get(13)?,
        dimensions: ImageDimensions {
            width: non_negative_u32(row.get(14)?, 14)?,
            height: non_negative_u32(row.get(15)?, 15)?,
        },
        bits_per_channel: non_negative_u16(row.get(16)?, 16)?,
        channels: non_negative_u16(row.get(17)?, 17)?,
        created_at_ms: row.get(18)?,
    })
}

fn cached_artifact_record(
    stored: StoredCachedArtifactRow,
) -> Result<CachedArtifactRecord, CatalogError> {
    Ok(CachedArtifactRecord {
        representation_id: stored.representation_id,
        source: stored.source,
        artifact: CachedArtifact {
            role: parse_role(stored.role)?,
            variant_key: stored.variant_key,
            generator_id: stored.generator_id,
            generator_version: stored.generator_version,
            recipe_snapshot_digest: stored.recipe_snapshot_digest,
            provider_preview_id: stored.provider_preview_id,
            blob_algorithm: stored.blob_algorithm,
            blob_digest: stored.blob_digest,
            blob_byte_len: stored.blob_byte_len,
            codec: parse_codec(stored.codec)?,
            byte_order: parse_byte_order(stored.byte_order)?,
            dimensions: stored.dimensions,
            bits_per_channel: stored.bits_per_channel,
            channels: stored.channels,
            created_at_ms: stored.created_at_ms,
        },
    })
}

fn preferred_artifact_ordering(
    left: &CachedArtifactRecord,
    right: &CachedArtifactRecord,
) -> Ordering {
    artifact_role_rank(left.artifact.role)
        .cmp(&artifact_role_rank(right.artifact.role))
        .then_with(|| artifact_area(right).cmp(&artifact_area(left)))
        .then_with(|| {
            right
                .artifact
                .created_at_ms
                .cmp(&left.artifact.created_at_ms)
        })
        .then_with(|| left.artifact.variant_key.cmp(&right.artifact.variant_key))
}

const fn artifact_role_rank(role: CachedArtifactRole) -> u8 {
    match role {
        CachedArtifactRole::RecipePreview => 0,
        CachedArtifactRole::GeneratedProxy => 1,
        CachedArtifactRole::EmbeddedPreview => 2,
    }
}

fn artifact_area(record: &CachedArtifactRecord) -> u64 {
    u64::from(record.artifact.dimensions.width) * u64::from(record.artifact.dimensions.height)
}
