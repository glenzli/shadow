//! Source-guarded cache-reference publication and exact-row invalidation.

use rusqlite::params;
use shadow_domain::EntityId;

use crate::{Catalog, CatalogError, decode_snapshot::representation_fingerprint_in_transaction};

use super::{
    CachedArtifact, CachedArtifactRecord, CachedArtifactRole, InvalidateCachedArtifactStatus,
    RecordCachedArtifact, RecordCachedArtifactStatus,
    codec::{sqlite_u64, sqlite_usize},
};

impl Catalog {
    /// Atomically records a reference to one already-written cache blob.
    ///
    /// The cache file is rebuildable and lives outside `SQLite`. This method only
    /// commits the content identity and image semantics when the representation
    /// still matches the source revision used to produce it.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for invalid metadata, a missing representation,
    /// integer overflow, or a failed transaction.
    pub fn record_cached_artifact(
        &mut self,
        request: &RecordCachedArtifact,
    ) -> Result<RecordCachedArtifactStatus, CatalogError> {
        validate_artifact(&request.artifact)?;
        let transaction = self.connection.transaction()?;
        let current =
            representation_fingerprint_in_transaction(&transaction, request.representation_id)?;
        if current != request.expected_source {
            return Ok(RecordCachedArtifactStatus::StaleSource);
        }

        let artifact = &request.artifact;
        transaction.execute(
            "INSERT INTO representation_cached_artifacts(
                 representation_id, role, variant_key, generator_id, generator_version,
                 recipe_snapshot_digest, provider_preview_id, source_byte_len,
                 source_modified_at_ms, blob_algorithm, blob_digest, blob_byte_len, codec,
                 byte_order, width, height, bits_per_channel, channels, created_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13,
                       ?14, ?15, ?16, ?17, ?18, ?19)
             ON CONFLICT(representation_id, role, variant_key) DO UPDATE SET
                 generator_id = excluded.generator_id,
                 generator_version = excluded.generator_version,
                 recipe_snapshot_digest = excluded.recipe_snapshot_digest,
                 provider_preview_id = excluded.provider_preview_id,
                 source_byte_len = excluded.source_byte_len,
                 source_modified_at_ms = excluded.source_modified_at_ms,
                 blob_algorithm = excluded.blob_algorithm,
                 blob_digest = excluded.blob_digest,
                 blob_byte_len = excluded.blob_byte_len,
                 codec = excluded.codec,
                 byte_order = excluded.byte_order,
                 width = excluded.width,
                 height = excluded.height,
                 bits_per_channel = excluded.bits_per_channel,
                 channels = excluded.channels,
                 created_at_ms = excluded.created_at_ms",
            params![
                request.representation_id.as_bytes().as_slice(),
                artifact.role.as_str(),
                artifact.variant_key,
                artifact.generator_id,
                artifact.generator_version,
                artifact
                    .recipe_snapshot_digest
                    .as_ref()
                    .map(<[u8; 32]>::as_slice),
                artifact
                    .provider_preview_id
                    .map(|value| sqlite_usize(value, "provider_preview_id"))
                    .transpose()?,
                sqlite_u64(request.expected_source.byte_len, "source_byte_len")?,
                request.expected_source.modified_at_ms,
                artifact.blob_algorithm,
                artifact.blob_digest.as_slice(),
                sqlite_u64(artifact.blob_byte_len, "blob_byte_len")?,
                artifact.codec.as_str(),
                artifact.byte_order.as_str(),
                i64::from(artifact.dimensions.width),
                i64::from(artifact.dimensions.height),
                i64::from(artifact.bits_per_channel),
                i64::from(artifact.channels),
                artifact.created_at_ms,
            ],
        )?;
        transaction.commit()?;
        Ok(RecordCachedArtifactStatus::Recorded)
    }

    /// Removes an artifact reference only if the Catalog row is still exactly
    /// the record observed by a failed cache read.
    ///
    /// The full provenance, source fingerprint, blob identity, and creation
    /// timestamp prevent a slow reader from deleting a replacement committed by
    /// another worker after that reader loaded its record.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for numeric overflow or a failed delete.
    pub fn invalidate_cached_artifact(
        &mut self,
        record: &CachedArtifactRecord,
    ) -> Result<InvalidateCachedArtifactStatus, CatalogError> {
        let artifact = &record.artifact;
        let deleted = self.connection.execute(
            "DELETE FROM representation_cached_artifacts
             WHERE representation_id = ?1 AND role = ?2 AND variant_key = ?3
               AND generator_id = ?4 AND generator_version = ?5
               AND recipe_snapshot_digest IS ?6
               AND source_byte_len = ?7 AND source_modified_at_ms IS ?8
               AND blob_algorithm = ?9 AND blob_digest = ?10 AND blob_byte_len = ?11
               AND created_at_ms = ?12",
            params![
                record.representation_id.as_bytes().as_slice(),
                artifact.role.as_str(),
                artifact.variant_key,
                artifact.generator_id,
                artifact.generator_version,
                artifact
                    .recipe_snapshot_digest
                    .as_ref()
                    .map(<[u8; 32]>::as_slice),
                sqlite_u64(record.source.byte_len, "source_byte_len")?,
                record.source.modified_at_ms,
                artifact.blob_algorithm,
                artifact.blob_digest.as_slice(),
                sqlite_u64(artifact.blob_byte_len, "blob_byte_len")?,
                artifact.created_at_ms,
            ],
        )?;
        Ok(if deleted == 1 {
            InvalidateCachedArtifactStatus::Invalidated
        } else {
            InvalidateCachedArtifactStatus::NotCurrent
        })
    }
}

fn validate_artifact(artifact: &CachedArtifact) -> Result<(), CatalogError> {
    if artifact.variant_key.is_empty() {
        return Err(CatalogError::InvalidCachedArtifact(
            "variant key must not be empty",
        ));
    }
    if artifact.generator_id.is_empty() {
        return Err(CatalogError::InvalidCachedArtifact(
            "generator id must not be empty",
        ));
    }
    if artifact.blob_algorithm.is_empty() || artifact.blob_byte_len == 0 {
        return Err(CatalogError::InvalidCachedArtifact(
            "blob identity and length must be present",
        ));
    }
    match (artifact.role, artifact.recipe_snapshot_digest.is_some()) {
        (CachedArtifactRole::RecipePreview, true)
        | (CachedArtifactRole::EmbeddedPreview | CachedArtifactRole::GeneratedProxy, false) => {}
        (CachedArtifactRole::RecipePreview, false) => {
            return Err(CatalogError::InvalidCachedArtifact(
                "recipe preview must name an exact Recipe snapshot",
            ));
        }
        (CachedArtifactRole::EmbeddedPreview | CachedArtifactRole::GeneratedProxy, true) => {
            return Err(CatalogError::InvalidCachedArtifact(
                "source preview must not carry a Recipe snapshot",
            ));
        }
    }
    Ok(())
}
