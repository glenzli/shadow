//! Client adapters for decoder observations and output-freshness policy.

use shadow_domain::RepresentationId;

use crate::{
    CatalogError, DecodeSnapshotRecord, RecordDecodeSnapshot, RecordDecodeSnapshotStatus,
    RepresentationFingerprint,
};

use super::super::{
    CatalogHandle,
    protocol::{DecodeSnapshotMessage, Message},
};

impl CatalogHandle {
    /// Records one decoder snapshot through the single catalog writer.
    ///
    /// A normal source race is reported in the returned status.
    pub fn record_decode_snapshot(
        &self,
        request: &RecordDecodeSnapshot,
    ) -> Result<RecordDecodeSnapshotStatus, CatalogError> {
        self.request(|response| {
            Message::DecodeSnapshot(DecodeSnapshotMessage::Record(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Returns all current provider snapshots for a representation.
    pub fn decode_snapshots(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Vec<DecodeSnapshotRecord>, CatalogError> {
        self.request(|response| {
            Message::DecodeSnapshot(DecodeSnapshotMessage::Snapshots(
                representation_id,
                response,
            ))
        })
    }

    /// Reports whether a provider's required output is current for a source.
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
        self.request(|response| {
            Message::DecodeSnapshot(DecodeSnapshotMessage::IsOutputCurrent {
                representation_id,
                provider_id: provider_id.to_owned(),
                provider_version: provider_version.to_owned(),
                source,
                require_cached_preview,
                proxy_variant_key: proxy_variant_key.to_owned(),
                required_technical_preprocessing: required_technical_preprocessing
                    .map(str::to_owned),
                response,
            })
        })
    }
}
