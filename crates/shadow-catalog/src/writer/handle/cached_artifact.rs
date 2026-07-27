//! Client adapters for content-addressed visual references and reachability.

use shadow_domain::RepresentationId;

use crate::{
    CachedArtifactRecord, CatalogError, InvalidateCachedArtifactStatus, LiveCachedArtifactBlob,
    RecordCachedArtifact, RecordCachedArtifactStatus,
};

use super::super::{
    CatalogHandle,
    protocol::{CachedArtifactMessage, Message},
};

impl CatalogHandle {
    /// Records a content-addressed cached artifact through the catalog writer.
    ///
    /// Source races are returned as a normal status.
    pub fn record_cached_artifact(
        &self,
        request: &RecordCachedArtifact,
    ) -> Result<RecordCachedArtifactStatus, CatalogError> {
        self.request(|response| {
            Message::CachedArtifact(CachedArtifactMessage::Record(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Returns cached preview/proxy references for a representation.
    pub fn cached_artifacts(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Vec<CachedArtifactRecord>, CatalogError> {
        self.request(|response| {
            Message::CachedArtifact(CachedArtifactMessage::Artifacts(
                representation_id,
                response,
            ))
        })
    }

    /// Returns the shared Review-selected current visual through the Catalog
    /// writer so analysis and UI use one artifact-selection contract.
    pub fn preferred_cached_artifact(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Option<CachedArtifactRecord>, CatalogError> {
        self.request(|response| {
            Message::CachedArtifact(CachedArtifactMessage::Preferred(
                representation_id,
                response,
            ))
        })
    }

    /// Selects current cache artifacts for one bounded Library page. Returned
    /// slots preserve request order, including duplicate ids.
    pub fn preferred_cached_artifacts(
        &self,
        representation_ids: &[RepresentationId],
    ) -> Result<Vec<Option<CachedArtifactRecord>>, CatalogError> {
        self.request(|response| {
            Message::CachedArtifact(CachedArtifactMessage::PreferredBatch(
                representation_ids.to_vec(),
                response,
            ))
        })
    }

    /// Lists every cache blob still reachable from a current source/Recipe.
    pub fn live_cached_artifact_blobs(&self) -> Result<Vec<LiveCachedArtifactBlob>, CatalogError> {
        self.request(|response| Message::CachedArtifact(CachedArtifactMessage::LiveBlobs(response)))
    }

    /// Invalidates an exact cache reference through the single Catalog writer.
    pub fn invalidate_cached_artifact(
        &self,
        record: &CachedArtifactRecord,
    ) -> Result<InvalidateCachedArtifactStatus, CatalogError> {
        self.request(|response| {
            Message::CachedArtifact(CachedArtifactMessage::Invalidate(
                Box::new(record.clone()),
                response,
            ))
        })
    }
}
