use std::{
    collections::BTreeSet,
    fs,
    io::Read,
    path::PathBuf,
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_ai::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmittedExecution, AdmittedModelIdentity, AiCapability,
    AiGeneratedPayload, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactHashAlgorithm,
    ArtifactReference, BackendKind, CancellationToken, EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
    ExecutionLease, ExecutionPlanIdentity, ExecutionRouteIdentity, FallbackDisclosure,
    GeneratedArtifactReference, InputRole, MaskPrompt, MaskSemantic, NumericPrecision,
    ObservationTarget, PrivacyClass, ProviderExecutionClass, ProviderIdentity, ProviderTerminal,
    RasterExtent, ResourceEstimate, RunPlan, RuntimeProgressReporter, RuntimeProvider,
    RuntimeTerminalOutcome, RuntimeUsage, SoftMaskArtifact, SoftMaskEncoding,
    SubjectMaskParameters, TaskPriority, UnitInterval, bind_system_execution,
    promote_derived_raster, reload_managed_derived_raster,
};
use shadow_domain::{MaskCoordinateSpace, MaskDefinition};

use super::{DerivedRasterStoreError, FilesystemDerivedRasterStore, managed_soft_mask_definition};

const MASK_BYTES: &[u8] = b"\x00\x20\x80\xff\xff\x80\x20\x00";

#[test]
fn staged_mask_is_promoted_reloaded_and_opened_with_exact_bytes() {
    let fixture = StoreFixture::new("roundtrip");
    let artifact = fixture.stage_mask(MASK_BYTES);
    let mut store = FilesystemDerivedRasterStore::open(&fixture.store_root).expect("open store");
    store
        .stage_proposal_file(&artifact, &fixture.provider_output)
        .expect("stage proposal");

    let request = shadow_ai::DerivedRasterPromotionRequest::new(
        "mask-promotion-1".into(),
        completed_output(artifact.clone()),
    );
    let promoted = promote_derived_raster(&mut store, request).expect("promote mask");
    assert_eq!(
        promoted.managed_artifact().store_object_id(),
        format!(
            "objects/v1/b3/{}/{}",
            &artifact.content_hash()[..2],
            &artifact.content_hash()[2..]
        )
    );

    let definition =
        managed_soft_mask_definition(&promoted, false).expect("immutable Recipe mask reference");
    let MaskDefinition::ManagedRaster { raster, invert, .. } = &definition else {
        panic!("expected managed raster mask")
    };
    assert!(!invert);
    let mut recipe_file = store
        .open_recipe_mask(raster)
        .expect("open Recipe mask bytes");
    let mut recipe_bytes = Vec::new();
    recipe_file
        .read_to_end(&mut recipe_bytes)
        .expect("read Recipe mask");
    assert_eq!(recipe_bytes, MASK_BYTES);

    let record = promoted.persisted_record();
    let reloaded = reload_managed_derived_raster(&mut store, record).expect("reload managed mask");
    let mut file = store
        .open_verified(reloaded.managed_artifact())
        .expect("open verified mask");
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes).expect("read managed mask");
    assert_eq!(bytes, MASK_BYTES);
}

#[test]
fn provider_output_must_match_its_declared_identity_before_staging() {
    let fixture = StoreFixture::new("stage-integrity");
    let artifact = fixture.stage_mask(MASK_BYTES);
    fs::write(&fixture.provider_output, b"substituted").expect("replace provider output");
    let store = FilesystemDerivedRasterStore::open(&fixture.store_root).expect("open store");

    let error = store
        .stage_proposal_file(&artifact, &fixture.provider_output)
        .expect_err("substituted proposal must fail");
    assert!(matches!(
        error,
        DerivedRasterStoreError::ByteLengthMismatch { .. }
            | DerivedRasterStoreError::ContentHashMismatch { .. }
    ));
}

#[test]
fn staged_proposal_can_be_opened_without_promoting_it() {
    let fixture = StoreFixture::new("proposal-preview");
    let artifact = fixture.stage_mask(MASK_BYTES);
    let store = FilesystemDerivedRasterStore::open(&fixture.store_root).expect("open store");
    store
        .stage_proposal_file(&artifact, &fixture.provider_output)
        .expect("stage proposal");

    let mut file = store
        .open_staged_proposal(&artifact)
        .expect("open verified staged proposal");
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes).expect("read staged proposal");

    assert_eq!(bytes, MASK_BYTES);
    assert!(
        !store
            .object_path(&artifact)
            .expect("managed object path")
            .exists(),
        "previewing a proposal must not publish a durable object"
    );
}

#[test]
fn reload_fails_closed_after_managed_bytes_are_corrupted() {
    let fixture = StoreFixture::new("reload-integrity");
    let artifact = fixture.stage_mask(MASK_BYTES);
    let mut store = FilesystemDerivedRasterStore::open(&fixture.store_root).expect("open store");
    store
        .stage_proposal_file(&artifact, &fixture.provider_output)
        .expect("stage proposal");
    let request = shadow_ai::DerivedRasterPromotionRequest::new(
        "mask-promotion-2".into(),
        completed_output(artifact.clone()),
    );
    let promoted = promote_derived_raster(&mut store, request).expect("promote mask");
    let record = promoted.persisted_record();

    let object_path = store.object_path(&artifact).expect("object path");
    fs::write(object_path, b"corrupt").expect("corrupt managed object");
    let error = reload_managed_derived_raster(&mut store, record)
        .expect_err("corrupt managed bytes must not regain authority");
    assert!(matches!(
        error,
        shadow_ai::DerivedRasterPromotionFailure::Store(
            DerivedRasterStoreError::ByteLengthMismatch { .. }
                | DerivedRasterStoreError::ContentHashMismatch { .. }
        )
    ));
}

fn artifact(bytes: &[u8]) -> GeneratedArtifactReference {
    GeneratedArtifactReference::new(
        ArtifactHashAlgorithm::Blake3_256,
        blake3::hash(bytes).to_hex().to_string(),
        u64::try_from(bytes.len()).expect("fixture length"),
        "application/x-shadow-soft-mask".into(),
        1,
    )
    .expect("valid artifact")
}

fn completed_output(
    artifact: GeneratedArtifactReference,
) -> shadow_ai::LeaseBoundOutput<AiGeneratedPayload> {
    let execution = execution();
    let provider = FixtureProvider {
        route: execution.route().clone(),
        plan: execution.plan_identity().clone(),
        payload: AiGeneratedPayload::SoftMask(SoftMaskArtifact {
            artifact,
            raster_extent: RasterExtent::new(4, 2).expect("raster extent"),
            coordinate_extent: RasterExtent::new(6000, 4000).expect("coordinate extent"),
            coordinate_space: MaskCoordinateSpace::Original,
            encoding: SoftMaskEncoding::Gray8Unorm,
            semantic: MaskSemantic::Subject,
        }),
    };
    let receipt = ExecutionLease::issue("mask-store-lease-1".into(), execution)
        .expect("lease")
        .execute(&provider, &CancellationToken::default());
    match receipt.outcome {
        RuntimeTerminalOutcome::Succeeded { output } => *output,
        other => panic!("expected successful fixture execution, got {other:?}"),
    }
}

fn execution() -> AdmittedExecution {
    let route = ExecutionRouteIdentity {
        contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
        provider: ProviderIdentity {
            provider_id: "fixture.coreml-sam2.1".into(),
            adapter_revision: "fixture-adapter-v1".into(),
            execution_class: ProviderExecutionClass::SystemFramework,
        },
        model: AdmittedModelIdentity::SystemFramework {
            framework: "FixtureCoreML".into(),
            request_name: "sam2.1-small-subject-mask".into(),
            request_revision: 1,
            operating_system_build: "fixture-macos".into(),
        },
    };
    let request = AiJobRequest {
        contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
        request_id: "mask-store-request-1".into(),
        generation: 1,
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
        "mask-store-execution-1".into(),
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
    .expect("valid execution")
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

#[derive(Debug)]
struct StoreFixture {
    root: PathBuf,
    store_root: PathBuf,
    provider_output: PathBuf,
}

impl StoreFixture {
    fn new(name: &str) -> Self {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock")
            .as_nanos();
        let root = std::env::temp_dir().join(format!(
            "shadow-derived-raster-store-{name}-{}-{nonce}",
            std::process::id()
        ));
        fs::create_dir_all(&root).expect("fixture root");
        Self {
            store_root: root.join("store"),
            provider_output: root.join("provider-output.mask"),
            root,
        }
    }

    fn stage_mask(&self, bytes: &[u8]) -> GeneratedArtifactReference {
        fs::write(&self.provider_output, bytes).expect("provider output");
        artifact(bytes)
    }
}

impl Drop for StoreFixture {
    fn drop(&mut self) {
        if self.root.starts_with(std::env::temp_dir()) {
            let _ = fs::remove_dir_all(&self.root);
        }
    }
}
