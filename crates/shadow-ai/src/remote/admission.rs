use serde::{Deserialize, Serialize};

use crate::{AiCapability, AiJobRequest, InputRole, PrivacyClass};

use super::{PreparedRemoteUpload, RemoteProviderManifest, RemoteUploadScope};

mod grant;
mod identity;
mod validation;

pub use grant::{RemoteExecutionGrant, RemoteTransportExecution};

pub const REMOTE_EXECUTION_REQUEST_SCHEMA_VERSION: u32 = 1;
pub const MAX_REMOTE_INPUTS: u32 = 64;

/// Product policy selected by the application before any remote bytes are
/// prepared or transported.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RemoteExecutionPolicy {
    Disabled,
    PublicOnly,
    PersonalAllowed,
}

/// Fresh application policy and consent facts used for one admission.
///
/// Upload facts do not live here. They can enter admission only through
/// store-issued [`PreparedRemoteUpload`] receipts.
#[derive(Debug, Clone, Eq, PartialEq, Serialize)]
pub struct RemoteExecutionContext {
    pub schema_version: u32,
    pub network_available: bool,
    pub policy: RemoteExecutionPolicy,
    pub accepted_terms_revision: Option<String>,
    pub maximum_retention_days: u16,
    pub policy_revision: String,
    pub consent_receipt_id: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RemoteExecutionContextWire {
    schema_version: u32,
    network_available: bool,
    policy: RemoteExecutionPolicy,
    accepted_terms_revision: Option<String>,
    maximum_retention_days: u16,
    policy_revision: String,
    consent_receipt_id: String,
}

impl<'de> Deserialize<'de> for RemoteExecutionContext {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let wire = RemoteExecutionContextWire::deserialize(deserializer)?;
        if wire.schema_version != REMOTE_EXECUTION_REQUEST_SCHEMA_VERSION {
            return Err(serde::de::Error::custom(format_args!(
                "remote execution context version {} is unsupported",
                wire.schema_version
            )));
        }
        if wire.policy_revision.trim().is_empty() || wire.consent_receipt_id.trim().is_empty() {
            return Err(serde::de::Error::custom(
                "remote execution context requires policy and consent revisions",
            ));
        }
        Ok(Self {
            schema_version: wire.schema_version,
            network_available: wire.network_available,
            policy: wire.policy,
            accepted_terms_revision: wire.accepted_terms_revision,
            maximum_retention_days: wire.maximum_retention_days,
            policy_revision: wire.policy_revision,
            consent_receipt_id: wire.consent_receipt_id,
        })
    }
}

/// Admits one exact request and prepared outbound inventory.
///
/// # Errors
///
/// Returns all deterministic blockers when service facts, policy, consent,
/// request identity, input roles, upload receipts, or bounds fail closed.
pub fn admit_remote_execution(
    manifest: &RemoteProviderManifest,
    request: &AiJobRequest,
    context: &RemoteExecutionContext,
    uploads: Vec<PreparedRemoteUpload>,
) -> Result<RemoteExecutionGrant, RemoteExecutionDenied> {
    let uploads = validation::validate(manifest, request, context, uploads)
        .map_err(|blockers| RemoteExecutionDenied { blockers })?;
    let identity = identity::build(manifest, request, context, &uploads).map_err(|blocker| {
        RemoteExecutionDenied {
            blockers: vec![blocker],
        }
    })?;
    RemoteExecutionGrant::issue(request.clone(), uploads, identity).map_err(|blocker| {
        RemoteExecutionDenied {
            blockers: vec![blocker],
        }
    })
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RemoteExecutionDenied {
    #[serde(deserialize_with = "crate::wire_v1::vec_64")]
    pub blockers: Vec<RemoteProviderBlocker>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "reason", deny_unknown_fields)]
pub enum RemoteProviderBlocker {
    InvalidManifest,
    UnsupportedContextSchema {
        actual: u32,
    },
    UnsupportedRequestSchema {
        actual: u32,
    },
    MissingRequestId,
    TooManyInputs {
        actual: u32,
        maximum: u32,
    },
    InvalidTaskParameters,
    CapabilityUnsupported {
        capability: AiCapability,
    },
    Offline,
    MissingPolicyRevision,
    MissingConsentReceipt,
    ProviderIdempotencyUnsupported,
    PrivacyPolicyDenied,
    SensitiveBiometricDeferred,
    PrivacyClassUnsupported {
        privacy: PrivacyClass,
    },
    InputPrivacyUnsupported {
        input_index: u32,
        privacy: PrivacyClass,
    },
    RequestUnderstatesInputPrivacy {
        input_index: u32,
    },
    RetentionExceedsConsent {
        offered_days: u16,
        maximum_days: u16,
    },
    RetentionUndisclosed,
    TrainingUseNotProhibited,
    TermsAcceptanceRequired,
    InvalidInputIdentity {
        input_index: u32,
    },
    InputRoleProhibited {
        input_index: u32,
        role: InputRole,
    },
    InvalidPreparedUpload {
        input_index: u32,
    },
    DuplicatePreparedUpload {
        input_index: u32,
    },
    UnexpectedPreparedUpload {
        input_index: u32,
    },
    MissingPreparedUpload {
        input_index: u32,
    },
    InputIdentityMismatch {
        input_index: u32,
    },
    UploadScopeMismatch {
        input_index: u32,
        expected: RemoteUploadScope,
        actual: RemoteUploadScope,
    },
    UploadScopeUnsupported {
        input_index: u32,
        scope: RemoteUploadScope,
    },
    RasterExtentExceedsLimit {
        input_index: u32,
        width: u32,
        height: u32,
        maximum_edge: u32,
    },
    UploadBytesOverflow,
    RequestBytesExceedLimit {
        actual: u64,
        maximum: u64,
    },
    CanonicalEncodingFailed,
    ClockUnavailable,
    GrantExpired,
}
