//! Stable keyset pagination for the Review grid.

use shadow_domain::EntityId;

use crate::{CachedArtifactGeneratorIdentity, Catalog, CatalogError, TechnicalObservationRevision};

use super::{
    ReviewCursor, ReviewPageRecord,
    projection::{
        read_review_item, review_item_count, review_item_from_stored, supported_revision,
    },
};

const MAX_REVIEW_PAGE_SIZE: usize = 512;

impl Catalog {
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
