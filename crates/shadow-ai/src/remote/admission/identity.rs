use blake3::Hasher;
use serde::Serialize;

use crate::remote::{
    PreparedRemoteUpload, RemoteDataRetention, RemoteProviderManifest, RemoteTrainingUse,
    RemoteUploadScope,
};
use crate::{AiJobRequest, ExecutionRouteIdentity, PrivacyClass, RasterExtent};

use super::{
    RemoteExecutionContext, RemoteExecutionPolicy, RemoteProviderBlocker,
    validation::ValidatedRemoteUploads,
};

pub(super) struct RemoteGrantIdentity {
    pub(super) request_blake3: String,
    pub(super) route: ExecutionRouteIdentity,
    pub(super) manifest_blake3: String,
    pub(super) legal_policy_blake3: String,
    pub(super) consent_policy_blake3: String,
    pub(super) idempotency_key: String,
}

pub(super) fn build(
    manifest: &RemoteProviderManifest,
    request: &AiJobRequest,
    context: &RemoteExecutionContext,
    uploads: &ValidatedRemoteUploads,
) -> Result<RemoteGrantIdentity, RemoteProviderBlocker> {
    let request_blake3 = digest_json("shadow.ai.remote-request.v1", request)?;
    let manifest_blake3 = digest_json("shadow.ai.remote-manifest.v1", manifest)?;
    let legal_policy_blake3 = legal_policy_digest(manifest)?;
    let consent_policy_blake3 = consent_policy_digest(request, context)?;
    let route = manifest.execution_route();
    let idempotency_key = idempotency_key(
        &request_blake3,
        &route,
        &manifest_blake3,
        &legal_policy_blake3,
        &consent_policy_blake3,
        uploads.uploads(),
    )?;
    Ok(RemoteGrantIdentity {
        request_blake3,
        route,
        manifest_blake3,
        legal_policy_blake3,
        consent_policy_blake3,
        idempotency_key,
    })
}

fn legal_policy_digest(manifest: &RemoteProviderManifest) -> Result<String, RemoteProviderBlocker> {
    #[derive(Serialize)]
    struct LegalPolicy<'a> {
        service_revision: &'a str,
        api_contract_revision: &'a str,
        terms_revision: &'a str,
        terms_url: &'a str,
        privacy_url: &'a str,
        retention: RemoteDataRetention,
        training_use: RemoteTrainingUse,
    }
    digest_json(
        "shadow.ai.remote-legal-policy.v1",
        &LegalPolicy {
            service_revision: &manifest.service_revision,
            api_contract_revision: &manifest.api_contract_revision,
            terms_revision: &manifest.terms_revision,
            terms_url: &manifest.terms_url,
            privacy_url: &manifest.privacy_url,
            retention: manifest.retention,
            training_use: manifest.training_use,
        },
    )
}

fn consent_policy_digest(
    request: &AiJobRequest,
    context: &RemoteExecutionContext,
) -> Result<String, RemoteProviderBlocker> {
    #[derive(Serialize)]
    struct ConsentPolicy<'a> {
        policy_revision: &'a str,
        consent_receipt_id: &'a str,
        policy: RemoteExecutionPolicy,
        accepted_terms_revision: Option<&'a str>,
        maximum_retention_days: u16,
        request_privacy: PrivacyClass,
    }
    digest_json(
        "shadow.ai.remote-consent-policy.v1",
        &ConsentPolicy {
            policy_revision: &context.policy_revision,
            consent_receipt_id: &context.consent_receipt_id,
            policy: context.policy,
            accepted_terms_revision: context.accepted_terms_revision.as_deref(),
            maximum_retention_days: context.maximum_retention_days,
            request_privacy: request.privacy,
        },
    )
}

fn idempotency_key(
    request_blake3: &str,
    route: &ExecutionRouteIdentity,
    manifest_blake3: &str,
    legal_policy_blake3: &str,
    consent_policy_blake3: &str,
    uploads: &[PreparedRemoteUpload],
) -> Result<String, RemoteProviderBlocker> {
    #[derive(Serialize)]
    struct UploadIdentity<'a> {
        input_index: u32,
        source_content_hash: &'a str,
        scope: RemoteUploadScope,
        raster_extent: Option<RasterExtent>,
        store_object_id: &'a str,
        storage_revision: u32,
        sanitization_revision: &'a str,
        outbound_content_hash: &'a str,
        outbound_byte_len: u64,
        outbound_media_type: &'a str,
    }
    #[derive(Serialize)]
    struct IdempotencyIdentity<'a> {
        request_blake3: &'a str,
        route: &'a ExecutionRouteIdentity,
        manifest_blake3: &'a str,
        legal_policy_blake3: &'a str,
        consent_policy_blake3: &'a str,
        uploads: Vec<UploadIdentity<'a>>,
    }
    let upload_identities = uploads
        .iter()
        .map(|upload| UploadIdentity {
            input_index: upload.input_index(),
            source_content_hash: &upload.source().content_hash,
            scope: upload.scope(),
            raster_extent: upload.raster_extent(),
            store_object_id: upload.store_object_id(),
            storage_revision: upload.storage_revision(),
            sanitization_revision: upload.sanitization_revision(),
            outbound_content_hash: upload.outbound_content_hash(),
            outbound_byte_len: upload.outbound_byte_len(),
            outbound_media_type: upload.outbound_media_type(),
        })
        .collect();
    digest_json(
        "shadow.ai.remote-idempotency-key.v1",
        &IdempotencyIdentity {
            request_blake3,
            route,
            manifest_blake3,
            legal_policy_blake3,
            consent_policy_blake3,
            uploads: upload_identities,
        },
    )
}

fn digest_json<T: Serialize + ?Sized>(
    context: &'static str,
    value: &T,
) -> Result<String, RemoteProviderBlocker> {
    let bytes =
        serde_json::to_vec(value).map_err(|_| RemoteProviderBlocker::CanonicalEncodingFailed)?;
    let mut hasher = Hasher::new_derive_key(context);
    hasher.update(&(bytes.len() as u64).to_be_bytes());
    hasher.update(&bytes);
    Ok(hasher.finalize().to_hex().to_string())
}
