use std::{
    collections::BTreeSet,
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};

use shadow_ai::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmittedExecution, AdmittedModelIdentity, AiCapability,
    AiGeneratedPayload, AiJobRequest, AiTaskKind, AiTaskParameters, ArtifactHashAlgorithm,
    ArtifactReference, BackendKind, CancellationToken, EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
    ExecutionLease, ExecutionPlanIdentity, ExecutionRouteIdentity, FallbackDisclosure,
    GeneratedArtifactReference, InputRole, MaskPrompt, MaskSemantic, NumericPrecision,
    ObservationTarget, PrivacyClass, ProviderExecutionClass, ProviderIdentity, ProviderTerminal,
    RasterExtent, ResourceEstimate, RunPlan, RuntimeProgressReporter, RuntimeProvider,
    RuntimeUsage, SoftMaskArtifact, SoftMaskEncoding, SubjectMaskParameters, TaskPriority,
    UnitInterval, bind_system_execution,
};
use shadow_domain::MaskCoordinateSpace;

use super::*;

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[test]
fn successful_runtime_bytes_are_staged_then_promoted_only_for_the_current_generation() {
    let fixture = Fixture::new("promote");
    let mut store = FilesystemDerivedRasterStore::open(&fixture.store_root).unwrap();
    let (lease, provider) = fixture.execution();

    let receipt = execute_and_stage_derived_raster(
        &store,
        "promotion-7".into(),
        lease,
        &provider,
        &CancellationToken::default(),
        &fixture.proposal_source,
    )
    .expect("staged receipt");
    assert_eq!(receipt.generation, 7);
    assert!(!fixture.proposal_source.exists());
    let DerivedRasterStageOutcome::Staged(staged) = receipt.outcome else {
        panic!("expected staged proposal");
    };
    assert_eq!(staged.request_id(), "mask-request-7");

    let promoted = promote_staged_derived_raster_if_current(&mut store, 7, staged)
        .expect("current proposal promotion");
    assert_eq!(promoted.promotion_id(), "promotion-7");
    assert_eq!(promoted.provenance().generation(), 7);
    store
        .open_verified(promoted.managed_artifact())
        .expect("durable managed bytes");
}

#[test]
fn stale_generation_never_publishes_a_managed_object() {
    let fixture = Fixture::new("stale");
    let mut store = FilesystemDerivedRasterStore::open(&fixture.store_root).unwrap();
    let (lease, provider) = fixture.execution();
    let receipt = execute_and_stage_derived_raster(
        &store,
        "promotion-stale".into(),
        lease,
        &provider,
        &CancellationToken::default(),
        &fixture.proposal_source,
    )
    .expect("staged receipt");
    let DerivedRasterStageOutcome::Staged(staged) = receipt.outcome else {
        panic!("expected staged proposal");
    };

    assert!(matches!(
        promote_staged_derived_raster_if_current(&mut store, 8, staged),
        Err(CurrentDerivedRasterPromotionFailure::StaleGeneration {
            proposal_generation: 7,
            current_generation: 8,
        })
    ));
    assert_eq!(
        count_files(&fixture.store_root.join("objects")),
        0,
        "stale proposals remain rebuildable staging only"
    );
}

fn count_files(root: &PathBuf) -> usize {
    if !root.exists() {
        return 0;
    }
    fs::read_dir(root)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .map(|path| if path.is_dir() { count_files(&path) } else { 1 })
        .sum()
}

struct Fixture {
    root: PathBuf,
    store_root: PathBuf,
    proposal_source: PathBuf,
    payload: AiGeneratedPayload,
}

impl Fixture {
    fn new(label: &str) -> Self {
        let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "shadow-derived-raster-workflow-{label}-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&root).unwrap();
        let proposal_source = root.join("provider-output.gray8");
        let bytes = [0_u8, 64, 192, 255];
        fs::write(&proposal_source, bytes).unwrap();
        let artifact = GeneratedArtifactReference::new(
            ArtifactHashAlgorithm::Blake3_256,
            blake3::hash(&bytes).to_hex().to_string(),
            bytes.len() as u64,
            "application/x-shadow-soft-mask".into(),
            1,
        )
        .unwrap();
        Self {
            store_root: root.join("derived-rasters"),
            root,
            proposal_source,
            payload: AiGeneratedPayload::SoftMask(SoftMaskArtifact {
                artifact,
                raster_extent: RasterExtent::new(2, 2).unwrap(),
                coordinate_extent: RasterExtent::new(1200, 800).unwrap(),
                coordinate_space: MaskCoordinateSpace::Original,
                encoding: SoftMaskEncoding::Gray8Unorm,
                semantic: MaskSemantic::UserPrompt,
            }),
        }
    }

    fn execution(&self) -> (ExecutionLease, FixtureProvider) {
        let route = ExecutionRouteIdentity {
            contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
            provider: ProviderIdentity {
                provider_id: "fixture.system-mask".into(),
                adapter_revision: "fixture-mask-v1".into(),
                execution_class: ProviderExecutionClass::SystemFramework,
            },
            model: AdmittedModelIdentity::SystemFramework {
                framework: "FixtureVision".into(),
                request_name: "subject-mask".into(),
                request_revision: 1,
                operating_system_build: "fixture-os".into(),
            },
        };
        let plan = RunPlan {
            backend_id: "fixture-mask".into(),
            backend_kind: BackendKind::Cpu,
            precision: NumericPrecision::Float32,
            cpu_threads: 1,
            reserved_system_ram_bytes: 64,
            reserved_device_memory_bytes: 0,
        };
        let estimate = ResourceEstimate {
            peak_system_ram_bytes: 64,
            peak_device_memory_bytes: 0,
            cpu_threads: 1,
            scratch_disk_bytes: 0,
            upload_bytes: 0,
            estimated_duration_ms: Some(10),
        };
        let request = AiJobRequest {
            contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
            request_id: "mask-request-7".into(),
            generation: 7,
            task: AiTaskKind::ProposeSubjectMask,
            target: ObservationTarget::Library,
            priority: TaskPriority::CurrentViewport,
            privacy: PrivacyClass::Personal,
            inputs: vec![ArtifactReference {
                role: InputRole::CurrentRenderedCrop,
                content_hash: "c".repeat(64),
                byte_len: 4096,
                media_type: "image/jpeg".into(),
                privacy: PrivacyClass::Personal,
            }],
            parameters: AiTaskParameters::SubjectMask(SubjectMaskParameters {
                prompt: MaskPrompt::AutomaticSubject,
                coordinate_space: MaskCoordinateSpace::Original,
                coordinate_extent: RasterExtent::new(1200, 800).unwrap(),
                edge_refinement: UnitInterval::new(0.5).unwrap(),
                maximum_candidates: 3,
            }),
            estimate,
        };
        let execution = bind_system_execution(
            "mask-execution-7".into(),
            request,
            route.clone(),
            &BTreeSet::from([AiCapability::SubjectMask]),
            plan,
            estimate,
            FallbackDisclosure::Primary,
        )
        .unwrap();
        let provider = FixtureProvider {
            route,
            plan: execution.plan_identity().clone(),
            payload: self.payload.clone(),
        };
        (
            ExecutionLease::issue("mask-lease-7".into(), execution).unwrap(),
            provider,
        )
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
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
