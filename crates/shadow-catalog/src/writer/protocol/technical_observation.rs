//! Actor messages for exact, revisioned visual-quality evidence.

use std::sync::mpsc::SyncSender;

use shadow_domain::RepresentationId;

use crate::{
    CachedArtifact, CatalogError, RecordTechnicalObservation, RecordTechnicalObservationStatus,
    RepresentationFingerprint, TechnicalObservationRecord, TechnicalObservationRevision,
};

pub(in crate::writer) enum TechnicalObservationMessage {
    Record(
        Box<RecordTechnicalObservation>,
        SyncSender<Result<RecordTechnicalObservationStatus, CatalogError>>,
    ),
    Read {
        representation_id: RepresentationId,
        expected_source: RepresentationFingerprint,
        expected_artifact: Box<CachedArtifact>,
        revision: TechnicalObservationRevision,
        response: SyncSender<Result<Option<TechnicalObservationRecord>, CatalogError>>,
    },
}
