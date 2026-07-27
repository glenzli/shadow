//! Actor messages for decoder observations and output-freshness policy.

use std::sync::mpsc::SyncSender;

use shadow_domain::RepresentationId;

use crate::{
    CatalogError, DecodeSnapshotRecord, RecordDecodeSnapshot, RecordDecodeSnapshotStatus,
    RepresentationFingerprint,
};

pub(in crate::writer) enum DecodeSnapshotMessage {
    Record(
        Box<RecordDecodeSnapshot>,
        SyncSender<Result<RecordDecodeSnapshotStatus, CatalogError>>,
    ),
    Snapshots(
        RepresentationId,
        SyncSender<Result<Vec<DecodeSnapshotRecord>, CatalogError>>,
    ),
    IsOutputCurrent {
        representation_id: RepresentationId,
        provider_id: String,
        provider_version: String,
        source: RepresentationFingerprint,
        require_cached_preview: bool,
        proxy_variant_key: String,
        required_technical_preprocessing: Option<String>,
        response: SyncSender<Result<bool, CatalogError>>,
    },
}
