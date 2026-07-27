//! Completion policy for provider snapshots, cached visuals, and technical backfill.

use rusqlite::{OptionalExtension, params};
use shadow_domain::{EntityId, RepresentationId};

use crate::{Catalog, CatalogError, TechnicalObservationRevision};

use super::snapshot_store::{RepresentationFingerprint, sqlite_u64};

impl Catalog {
    /// Reports whether a provider's required decode output is current for this
    /// exact source revision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the representation is absent or the query
    /// fails.
    #[allow(clippy::too_many_arguments)]
    pub fn is_decode_output_current(
        &self,
        representation_id: RepresentationId,
        provider_id: &str,
        provider_version: &str,
        source: RepresentationFingerprint,
        require_cached_preview: bool,
        proxy_variant_key: &str,
        required_technical_preprocessing: Option<&str>,
    ) -> Result<bool, CatalogError> {
        if self.representation_fingerprint(representation_id)? != source {
            return Ok(false);
        }
        let byte_len = sqlite_u64(source.byte_len, "source_byte_len")?;
        let current: Option<(i64, i64, i64, i64)> = self
            .connection
            .query_row(
                "SELECT s.has_embedded_previews,
                        s.can_render_reference_rgb,
                        EXISTS(
                            SELECT 1 FROM representation_cached_artifacts a
                            WHERE a.representation_id = s.representation_id
                              AND a.role = 'embedded_preview'
                              AND a.variant_key = s.provider_id
                              AND a.generator_id = s.provider_id
                              AND a.generator_version = s.provider_version
                              AND a.source_byte_len = s.source_byte_len
                              AND a.source_modified_at_ms IS s.source_modified_at_ms
                        ),
                        EXISTS(
                            SELECT 1 FROM representation_cached_artifacts a
                            WHERE a.representation_id = s.representation_id
                              AND a.role = 'generated_proxy'
                              AND a.variant_key = ?6
                              AND a.generator_id = s.provider_id
                              AND a.generator_version = s.provider_version
                              AND a.source_byte_len = s.source_byte_len
                              AND a.source_modified_at_ms IS s.source_modified_at_ms
                        )
                 FROM representation_decode_snapshots s
                 WHERE s.representation_id = ?1 AND s.provider_id = ?2
                   AND s.provider_version = ?3
                   AND s.source_byte_len = ?4
                   AND s.source_modified_at_ms IS ?5",
                params![
                    representation_id.as_bytes().as_slice(),
                    provider_id,
                    provider_version,
                    byte_len,
                    source.modified_at_ms,
                    proxy_variant_key,
                ],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
            )
            .optional()?;
        let output_current =
            current.is_some_and(|(has_preview, can_render, cached_preview, cached_proxy)| {
                !require_cached_preview
                    || if can_render != 0 {
                        // Embedded previews are immediate placeholders. Once
                        // the provider can develop reference RGB, the durable
                        // Review contract is complete only after the current
                        // Shadow proxy exists.
                        cached_proxy != 0
                    } else if has_preview != 0 {
                        cached_preview != 0
                    } else {
                        true
                    }
            });
        if !output_current {
            return Ok(false);
        }
        let Some(preprocessing_version) = required_technical_preprocessing else {
            return Ok(true);
        };
        preferred_visual_has_current_technical_observation(
            self,
            representation_id,
            source,
            &TechnicalObservationRevision::current(preprocessing_version),
        )
    }
}

fn preferred_visual_has_current_technical_observation(
    catalog: &Catalog,
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    revision: &TechnicalObservationRevision,
) -> Result<bool, CatalogError> {
    if !revision.is_supported_by_this_build() {
        return Ok(false);
    }
    let preferred = catalog.preferred_cached_artifact(representation_id)?;
    Ok(match preferred {
        // No supported visual means there is nothing for this observer to do.
        None => true,
        Some(record) if record.artifact.codec != shadow_domain::PreviewCodec::Jpeg => true,
        Some(record) if record.source != source => false,
        Some(record) => match catalog.technical_observation(
            representation_id,
            source,
            &record.artifact,
            revision,
        ) {
            Ok(value) => value.is_some(),
            Err(
                CatalogError::InvalidPersistedTechnicalObservation(_)
                | CatalogError::TechnicalObservationJson(_),
            ) => false,
            Err(error) => return Err(error),
        },
    })
}
