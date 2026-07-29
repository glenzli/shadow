use std::collections::BTreeSet;

use crate::{
    AiCapability, PrivacyClass,
    remote::{
        REMOTE_PROVIDER_MANIFEST_SCHEMA_VERSION, RemoteDataRetention, RemoteOfflineBehavior,
        RemoteProviderManifest, RemoteTrainingUse, RemoteUploadScope,
    },
};

pub(super) fn manifest() -> RemoteProviderManifest {
    RemoteProviderManifest {
        schema_version: REMOTE_PROVIDER_MANIFEST_SCHEMA_VERSION,
        provider_id: "example.fill".into(),
        adapter_revision: "shadow-remote-fill-adapter-v1".into(),
        service_revision: "2026-07".into(),
        model_id: "fill".into(),
        model_revision: "v1".into(),
        api_contract_revision: "v2".into(),
        capabilities: BTreeSet::from([AiCapability::InpaintPatch]),
        upload_scopes: BTreeSet::from([
            RemoteUploadScope::BoundedRenderedCrop,
            RemoteUploadScope::Mask,
        ]),
        maximum_raster_edge: 4096,
        maximum_request_bytes: 16 * 1024 * 1024,
        admitted_privacy: BTreeSet::from([PrivacyClass::Public, PrivacyClass::Personal]),
        retention: RemoteDataRetention::NotRetained,
        training_use: RemoteTrainingUse::ProhibitedByContract,
        supports_idempotency_key: true,
        supports_cancellation: true,
        offline_behavior: RemoteOfflineBehavior::Unavailable,
        terms_revision: "terms-2026-07".into(),
        terms_url: "https://example.invalid/terms".into(),
        privacy_url: "https://example.invalid/privacy".into(),
    }
}
