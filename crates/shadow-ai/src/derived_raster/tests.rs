use std::collections::BTreeSet;

use shadow_domain::MaskCoordinateSpace;
use thiserror::Error;

use super::{
    DerivedRasterPromotionError, DerivedRasterPromotionFailure, DerivedRasterPromotionRequest,
    ManagedDerivedRasterStore, ManagedDerivedStoreCommit, ManagedDerivedStoreRead,
    ManagedDerivedStoreWrite, PersistedManagedDerivedRaster, promote_derived_raster,
    reload_managed_derived_raster,
};
use crate::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmittedExecution, AdmittedModelIdentity, AiCapability,
    AiGeneratedPayload, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactHashAlgorithm,
    ArtifactReference, BackendKind, CancellationToken, EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
    ExecutionLease, ExecutionPlanIdentity, ExecutionRouteIdentity, FallbackDisclosure,
    GeneratedArtifactReference, InputRole, MaskPrompt, MaskSemantic, NumericPrecision,
    ObservationTarget, PrivacyClass, ProviderExecutionClass, ProviderIdentity, ProviderTerminal,
    RasterExtent, ResourceEstimate, RunPlan, RuntimeProgressReporter, RuntimeProvider,
    RuntimeTerminalOutcome, RuntimeUsage, SoftMaskArtifact, SoftMaskEncoding,
    SubjectMaskParameters, TaskPriority, UnitInterval, bind_system_execution,
};

fn artifact(hash: char) -> GeneratedArtifactReference {
    GeneratedArtifactReference::new(
        ArtifactHashAlgorithm::Blake3_256,
        hash.to_string().repeat(64),
        2048,
        "application/x-shadow-soft-mask".into(),
        1,
    )
    .expect("valid generated artifact")
}

fn proposal_payload() -> AiGeneratedPayload {
    AiGeneratedPayload::SoftMask(SoftMaskArtifact {
        artifact: artifact('a'),
        raster_extent: RasterExtent::new(512, 512).expect("raster"),
        coordinate_extent: RasterExtent::new(6000, 4000).expect("coordinate extent"),
        coordinate_space: MaskCoordinateSpace::Original,
        encoding: SoftMaskEncoding::Gray8Unorm,
        semantic: MaskSemantic::Subject,
    })
}

fn execution() -> AdmittedExecution {
    let route = ExecutionRouteIdentity {
        contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
        provider: ProviderIdentity {
            provider_id: "example.system-mask".into(),
            adapter_revision: "system-mask-adapter-v1".into(),
            execution_class: ProviderExecutionClass::SystemFramework,
        },
        model: AdmittedModelIdentity::SystemFramework {
            framework: "FixtureVision".into(),
            request_name: "subject-mask".into(),
            request_revision: 1,
            operating_system_build: "fixture-os-1".into(),
        },
    };
    let request = AiJobRequest {
        contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
        request_id: "mask-request-1".into(),
        generation: 7,
        task: AiTaskKind::ProposeSubjectMask,
        target: ObservationTarget::Library,
        priority: TaskPriority::CurrentViewport,
        privacy: PrivacyClass::Personal,
        inputs: vec![ArtifactReference {
            role: InputRole::CurrentRenderedCrop,
            content_hash: "c".repeat(64),
            byte_len: 4096,
            media_type: "image/tiff".into(),
            privacy: PrivacyClass::Personal,
        }],
        parameters: AiTaskParameters::SubjectMask(SubjectMaskParameters {
            prompt: MaskPrompt::AutomaticSubject,
            coordinate_space: MaskCoordinateSpace::Original,
            coordinate_extent: RasterExtent::new(6000, 4000).expect("coordinate extent"),
            edge_refinement: UnitInterval::new(0.5).expect("unit interval"),
            maximum_candidates: 1,
        }),
        estimate: ResourceEstimate {
            peak_system_ram_bytes: 64,
            peak_device_memory_bytes: 0,
            cpu_threads: 1,
            scratch_disk_bytes: 0,
            upload_bytes: 0,
            estimated_duration_ms: Some(10),
        },
    };
    bind_system_execution(
        "mask-execution-1".into(),
        request,
        route,
        &BTreeSet::from([AiCapability::SubjectMask]),
        RunPlan {
            backend_id: "cpu".into(),
            backend_kind: BackendKind::Cpu,
            precision: NumericPrecision::Float32,
            cpu_threads: 1,
            reserved_system_ram_bytes: 64,
            reserved_device_memory_bytes: 0,
        },
        ResourceEstimate {
            peak_system_ram_bytes: 64,
            peak_device_memory_bytes: 0,
            cpu_threads: 1,
            scratch_disk_bytes: 0,
            upload_bytes: 0,
            estimated_duration_ms: Some(10),
        },
        FallbackDisclosure::Primary,
    )
    .expect("valid system execution")
}

#[derive(Debug)]
struct FixtureProvider {
    route: ExecutionRouteIdentity,
    plan: ExecutionPlanIdentity,
    payload: AiGeneratedPayload,
}

impl RuntimeProvider for FixtureProvider {
    type Output = AiGeneratedPayload;

    fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.plan
    }

    fn supports(&self, capability: AiCapability) -> bool {
        capability == AiCapability::SubjectMask
    }

    fn execute(
        &self,
        _execution: &AdmittedExecution,
        _cancellation: &CancellationToken,
        _progress: &RuntimeProgressReporter<'_>,
    ) -> ProviderTerminal<Self::Output> {
        ProviderTerminal::Succeeded {
            output: self.payload.clone(),
            usage: RuntimeUsage::default(),
        }
    }
}

fn completed_output() -> crate::LeaseBoundOutput<AiGeneratedPayload> {
    let execution = execution();
    let provider = FixtureProvider {
        route: execution.route().clone(),
        plan: execution.plan_identity().clone(),
        payload: proposal_payload(),
    };
    let receipt = ExecutionLease::issue("mask-lease-1".into(), execution)
        .expect("lease")
        .execute(&provider, &CancellationToken::default());
    match receipt.outcome {
        RuntimeTerminalOutcome::Succeeded { output } => *output,
        other => panic!("expected successful fixture execution, got {other:?}"),
    }
}

fn request() -> DerivedRasterPromotionRequest {
    DerivedRasterPromotionRequest::new("promotion-1".into(), completed_output())
}

#[derive(Debug, Error)]
#[error("fixture store failed")]
struct FixtureStoreError;

#[derive(Debug)]
struct FixtureStore {
    promote_commit: Option<ManagedDerivedStoreCommit>,
    verify_commit: Option<ManagedDerivedStoreCommit>,
    fail_promote: bool,
    fail_verify: bool,
    observed_promotion_id: Option<String>,
    observed_provenance_cache_key: Option<String>,
    observed_verification: bool,
}

impl FixtureStore {
    fn successful(artifact: GeneratedArtifactReference) -> Self {
        let commit = ManagedDerivedStoreCommit {
            store_object_id: "derived/b3/aa".into(),
            storage_revision: 1,
            artifact,
        };
        Self {
            promote_commit: Some(commit.clone()),
            verify_commit: Some(commit),
            fail_promote: false,
            fail_verify: false,
            observed_promotion_id: None,
            observed_provenance_cache_key: None,
            observed_verification: false,
        }
    }
}

impl ManagedDerivedRasterStore for FixtureStore {
    type Error = FixtureStoreError;

    fn promote(
        &mut self,
        write: ManagedDerivedStoreWrite<'_>,
    ) -> Result<ManagedDerivedStoreCommit, Self::Error> {
        self.observed_promotion_id = Some(write.promotion_id().to_owned());
        self.observed_provenance_cache_key = Some(write.provenance().cache_key_blake3().to_owned());
        assert_eq!(write.task(), AiTaskKind::ProposeSubjectMask);
        assert_eq!(write.proposal(), &artifact('a'));
        if self.fail_promote {
            return Err(FixtureStoreError);
        }
        Ok(self.promote_commit.clone().expect("fixture promote commit"))
    }

    fn verify(
        &mut self,
        read: ManagedDerivedStoreRead<'_>,
    ) -> Result<ManagedDerivedStoreCommit, Self::Error> {
        self.observed_verification = true;
        assert_eq!(read.promotion_id(), "promotion-1");
        assert_eq!(read.task(), AiTaskKind::ProposeSubjectMask);
        assert_eq!(read.store_object_id(), "derived/b3/aa");
        assert_eq!(read.storage_revision(), 1);
        assert_eq!(
            read.provenance().cache_key_blake3(),
            self.observed_provenance_cache_key
                .as_deref()
                .expect("promotion provenance observed")
        );
        assert!(matches!(read.payload(), AiGeneratedPayload::SoftMask(_)));
        if self.fail_verify {
            return Err(FixtureStoreError);
        }
        Ok(self.verify_commit.clone().expect("fixture verify commit"))
    }
}

#[test]
fn promotion_accepts_only_runtime_output_and_preserves_its_provenance() {
    let completed = completed_output();
    let expected_provenance = completed.provenance().clone();
    // The promotion request deliberately has no task, payload, or provenance
    // argument that a caller could substitute independently of this envelope.
    let request = DerivedRasterPromotionRequest::new("promotion-1".into(), completed);
    let mut store = FixtureStore::successful(artifact('a'));
    let promoted = promote_derived_raster(&mut store, request).expect("promotion");

    assert_eq!(promoted.promotion_id(), "promotion-1");
    assert_eq!(promoted.task(), AiTaskKind::ProposeSubjectMask);
    assert_eq!(promoted.provenance(), &expected_provenance);
    assert_eq!(promoted.managed_artifact().artifact(), &artifact('a'));
    assert_eq!(
        promoted.managed_artifact().store_object_id(),
        "derived/b3/aa"
    );
    assert_eq!(store.observed_promotion_id.as_deref(), Some("promotion-1"));
    assert_eq!(
        store.observed_provenance_cache_key.as_deref(),
        Some(expected_provenance.cache_key_blake3())
    );
}

#[test]
fn persisted_descriptor_requires_store_verification_to_restore_authority() {
    let mut store = FixtureStore::successful(artifact('a'));
    let promoted = promote_derived_raster(&mut store, request()).expect("promotion");
    let encoded =
        serde_json::to_vec(&promoted.persisted_record()).expect("serialize descriptor only");
    let record: PersistedManagedDerivedRaster =
        serde_json::from_slice(&encoded).expect("deserialize descriptor");

    store.fail_verify = true;
    assert!(matches!(
        reload_managed_derived_raster(&mut store, record),
        Err(DerivedRasterPromotionFailure::Store(FixtureStoreError))
    ));
    assert!(store.observed_verification);
}

#[test]
fn store_authority_cannot_change_bytes_during_promotion() {
    let mut store = FixtureStore::successful(artifact('d'));
    let error =
        promote_derived_raster(&mut store, request()).expect_err("identity mismatch must fail");
    assert!(matches!(
        error,
        DerivedRasterPromotionFailure::Contract(
            DerivedRasterPromotionError::ArtifactIdentityChanged
        )
    ));
}

#[test]
fn reload_rejects_a_changed_store_identity() {
    let mut store = FixtureStore::successful(artifact('a'));
    let promoted = promote_derived_raster(&mut store, request()).expect("promotion");
    let record = promoted.persisted_record();
    store.verify_commit = Some(ManagedDerivedStoreCommit {
        store_object_id: "derived/b3/substituted".into(),
        storage_revision: 2,
        artifact: artifact('a'),
    });

    let error = reload_managed_derived_raster(&mut store, record)
        .expect_err("changed store identity must fail closed");
    assert!(matches!(
        error,
        DerivedRasterPromotionFailure::Contract(DerivedRasterPromotionError::StoreRecordMismatch)
    ));
}

#[test]
fn reload_rejects_changed_managed_bytes() {
    let mut store = FixtureStore::successful(artifact('a'));
    let promoted = promote_derived_raster(&mut store, request()).expect("promotion");
    let record = promoted.persisted_record();
    store.verify_commit = Some(ManagedDerivedStoreCommit {
        store_object_id: "derived/b3/aa".into(),
        storage_revision: 1,
        artifact: artifact('d'),
    });

    let error = reload_managed_derived_raster(&mut store, record)
        .expect_err("changed managed bytes must fail closed");
    assert!(matches!(
        error,
        DerivedRasterPromotionFailure::Contract(
            DerivedRasterPromotionError::ArtifactIdentityChanged
        )
    ));
}

#[test]
fn store_failure_never_returns_managed_authority() {
    let mut store = FixtureStore {
        promote_commit: None,
        verify_commit: None,
        fail_promote: true,
        fail_verify: false,
        observed_promotion_id: None,
        observed_provenance_cache_key: None,
        observed_verification: false,
    };
    assert!(matches!(
        promote_derived_raster(&mut store, request()),
        Err(DerivedRasterPromotionFailure::Store(FixtureStoreError))
    ));
}
