//! Actor-side execution for content-addressed visual references.

use crate::Catalog;

use super::super::protocol::CachedArtifactMessage;

pub(super) fn run_cached_artifact_message(catalog: &mut Catalog, message: CachedArtifactMessage) {
    match message {
        CachedArtifactMessage::Record(request, response) => {
            let _ = response.send(catalog.record_cached_artifact(request.as_ref()));
        }
        CachedArtifactMessage::Artifacts(representation_id, response) => {
            let _ = response.send(catalog.cached_artifacts(representation_id));
        }
        CachedArtifactMessage::Preferred(representation_id, response) => {
            let _ = response.send(catalog.preferred_cached_artifact(representation_id));
        }
        CachedArtifactMessage::PreferredBatch(representation_ids, response) => {
            let _ = response.send(catalog.preferred_cached_artifacts(&representation_ids));
        }
        CachedArtifactMessage::LiveBlobs(response) => {
            let _ = response.send(catalog.live_cached_artifact_blobs());
        }
        CachedArtifactMessage::Invalidate(record, response) => {
            let _ = response.send(catalog.invalidate_cached_artifact(record.as_ref()));
        }
    }
}
