//! Actor messages for content-addressed visual references and reachability.

use std::sync::mpsc::SyncSender;

use shadow_domain::RepresentationId;

use crate::{
    CachedArtifactRecord, CatalogError, InvalidateCachedArtifactStatus, LiveCachedArtifactBlob,
    RecordCachedArtifact, RecordCachedArtifactStatus,
};

pub(in crate::writer) enum CachedArtifactMessage {
    Record(
        Box<RecordCachedArtifact>,
        SyncSender<Result<RecordCachedArtifactStatus, CatalogError>>,
    ),
    Artifacts(
        RepresentationId,
        SyncSender<Result<Vec<CachedArtifactRecord>, CatalogError>>,
    ),
    Preferred(
        RepresentationId,
        SyncSender<Result<Option<CachedArtifactRecord>, CatalogError>>,
    ),
    PreferredBatch(
        Vec<RepresentationId>,
        SyncSender<Result<Vec<Option<CachedArtifactRecord>>, CatalogError>>,
    ),
    LiveBlobs(SyncSender<Result<Vec<LiveCachedArtifactBlob>, CatalogError>>),
    Invalidate(
        Box<CachedArtifactRecord>,
        SyncSender<Result<InvalidateCachedArtifactStatus, CatalogError>>,
    ),
}
