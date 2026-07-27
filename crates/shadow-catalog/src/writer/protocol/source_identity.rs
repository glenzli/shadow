//! Actor messages for asset registration and path-independent source identity.

use std::sync::mpsc::SyncSender;

use shadow_domain::RepresentationId;

use crate::{
    CatalogError, ContentIdentity, RecordRepresentationContentIdentity,
    RecordRepresentationContentIdentityStatus, RegisterAsset, RegisteredAsset, RelinkMatch,
    RepresentationFingerprint,
};

pub(in crate::writer) enum SourceIdentityMessage {
    RegisterAsset(
        RegisterAsset,
        SyncSender<Result<RegisteredAsset, CatalogError>>,
    ),
    RegisterAssetWithContentIdentity(
        RegisterAsset,
        ContentIdentity,
        SyncSender<Result<RegisteredAsset, CatalogError>>,
    ),
    RecordContentIdentity(
        Box<RecordRepresentationContentIdentity>,
        SyncSender<Result<RecordRepresentationContentIdentityStatus, CatalogError>>,
    ),
    RelinkMatch(
        ContentIdentity,
        SyncSender<Result<Option<RelinkMatch>, CatalogError>>,
    ),
    Fingerprint(
        RepresentationId,
        SyncSender<Result<RepresentationFingerprint, CatalogError>>,
    ),
}
