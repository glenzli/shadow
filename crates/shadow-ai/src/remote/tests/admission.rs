use std::convert::Infallible;
use std::str::FromStr;

use shadow_domain::PhotoId;

use crate::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactReference,
    ImageCompletionParameters, InputRole, ObservationTarget, PrivacyClass, RasterExtent,
    RemoteExecutionPolicy, RemoteUploadPreparation, RemoteUploadStore, ResourceEstimate,
    TaskPriority,
    remote::{
        PreparedRemoteUpload, PreparedRemoteUploadCommit, REMOTE_EXECUTION_REQUEST_SCHEMA_VERSION,
        RemoteDataRetention, RemoteExecutionContext, RemoteProviderBlocker, RemoteTrainingUse,
        RemoteUploadScope, admit_remote_execution, prepare_remote_upload,
    },
};

use super::provider_fixture::manifest;

fn photo() -> PhotoId {
    PhotoId::from_str("018f3ec1-6219-7df2-a52d-f744c4f88533").expect("photo id")
}

fn artifact(role: InputRole, hash: char, privacy: PrivacyClass) -> ArtifactReference {
    ArtifactReference {
        role,
        content_hash: hash.to_string().repeat(64),
        byte_len: 4096,
        media_type: match role {
            InputRole::Mask => "image/x-shadow-mask",
            _ => "image/png",
        }
        .into(),
        privacy,
    }
}

fn request() -> AiJobRequest {
    AiJobRequest {
        contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
        request_id: "remote-fill-1".into(),
        generation: 4,
        task: AiTaskKind::GenerateInpaintPatch,
        target: ObservationTarget::Region {
            photo_id: photo(),
            region_id: "repair-1".into(),
        },
        priority: TaskPriority::CurrentInput,
        privacy: PrivacyClass::Personal,
        inputs: vec![
            artifact(InputRole::CurrentRenderedCrop, 'a', PrivacyClass::Personal),
            artifact(InputRole::Mask, 'b', PrivacyClass::Personal),
        ],
        parameters: AiTaskParameters::ImageCompletion(ImageCompletionParameters {
            coordinate_extent: RasterExtent::new(1024, 768).expect("fixture extent"),
            source_recipe_blake3: "e".repeat(64),
            mask_revision: "f".repeat(64),
        }),
        estimate: ResourceEstimate {
            upload_bytes: 8192,
            cpu_threads: 1,
            ..ResourceEstimate::default()
        },
    }
}

fn context() -> RemoteExecutionContext {
    RemoteExecutionContext {
        schema_version: REMOTE_EXECUTION_REQUEST_SCHEMA_VERSION,
        network_available: true,
        policy: RemoteExecutionPolicy::PersonalAllowed,
        accepted_terms_revision: Some("terms-2026-07".into()),
        maximum_retention_days: 0,
        policy_revision: "remote-policy-v3".into(),
        consent_receipt_id: "consent-018f3ec1".into(),
    }
}

struct FixtureUploadStore {
    outbound_lengths: Vec<u64>,
    sanitization_revision: &'static str,
}

impl RemoteUploadStore for FixtureUploadStore {
    type Error = Infallible;

    fn prepare(
        &mut self,
        request: RemoteUploadPreparation<'_>,
    ) -> Result<PreparedRemoteUploadCommit, Self::Error> {
        let input_index = request.input_index() as usize;
        let hash = if input_index.is_multiple_of(2) {
            'c'
        } else {
            'd'
        };
        Ok(PreparedRemoteUploadCommit {
            store_object_id: format!("outbound/{input_index}"),
            storage_revision: 7,
            sanitization_revision: self.sanitization_revision.into(),
            outbound_content_hash: hash.to_string().repeat(64),
            outbound_byte_len: self.outbound_lengths[input_index],
            outbound_media_type: match request.scope() {
                RemoteUploadScope::Mask => "image/x-shadow-mask",
                _ => "image/png",
            }
            .into(),
            outbound_raster_extent: request.raster_extent(),
        })
    }
}

fn prepared_uploads(
    request: &AiJobRequest,
    outbound_lengths: [u64; 2],
) -> Vec<PreparedRemoteUpload> {
    let mut store = FixtureUploadStore {
        outbound_lengths: outbound_lengths.into(),
        sanitization_revision: "sanitize-v5",
    };
    request
        .inputs
        .iter()
        .enumerate()
        .map(|(index, input)| {
            let scope = match input.role {
                InputRole::CurrentRenderedCrop => RemoteUploadScope::BoundedRenderedCrop,
                InputRole::Mask => RemoteUploadScope::Mask,
                role => panic!("unexpected fixture role {role:?}"),
            };
            prepare_remote_upload(
                &mut store,
                u32::try_from(index).expect("fixture input index"),
                input,
                scope,
                Some(RasterExtent::new(1024, 768).expect("fixture extent")),
            )
            .expect("prepared upload")
        })
        .collect()
}

#[test]
fn exact_request_and_outbound_inventory_produce_one_consumable_grant() {
    let manifest = manifest();
    let request = request();
    let grant = admit_remote_execution(
        &manifest,
        &request,
        &context(),
        prepared_uploads(&request, [2048, 1024]),
    )
    .expect("remote admission grant");

    assert_eq!(grant.route(), &manifest.execution_route());
    assert_eq!(grant.total_upload_bytes(), 3072);
    assert_eq!(grant.request_id(), request.request_id);
    assert_eq!(grant.generation(), request.generation);
    assert_eq!(grant.request_blake3().len(), 64);
    assert_eq!(grant.manifest_blake3().len(), 64);
    assert_eq!(grant.legal_policy_blake3().len(), 64);
    assert_eq!(grant.consent_policy_blake3().len(), 64);

    let transport = grant
        .into_transport_execution()
        .expect("fresh grant is consumable");
    assert_eq!(transport.request(), &request);
    assert_eq!(transport.uploads().len(), 2);
    assert_eq!(transport.total_upload_bytes(), 3072);
    assert_eq!(transport.request_blake3().len(), 64);
}

#[test]
fn outbound_bytes_not_source_bytes_control_request_admission() {
    let request = request();
    let mut manifest = manifest();
    manifest.maximum_request_bytes = 3000;

    admit_remote_execution(
        &manifest,
        &request,
        &context(),
        prepared_uploads(&request, [1500, 1500]),
    )
    .expect("8 KiB sources may sanitize to an admitted 3 KiB outbound body");

    let denial = admit_remote_execution(
        &manifest,
        &request,
        &context(),
        prepared_uploads(&request, [1501, 1500]),
    )
    .expect_err("outbound body exceeds provider contract");
    assert!(
        denial
            .blockers
            .contains(&RemoteProviderBlocker::RequestBytesExceedLimit {
                actual: 3001,
                maximum: 3000,
            })
    );
}

#[test]
fn grant_binds_full_request_policy_manifest_and_prepared_bytes() {
    let request = request();
    let manifest = manifest();
    let context = context();
    let first = admit_remote_execution(
        &manifest,
        &request,
        &context,
        prepared_uploads(&request, [2048, 1024]),
    )
    .expect("first grant");
    let same = admit_remote_execution(
        &manifest,
        &request,
        &context,
        prepared_uploads(&request, [2048, 1024]),
    )
    .expect("same logical grant");
    assert_eq!(first.idempotency_key(), same.idempotency_key());

    let mut changed_request = request.clone();
    changed_request.generation += 1;
    let changed = admit_remote_execution(
        &manifest,
        &changed_request,
        &context,
        prepared_uploads(&changed_request, [2048, 1024]),
    )
    .expect("changed request");
    assert_ne!(first.idempotency_key(), changed.idempotency_key());

    let mut changed_context = context.clone();
    changed_context.policy_revision = "remote-policy-v4".into();
    let changed = admit_remote_execution(
        &manifest,
        &request,
        &changed_context,
        prepared_uploads(&request, [2048, 1024]),
    )
    .expect("changed policy");
    assert_ne!(first.idempotency_key(), changed.idempotency_key());

    let changed = admit_remote_execution(
        &manifest,
        &request,
        &context,
        prepared_uploads(&request, [2049, 1024]),
    )
    .expect("changed outbound object");
    assert_ne!(first.idempotency_key(), changed.idempotency_key());
}

#[test]
fn upload_permutations_share_one_identity_and_canonical_transport_order() {
    let request = request();
    let canonical = admit_remote_execution(
        &manifest(),
        &request,
        &context(),
        prepared_uploads(&request, [2048, 1024]),
    )
    .expect("canonical upload order");

    let mut reversed_uploads = prepared_uploads(&request, [2048, 1024]);
    reversed_uploads.reverse();
    let reversed = admit_remote_execution(&manifest(), &request, &context(), reversed_uploads)
        .expect("permuted upload order");

    assert_eq!(canonical.idempotency_key(), reversed.idempotency_key());
    for grant in [canonical, reversed] {
        let transport = grant
            .into_transport_execution()
            .expect("fresh grant is consumable");
        let input_indices = transport
            .uploads()
            .iter()
            .map(PreparedRemoteUpload::input_index)
            .collect::<Vec<_>>();
        assert_eq!(input_indices, vec![0, 1]);
    }
}

#[test]
fn substituted_duplicate_and_missing_upload_receipts_fail_closed() {
    let request = request();
    let mut other_request = request.clone();
    other_request.inputs[0].content_hash = "e".repeat(64);
    let denial = admit_remote_execution(
        &manifest(),
        &other_request,
        &context(),
        prepared_uploads(&request, [2048, 1024]),
    )
    .expect_err("receipts were prepared for another source");
    assert!(
        denial
            .blockers
            .contains(&RemoteProviderBlocker::InputIdentityMismatch { input_index: 0 })
    );

    let mut duplicates = prepared_uploads(&request, [2048, 1024]);
    let mut another = prepared_uploads(&request, [2048, 1024]);
    duplicates.push(another.remove(0));
    let denial = admit_remote_execution(&manifest(), &request, &context(), duplicates)
        .expect_err("duplicate input receipt");
    assert!(
        denial
            .blockers
            .contains(&RemoteProviderBlocker::DuplicatePreparedUpload { input_index: 0 })
    );

    let mut missing = prepared_uploads(&request, [2048, 1024]);
    missing.pop();
    let denial = admit_remote_execution(&manifest(), &request, &context(), missing)
        .expect_err("missing input receipt");
    assert!(
        denial
            .blockers
            .contains(&RemoteProviderBlocker::MissingPreparedUpload { input_index: 1 })
    );
}

#[test]
fn expired_grant_cannot_become_transport_authority() {
    let request = request();
    let grant = admit_remote_execution(
        &manifest(),
        &request,
        &context(),
        prepared_uploads(&request, [2048, 1024]),
    )
    .expect("grant");
    let expired_at = grant.expires_at_unix_ms() + 1;
    assert!(matches!(
        grant.into_transport_execution_at_for_test(expired_at),
        Err(RemoteProviderBlocker::GrantExpired)
    ));
}

#[test]
fn gate_rejects_schema_capability_manifest_and_idempotency_mismatches() {
    let request = request();
    let mut bad_schema = request.clone();
    bad_schema.contract_version += 1;
    let denial = admit_remote_execution(
        &manifest(),
        &bad_schema,
        &context(),
        prepared_uploads(&request, [2048, 1024]),
    )
    .expect_err("request schema");
    assert!(
        denial
            .blockers
            .contains(&RemoteProviderBlocker::UnsupportedRequestSchema {
                actual: AI_JOB_REQUEST_CONTRACT_VERSION + 1,
            })
    );

    let mut unsupported = request.clone();
    unsupported.task = AiTaskKind::SuperResolve;
    let denial = admit_remote_execution(
        &manifest(),
        &unsupported,
        &context(),
        prepared_uploads(&unsupported, [2048, 1024]),
    )
    .expect_err("capability");
    assert!(
        denial
            .blockers
            .contains(&RemoteProviderBlocker::CapabilityUnsupported {
                capability: crate::AiCapability::SuperResolution,
            })
    );

    let mut invalid_manifest = manifest();
    invalid_manifest.adapter_revision.clear();
    assert_eq!(
        admit_remote_execution(
            &invalid_manifest,
            &request,
            &context(),
            prepared_uploads(&request, [2048, 1024]),
        )
        .expect_err("invalid manifest")
        .blockers,
        vec![RemoteProviderBlocker::InvalidManifest]
    );

    let mut no_idempotency = manifest();
    no_idempotency.supports_idempotency_key = false;
    let denial = admit_remote_execution(
        &no_idempotency,
        &request,
        &context(),
        prepared_uploads(&request, [2048, 1024]),
    )
    .expect_err("idempotency is required");
    assert!(
        denial
            .blockers
            .contains(&RemoteProviderBlocker::ProviderIdempotencyUnsupported)
    );
}

#[test]
fn raw_sensor_and_scene_linear_inputs_are_never_remotely_admissible() {
    for role in [
        InputRole::RawFile,
        InputRole::SensorMosaic,
        InputRole::SceneLinearTile,
        InputRole::FrozenFeatureVector,
    ] {
        let mut request = request();
        request.inputs[0] = artifact(role, 'a', PrivacyClass::Personal);
        let denial = admit_remote_execution(
            &manifest(),
            &request,
            &context(),
            prepared_uploads_for_prohibited_role(&request),
        )
        .expect_err("role denied");
        assert!(
            denial
                .blockers
                .contains(&RemoteProviderBlocker::InputRoleProhibited {
                    input_index: 0,
                    role,
                })
        );
    }
}

fn prepared_uploads_for_prohibited_role(request: &AiJobRequest) -> Vec<PreparedRemoteUpload> {
    let mut store = FixtureUploadStore {
        outbound_lengths: vec![2048, 1024],
        sanitization_revision: "sanitize-v5",
    };
    request
        .inputs
        .iter()
        .enumerate()
        .map(|(index, input)| {
            let scope = if index == 0 {
                RemoteUploadScope::BoundedRenderedCrop
            } else {
                RemoteUploadScope::Mask
            };
            prepare_remote_upload(
                &mut store,
                u32::try_from(index).expect("fixture input index"),
                input,
                scope,
                Some(RasterExtent::new(1024, 768).expect("fixture extent")),
            )
            .expect("prepared upload")
        })
        .collect()
}

#[test]
fn network_privacy_terms_retention_and_training_all_fail_closed() {
    let mut manifest = manifest();
    manifest.retention = RemoteDataRetention::BoundedDays(30);
    manifest.training_use = RemoteTrainingUse::OptOutRequired;
    let mut request = request();
    request.privacy = PrivacyClass::SensitiveBiometric;
    request.inputs[0].privacy = PrivacyClass::SensitiveBiometric;
    let mut context = context();
    context.network_available = false;
    context.policy = RemoteExecutionPolicy::PublicOnly;
    context.accepted_terms_revision = None;
    context.maximum_retention_days = 7;

    let denial = admit_remote_execution(
        &manifest,
        &request,
        &context,
        prepared_uploads(&request, [2048, 1024]),
    )
    .expect_err("policy denial");
    for blocker in [
        RemoteProviderBlocker::Offline,
        RemoteProviderBlocker::PrivacyPolicyDenied,
        RemoteProviderBlocker::SensitiveBiometricDeferred,
        RemoteProviderBlocker::RetentionExceedsConsent {
            offered_days: 30,
            maximum_days: 7,
        },
        RemoteProviderBlocker::TrainingUseNotProhibited,
        RemoteProviderBlocker::TermsAcceptanceRequired,
    ] {
        assert!(denial.blockers.contains(&blocker), "missing {blocker:?}");
    }
}
