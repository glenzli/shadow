use std::collections::BTreeSet;

use serde::{Deserialize, Deserializer, Serialize, de::Error as _};
use thiserror::Error;

use crate::{
    AdmittedModelIdentity, AiCapability, EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
    ExecutionRouteIdentity, PrivacyClass, ProviderExecutionClass, ProviderIdentity,
};

pub const REMOTE_PROVIDER_MANIFEST_SCHEMA_VERSION: u32 = 1;

/// Exact payload category that one service contract may receive.
///
/// RAW files, sensor mosaics, and scene-linear pixels are intentionally absent.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RemoteUploadScope {
    BoundedRenderedCrop,
    FullRenderedImage,
    Mask,
    StructuredMetadata,
    SearchText,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(
    rename_all = "snake_case",
    tag = "policy",
    content = "days",
    deny_unknown_fields
)]
pub enum RemoteDataRetention {
    NotRetained,
    BoundedDays(u16),
    Undisclosed,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RemoteTrainingUse {
    ProhibitedByContract,
    OptOutRequired,
    Permitted,
    Undisclosed,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RemoteOfflineBehavior {
    Unavailable,
}

/// Auditable service facts kept separate from local model package manifests.
///
/// This value contains no credential, account identity, mutable endpoint token,
/// or response body. `adapter_revision` is the stable application transport
/// protocol implementation identity, not a secret or availability flag.
#[derive(Debug, Clone, Eq, PartialEq, Serialize)]
pub struct RemoteProviderManifest {
    pub schema_version: u32,
    pub provider_id: String,
    pub adapter_revision: String,
    pub service_revision: String,
    pub model_id: String,
    pub model_revision: String,
    pub api_contract_revision: String,
    pub capabilities: BTreeSet<AiCapability>,
    pub upload_scopes: BTreeSet<RemoteUploadScope>,
    pub maximum_raster_edge: u32,
    pub maximum_request_bytes: u64,
    pub admitted_privacy: BTreeSet<PrivacyClass>,
    pub retention: RemoteDataRetention,
    pub training_use: RemoteTrainingUse,
    pub supports_idempotency_key: bool,
    pub supports_cancellation: bool,
    pub offline_behavior: RemoteOfflineBehavior,
    pub terms_revision: String,
    pub terms_url: String,
    pub privacy_url: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RemoteProviderManifestWire {
    schema_version: u32,
    provider_id: String,
    adapter_revision: String,
    service_revision: String,
    model_id: String,
    model_revision: String,
    api_contract_revision: String,
    #[serde(deserialize_with = "crate::wire_v1::set_16")]
    capabilities: BTreeSet<AiCapability>,
    #[serde(deserialize_with = "crate::wire_v1::set_16")]
    upload_scopes: BTreeSet<RemoteUploadScope>,
    maximum_raster_edge: u32,
    maximum_request_bytes: u64,
    #[serde(deserialize_with = "crate::wire_v1::set_16")]
    admitted_privacy: BTreeSet<PrivacyClass>,
    retention: RemoteDataRetention,
    training_use: RemoteTrainingUse,
    supports_idempotency_key: bool,
    supports_cancellation: bool,
    offline_behavior: RemoteOfflineBehavior,
    terms_revision: String,
    terms_url: String,
    privacy_url: String,
}

impl<'de> Deserialize<'de> for RemoteProviderManifest {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = RemoteProviderManifestWire::deserialize(deserializer)?;
        let manifest = Self {
            schema_version: wire.schema_version,
            provider_id: wire.provider_id,
            adapter_revision: wire.adapter_revision,
            service_revision: wire.service_revision,
            model_id: wire.model_id,
            model_revision: wire.model_revision,
            api_contract_revision: wire.api_contract_revision,
            capabilities: wire.capabilities,
            upload_scopes: wire.upload_scopes,
            maximum_raster_edge: wire.maximum_raster_edge,
            maximum_request_bytes: wire.maximum_request_bytes,
            admitted_privacy: wire.admitted_privacy,
            retention: wire.retention,
            training_use: wire.training_use,
            supports_idempotency_key: wire.supports_idempotency_key,
            supports_cancellation: wire.supports_cancellation,
            offline_behavior: wire.offline_behavior,
            terms_revision: wire.terms_revision,
            terms_url: wire.terms_url,
            privacy_url: wire.privacy_url,
        };
        manifest.validate().map_err(D::Error::custom)?;
        Ok(manifest)
    }
}

impl RemoteProviderManifest {
    /// Validates exact-schema service facts without claiming current reachability
    /// or user approval.
    ///
    /// # Errors
    ///
    /// Returns an error when the schema, execution identity, declared bounds,
    /// capabilities, privacy classes, or legal-policy facts are incomplete.
    pub fn validate(&self) -> Result<(), RemoteProviderManifestError> {
        if self.schema_version != REMOTE_PROVIDER_MANIFEST_SCHEMA_VERSION {
            return Err(RemoteProviderManifestError::UnsupportedSchemaVersion(
                self.schema_version,
            ));
        }
        for (field, value) in [
            ("provider_id", self.provider_id.as_str()),
            ("adapter_revision", self.adapter_revision.as_str()),
            ("service_revision", self.service_revision.as_str()),
            ("model_id", self.model_id.as_str()),
            ("model_revision", self.model_revision.as_str()),
            ("api_contract_revision", self.api_contract_revision.as_str()),
            ("terms_revision", self.terms_revision.as_str()),
            ("terms_url", self.terms_url.as_str()),
            ("privacy_url", self.privacy_url.as_str()),
        ] {
            if value.trim().is_empty() {
                return Err(RemoteProviderManifestError::MissingText(field));
            }
        }
        if self.capabilities.is_empty() {
            return Err(RemoteProviderManifestError::MissingCapabilities);
        }
        if self.upload_scopes.is_empty() {
            return Err(RemoteProviderManifestError::MissingUploadScopes);
        }
        if self.admitted_privacy.is_empty() {
            return Err(RemoteProviderManifestError::MissingPrivacyClasses);
        }
        let uploads_pixels = self.upload_scopes.iter().any(|scope| {
            matches!(
                scope,
                RemoteUploadScope::BoundedRenderedCrop
                    | RemoteUploadScope::FullRenderedImage
                    | RemoteUploadScope::Mask
            )
        });
        if uploads_pixels && self.maximum_raster_edge == 0 {
            return Err(RemoteProviderManifestError::MissingMaximumRasterEdge);
        }
        if self.maximum_request_bytes == 0 {
            return Err(RemoteProviderManifestError::MissingMaximumRequestBytes);
        }
        if self.retention == RemoteDataRetention::BoundedDays(0) {
            return Err(RemoteProviderManifestError::InvalidRetentionDays);
        }
        self.execution_route()
            .validate()
            .map_err(|_| RemoteProviderManifestError::InvalidExecutionRoute)?;
        Ok(())
    }

    pub fn execution_route(&self) -> ExecutionRouteIdentity {
        ExecutionRouteIdentity {
            contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
            provider: ProviderIdentity {
                provider_id: self.provider_id.clone(),
                adapter_revision: self.adapter_revision.clone(),
                execution_class: ProviderExecutionClass::RemoteService,
            },
            model: AdmittedModelIdentity::RemoteService {
                service_revision: self.service_revision.clone(),
                model_id: self.model_id.clone(),
                model_revision: self.model_revision.clone(),
                api_contract_revision: self.api_contract_revision.clone(),
            },
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Error)]
pub enum RemoteProviderManifestError {
    #[error("remote manifest schema version {0} is unsupported")]
    UnsupportedSchemaVersion(u32),
    #[error("remote manifest field {0} must not be empty")]
    MissingText(&'static str),
    #[error("remote manifest must declare at least one capability")]
    MissingCapabilities,
    #[error("remote manifest must declare at least one upload scope")]
    MissingUploadScopes,
    #[error("remote manifest must declare at least one admitted privacy class")]
    MissingPrivacyClasses,
    #[error("remote pixel uploads require a non-zero maximum raster edge")]
    MissingMaximumRasterEdge,
    #[error("remote provider must declare a non-zero request byte bound")]
    MissingMaximumRequestBytes,
    #[error("bounded remote retention must be at least one day")]
    InvalidRetentionDays,
    #[error("remote provider execution route is incomplete")]
    InvalidExecutionRoute,
}
