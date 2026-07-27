//! Actor messages for photo/source/visual Review read models.

use std::sync::mpsc::SyncSender;

use shadow_domain::PhotoId;

use crate::{
    CachedArtifactGeneratorIdentity, CatalogError, ReviewCursor, ReviewItemRecord,
    ReviewPageRecord, TechnicalObservationRevision,
};

pub(in crate::writer) enum ReviewProjectionMessage {
    Page {
        after: Option<ReviewCursor>,
        limit: usize,
        revision: Option<TechnicalObservationRevision>,
        recipe_preview_generator: Option<CachedArtifactGeneratorIdentity>,
        response: SyncSender<Result<ReviewPageRecord, CatalogError>>,
    },
    ReviewSource {
        photo_id: PhotoId,
        revision: Option<TechnicalObservationRevision>,
        response: SyncSender<Result<Option<ReviewItemRecord>, CatalogError>>,
    },
    PhotoSource(
        PhotoId,
        SyncSender<Result<Option<ReviewItemRecord>, CatalogError>>,
    ),
}
