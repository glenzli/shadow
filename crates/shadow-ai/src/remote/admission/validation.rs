use std::collections::BTreeSet;

use crate::remote::{
    PREPARED_REMOTE_UPLOAD_CONTRACT_VERSION, PreparedRemoteUpload, RemoteDataRetention,
    RemoteProviderManifest, RemoteTrainingUse, RemoteUploadScope,
};
use crate::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AiJobRequest, ArtifactReference, InputRole, PrivacyClass,
};

use super::{
    MAX_REMOTE_INPUTS, REMOTE_EXECUTION_REQUEST_SCHEMA_VERSION, RemoteExecutionContext,
    RemoteExecutionPolicy, RemoteProviderBlocker,
};

pub(super) struct ValidatedRemoteUploads {
    uploads: Vec<PreparedRemoteUpload>,
    total_upload_bytes: u64,
}

impl ValidatedRemoteUploads {
    pub(super) fn uploads(&self) -> &[PreparedRemoteUpload] {
        &self.uploads
    }

    pub(super) fn into_parts(self) -> (Vec<PreparedRemoteUpload>, u64) {
        (self.uploads, self.total_upload_bytes)
    }
}

pub(super) fn validate(
    manifest: &RemoteProviderManifest,
    request: &AiJobRequest,
    context: &RemoteExecutionContext,
    mut uploads: Vec<PreparedRemoteUpload>,
) -> Result<ValidatedRemoteUploads, Vec<RemoteProviderBlocker>> {
    if manifest.validate().is_err() {
        return Err(vec![RemoteProviderBlocker::InvalidManifest]);
    }
    let mut blockers = Vec::new();
    validate_context(manifest, request, context, &mut blockers);
    validate_request(manifest, request, &mut blockers);
    let total_upload_bytes = validate_uploads(manifest, request, &uploads, &mut blockers);
    if blockers.is_empty() {
        uploads.sort_unstable_by_key(PreparedRemoteUpload::input_index);
        Ok(ValidatedRemoteUploads {
            uploads,
            total_upload_bytes,
        })
    } else {
        Err(blockers)
    }
}

fn validate_context(
    manifest: &RemoteProviderManifest,
    request: &AiJobRequest,
    context: &RemoteExecutionContext,
    blockers: &mut Vec<RemoteProviderBlocker>,
) {
    if context.schema_version != REMOTE_EXECUTION_REQUEST_SCHEMA_VERSION {
        push_unique(
            blockers,
            RemoteProviderBlocker::UnsupportedContextSchema {
                actual: context.schema_version,
            },
        );
    }
    if !context.network_available {
        push_unique(blockers, RemoteProviderBlocker::Offline);
    }
    if context.policy_revision.trim().is_empty() {
        push_unique(blockers, RemoteProviderBlocker::MissingPolicyRevision);
    }
    if context.consent_receipt_id.trim().is_empty() {
        push_unique(blockers, RemoteProviderBlocker::MissingConsentReceipt);
    }
    if !manifest.supports_idempotency_key {
        push_unique(
            blockers,
            RemoteProviderBlocker::ProviderIdempotencyUnsupported,
        );
    }
    if !privacy_policy_allows(context.policy, request.privacy) {
        push_unique(blockers, RemoteProviderBlocker::PrivacyPolicyDenied);
    }
    if request.privacy == PrivacyClass::SensitiveBiometric {
        push_unique(blockers, RemoteProviderBlocker::SensitiveBiometricDeferred);
    }
    match manifest.retention {
        RemoteDataRetention::NotRetained => {}
        RemoteDataRetention::BoundedDays(offered_days)
            if offered_days <= context.maximum_retention_days => {}
        RemoteDataRetention::BoundedDays(offered_days) => push_unique(
            blockers,
            RemoteProviderBlocker::RetentionExceedsConsent {
                offered_days,
                maximum_days: context.maximum_retention_days,
            },
        ),
        RemoteDataRetention::Undisclosed => {
            push_unique(blockers, RemoteProviderBlocker::RetentionUndisclosed);
        }
    }
    if manifest.training_use != RemoteTrainingUse::ProhibitedByContract {
        push_unique(blockers, RemoteProviderBlocker::TrainingUseNotProhibited);
    }
    if context.accepted_terms_revision.as_deref() != Some(manifest.terms_revision.as_str()) {
        push_unique(blockers, RemoteProviderBlocker::TermsAcceptanceRequired);
    }
}

fn validate_request(
    manifest: &RemoteProviderManifest,
    request: &AiJobRequest,
    blockers: &mut Vec<RemoteProviderBlocker>,
) {
    if request.contract_version != AI_JOB_REQUEST_CONTRACT_VERSION {
        push_unique(
            blockers,
            RemoteProviderBlocker::UnsupportedRequestSchema {
                actual: request.contract_version,
            },
        );
    }
    if request.request_id.trim().is_empty() {
        push_unique(blockers, RemoteProviderBlocker::MissingRequestId);
    }
    if request.inputs.len() > MAX_REMOTE_INPUTS as usize {
        push_unique(
            blockers,
            RemoteProviderBlocker::TooManyInputs {
                actual: u32::try_from(request.inputs.len()).unwrap_or(u32::MAX),
                maximum: MAX_REMOTE_INPUTS,
            },
        );
    }
    if request.validate_task_parameters().is_err() {
        push_unique(blockers, RemoteProviderBlocker::InvalidTaskParameters);
    }
    let capability = request.task.capability();
    if !manifest.capabilities.contains(&capability) {
        push_unique(
            blockers,
            RemoteProviderBlocker::CapabilityUnsupported { capability },
        );
    }
    if !manifest.admitted_privacy.contains(&request.privacy) {
        push_unique(
            blockers,
            RemoteProviderBlocker::PrivacyClassUnsupported {
                privacy: request.privacy,
            },
        );
    }
    for (index, input) in request.inputs.iter().enumerate() {
        let input_index = u32::try_from(index).unwrap_or(u32::MAX);
        if invalid_artifact(input) {
            push_unique(
                blockers,
                RemoteProviderBlocker::InvalidInputIdentity { input_index },
            );
        }
        if input.privacy > request.privacy {
            push_unique(
                blockers,
                RemoteProviderBlocker::RequestUnderstatesInputPrivacy { input_index },
            );
        }
        if !manifest.admitted_privacy.contains(&input.privacy) {
            push_unique(
                blockers,
                RemoteProviderBlocker::InputPrivacyUnsupported {
                    input_index,
                    privacy: input.privacy,
                },
            );
        }
        if upload_scope_for(input.role).is_none() {
            push_unique(
                blockers,
                RemoteProviderBlocker::InputRoleProhibited {
                    input_index,
                    role: input.role,
                },
            );
        }
    }
}

fn validate_uploads(
    manifest: &RemoteProviderManifest,
    request: &AiJobRequest,
    uploads: &[PreparedRemoteUpload],
    blockers: &mut Vec<RemoteProviderBlocker>,
) -> u64 {
    if uploads.len() > MAX_REMOTE_INPUTS as usize {
        push_unique(
            blockers,
            RemoteProviderBlocker::TooManyInputs {
                actual: u32::try_from(uploads.len()).unwrap_or(u32::MAX),
                maximum: MAX_REMOTE_INPUTS,
            },
        );
    }

    let mut seen = BTreeSet::new();
    let mut total = 0_u64;
    for upload in uploads {
        let input_index = upload.input_index();
        if upload.contract_version() != PREPARED_REMOTE_UPLOAD_CONTRACT_VERSION {
            push_unique(
                blockers,
                RemoteProviderBlocker::InvalidPreparedUpload { input_index },
            );
        }
        if !seen.insert(input_index) {
            push_unique(
                blockers,
                RemoteProviderBlocker::DuplicatePreparedUpload { input_index },
            );
            continue;
        }
        let Some(input) = request.inputs.get(input_index as usize) else {
            push_unique(
                blockers,
                RemoteProviderBlocker::UnexpectedPreparedUpload { input_index },
            );
            continue;
        };
        if upload.source() != input {
            push_unique(
                blockers,
                RemoteProviderBlocker::InputIdentityMismatch { input_index },
            );
        }
        let Some(expected) = upload_scope_for(input.role) else {
            continue;
        };
        if upload.scope() != expected {
            push_unique(
                blockers,
                RemoteProviderBlocker::UploadScopeMismatch {
                    input_index,
                    expected,
                    actual: upload.scope(),
                },
            );
        }
        if !manifest.upload_scopes.contains(&upload.scope()) {
            push_unique(
                blockers,
                RemoteProviderBlocker::UploadScopeUnsupported {
                    input_index,
                    scope: upload.scope(),
                },
            );
        }
        if let Some(extent) = upload.raster_extent()
            && (extent.width > manifest.maximum_raster_edge
                || extent.height > manifest.maximum_raster_edge)
        {
            push_unique(
                blockers,
                RemoteProviderBlocker::RasterExtentExceedsLimit {
                    input_index,
                    width: extent.width,
                    height: extent.height,
                    maximum_edge: manifest.maximum_raster_edge,
                },
            );
        }
        total = accumulate_upload_bytes(total, upload.outbound_byte_len(), blockers);
    }
    for input_index in 0..u32::try_from(request.inputs.len()).unwrap_or(u32::MAX) {
        if !seen.contains(&input_index) {
            push_unique(
                blockers,
                RemoteProviderBlocker::MissingPreparedUpload { input_index },
            );
        }
    }
    if total > manifest.maximum_request_bytes {
        push_unique(
            blockers,
            RemoteProviderBlocker::RequestBytesExceedLimit {
                actual: total,
                maximum: manifest.maximum_request_bytes,
            },
        );
    }
    total
}

fn accumulate_upload_bytes(
    current: u64,
    next: u64,
    blockers: &mut Vec<RemoteProviderBlocker>,
) -> u64 {
    if let Some(total) = current.checked_add(next) {
        total
    } else {
        push_unique(blockers, RemoteProviderBlocker::UploadBytesOverflow);
        u64::MAX
    }
}

fn upload_scope_for(role: InputRole) -> Option<RemoteUploadScope> {
    match role {
        InputRole::EmbeddedPreview | InputRole::DisplayProxy => {
            Some(RemoteUploadScope::FullRenderedImage)
        }
        InputRole::CurrentRenderedCrop => Some(RemoteUploadScope::BoundedRenderedCrop),
        InputRole::Mask => Some(RemoteUploadScope::Mask),
        InputRole::StructuredMetadata => Some(RemoteUploadScope::StructuredMetadata),
        InputRole::SearchText => Some(RemoteUploadScope::SearchText),
        InputRole::RawFile
        | InputRole::SensorMosaic
        | InputRole::SceneLinearTile
        | InputRole::FrozenFeatureVector => None,
    }
}

fn invalid_artifact(artifact: &ArtifactReference) -> bool {
    artifact.byte_len == 0
        || artifact.media_type.trim().is_empty()
        || artifact.media_type.len() > 256
        || !valid_digest(&artifact.content_hash)
}

fn valid_digest(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

const fn privacy_policy_allows(policy: RemoteExecutionPolicy, privacy: PrivacyClass) -> bool {
    match policy {
        RemoteExecutionPolicy::Disabled => false,
        RemoteExecutionPolicy::PublicOnly => matches!(privacy, PrivacyClass::Public),
        RemoteExecutionPolicy::PersonalAllowed => {
            matches!(privacy, PrivacyClass::Public | PrivacyClass::Personal)
        }
    }
}

fn push_unique(blockers: &mut Vec<RemoteProviderBlocker>, blocker: RemoteProviderBlocker) {
    if !blockers.contains(&blocker) {
        blockers.push(blocker);
    }
}
