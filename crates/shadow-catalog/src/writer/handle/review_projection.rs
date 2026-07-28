//! Client adapters for photo/source/visual Review read models.

use shadow_domain::{PhotoId, RepresentationId};

use crate::{
    CachedArtifactGeneratorIdentity, CatalogError, PhotoInspectionRecord, ReviewCursor,
    ReviewItemRecord, ReviewPageRecord, TechnicalObservationRevision,
};

use super::super::{
    CatalogHandle,
    protocol::{Message, ReviewProjectionMessage},
};

impl CatalogHandle {
    /// Returns one immutable, keyset-paginated Review-grid page.
    pub fn review_page(
        &self,
        after: Option<&ReviewCursor>,
        limit: usize,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.request(|response| {
            Message::ReviewProjection(ReviewProjectionMessage::Page {
                after: after.cloned(),
                limit,
                revision: None,
                recipe_preview_generator: None,
                response,
            })
        })
    }

    /// Returns a Review page with summaries for one exact technical revision.
    pub fn review_page_with_technical(
        &self,
        after: Option<&ReviewCursor>,
        limit: usize,
        revision: &TechnicalObservationRevision,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.request(|response| {
            Message::ReviewProjection(ReviewProjectionMessage::Page {
                after: after.cloned(),
                limit,
                revision: Some(revision.clone()),
                recipe_preview_generator: None,
                response,
            })
        })
    }

    /// Returns a Review page whose Recipe-preview candidates match one exact
    /// generator implementation identity.
    pub fn review_page_with_technical_and_recipe_preview_generator(
        &self,
        after: Option<&ReviewCursor>,
        limit: usize,
        revision: &TechnicalObservationRevision,
        recipe_preview_generator: &CachedArtifactGeneratorIdentity,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.request(|response| {
            Message::ReviewProjection(ReviewProjectionMessage::Page {
                after: after.cloned(),
                limit,
                revision: Some(revision.clone()),
                recipe_preview_generator: Some(recipe_preview_generator.clone()),
                response,
            })
        })
    }

    /// Returns the catalog-owned online RAW source for one photo.
    pub fn review_source(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.request(|response| {
            Message::ReviewProjection(ReviewProjectionMessage::ReviewSource {
                photo_id,
                revision: None,
                response,
            })
        })
    }

    /// Returns one Review source with an exact current technical summary.
    pub fn review_source_with_technical(
        &self,
        photo_id: PhotoId,
        revision: &TechnicalObservationRevision,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.request(|response| {
            Message::ReviewProjection(ReviewProjectionMessage::ReviewSource {
                photo_id,
                revision: Some(revision.clone()),
                response,
            })
        })
    }

    /// Returns the online original source for one photo, preferring RAW and
    /// falling back to an original raster.
    pub fn photo_source(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.request(|response| {
            Message::ReviewProjection(ReviewProjectionMessage::PhotoSource(photo_id, response))
        })
    }

    /// Returns inspection data for one exact selected photo representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the catalog actor rejects the request or
    /// the exact persisted inspection projection cannot be decoded.
    pub fn photo_inspection(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
        revision: &TechnicalObservationRevision,
    ) -> Result<Option<PhotoInspectionRecord>, CatalogError> {
        self.request(|response| {
            Message::ReviewProjection(ReviewProjectionMessage::PhotoInspection {
                photo_id,
                representation_id,
                revision: revision.clone(),
                response,
            })
        })
    }
}
