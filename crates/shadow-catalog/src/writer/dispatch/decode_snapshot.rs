//! Actor-side execution for decoder observations and output freshness.

use crate::Catalog;

use super::super::protocol::DecodeSnapshotMessage;

pub(super) fn run_decode_snapshot_message(catalog: &mut Catalog, message: DecodeSnapshotMessage) {
    match message {
        DecodeSnapshotMessage::Record(request, response) => {
            let _ = response.send(catalog.record_decode_snapshot(request.as_ref()));
        }
        DecodeSnapshotMessage::Snapshots(representation_id, response) => {
            let _ = response.send(catalog.decode_snapshots(representation_id));
        }
        DecodeSnapshotMessage::IsOutputCurrent {
            representation_id,
            provider_id,
            provider_version,
            source,
            require_cached_preview,
            proxy_variant_key,
            required_technical_preprocessing,
            response,
        } => {
            let _ = response.send(catalog.is_decode_output_current(
                representation_id,
                &provider_id,
                &provider_version,
                source,
                require_cached_preview,
                &proxy_variant_key,
                required_technical_preprocessing.as_deref(),
            ));
        }
    }
}
