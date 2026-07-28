//! Exact photo-source selection and selected-representation inspection.

use rusqlite::OptionalExtension;
use shadow_domain::{EntityId, PhotoId, RepresentationId};

use crate::{Catalog, CatalogError, TechnicalObservationRevision};

use super::{
    PhotoInspectionRecord, ReviewItemRecord,
    projection::{
        DecodedPhotoInspection, SOURCE_PROJECTION_FROM, SOURCE_PROJECTION_SELECT,
        decode_photo_inspection, read_review_item, read_stored_photo_inspection,
        review_item_from_stored, supported_revision,
    },
};

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

    /// Returns inspection data for exactly one `{photo, representation}` pair.
    ///
    /// The representation must be an online original RAW or raster owned by
    /// `photo_id`. Unknown, mismatched, or offline identities return `None`;
    /// this method never substitutes another representation of the photo.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for unknown persisted values or a failed query.
    pub fn photo_inspection(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
        revision: &TechnicalObservationRevision,
    ) -> Result<Option<PhotoInspectionRecord>, CatalogError> {
        let revision = supported_revision(Some(revision));
        let sql = format!(
            "{SOURCE_PROJECTION_SELECT}
             {SOURCE_PROJECTION_FROM}
             WHERE r.photo_id = ?1
               AND r.id = ?6
               AND r.kind IN ('original_raw', 'original_raster')
             LIMIT 1"
        );
        let mut statement = self.connection.prepare(&sql)?;
        let stored = statement
            .query_row(
                rusqlite::params![
                    photo_id.as_bytes().as_slice(),
                    revision.map(|value| i64::from(value.observation_schema)),
                    revision.map(|value| value.implementation_version.as_str()),
                    revision.map(|value| i64::from(value.display_luma_contract_version)),
                    revision.map(|value| value.preprocessing_version.as_str()),
                    representation_id.as_bytes().as_slice(),
                ],
                read_stored_photo_inspection,
            )
            .optional()?;
        stored
            .map(|stored| decode_photo_inspection(stored, revision))
            .transpose()
            .map(|inspection| {
                inspection.map(|inspection| {
                    let DecodedPhotoInspection {
                        photo_id,
                        representation_id,
                        location,
                        source,
                        metadata,
                        technical,
                        ..
                    } = inspection;
                    PhotoInspectionRecord {
                        photo_id,
                        representation_id,
                        location,
                        source,
                        metadata,
                        technical,
                    }
                })
            })
    }

    fn source_inner(
        &self,
        photo_id: PhotoId,
        revision: Option<&TechnicalObservationRevision>,
        selection: SourceSelection,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        let revision = supported_revision(revision);
        let sql = format!(
            "{SOURCE_PROJECTION_SELECT},
                    dc.head_sequence, de.after_flag, de.after_rating,
                    EXISTS (
                        SELECT 1 FROM recipe_refs edit_ref
                        WHERE edit_ref.photo_id = r.photo_id
                          AND edit_ref.name = 'working'
                    )
             {SOURCE_PROJECTION_FROM}
             LEFT JOIN photo_decision_current dc ON dc.photo_id = r.photo_id
             LEFT JOIN photo_decision_events de
               ON de.sequence = dc.head_sequence AND de.photo_id = r.photo_id
             WHERE r.photo_id = ?1
               AND (r.kind = 'original_raw'
                    OR (?6 != 0 AND r.kind = 'original_raster'))
             ORDER BY CASE r.kind WHEN 'original_raw' THEN 0 ELSE 1 END,
                      r.created_at_ms, r.id
             LIMIT 1"
        );
        let mut statement = self.connection.prepare(&sql)?;
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
}
