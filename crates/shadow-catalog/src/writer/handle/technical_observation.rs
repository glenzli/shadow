//! Client adapters for exact, revisioned visual-quality evidence.

use shadow_domain::RepresentationId;

use crate::{
    CachedArtifact, CatalogError, RecordTechnicalObservation, RecordTechnicalObservationStatus,
    RepresentationFingerprint, TechnicalObservationRecord, TechnicalObservationRevision,
};

use super::super::{
    CatalogHandle,
    protocol::{Message, TechnicalObservationMessage},
};

impl CatalogHandle {
    /// Records one display-luma observation through the single Catalog writer.
    ///
    /// A normal source/artifact race is returned in the status.
    pub fn record_technical_observation(
        &self,
        request: &RecordTechnicalObservation,
    ) -> Result<RecordTechnicalObservationStatus, CatalogError> {
        self.request(|response| {
            Message::TechnicalObservation(TechnicalObservationMessage::Record(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Reads one exact, current technical observation through the Catalog actor.
    pub fn technical_observation(
        &self,
        representation_id: RepresentationId,
        expected_source: RepresentationFingerprint,
        expected_artifact: &CachedArtifact,
        revision: &TechnicalObservationRevision,
    ) -> Result<Option<TechnicalObservationRecord>, CatalogError> {
        self.request(|response| {
            Message::TechnicalObservation(TechnicalObservationMessage::Read {
                representation_id,
                expected_source,
                expected_artifact: Box::new(expected_artifact.clone()),
                revision: revision.clone(),
                response,
            })
        })
    }
}
