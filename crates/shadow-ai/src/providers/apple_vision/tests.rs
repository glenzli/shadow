use std::str::FromStr;

use shadow_domain::GroupId;

use super::*;
use crate::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AiJobRequest, AiTaskKind, ArtifactReference, BackendKind,
    ExecutionLease, ExecutionPlanIdentity, FallbackDisclosure, InputRole, NumericPrecision,
    ObservationTarget, PrivacyClass, ResourceEstimate, RunPlan, RuntimeTerminalOutcome,
    TaskPriority, bind_system_execution,
};

fn request() -> AiJobRequest {
    AiJobRequest {
        contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
        request_id: "vision-burst-1".into(),
        generation: 3,
        task: AiTaskKind::ProposeBurstGroup,
        target: ObservationTarget::Group {
            group_id: GroupId::from_str("018f3ec1-6219-7df2-a52d-f744c4f88533").expect("group id"),
        },
        priority: TaskPriority::CurrentCollectionAnalysis,
        privacy: PrivacyClass::Personal,
        inputs: vec![ArtifactReference {
            role: InputRole::DisplayProxy,
            content_hash: "a".repeat(64),
            byte_len: 4096,
            media_type: "image/jpeg".into(),
            privacy: PrivacyClass::Personal,
        }],
        parameters: crate::AiTaskParameters::None,
        estimate: ResourceEstimate {
            peak_system_ram_bytes: 32 * 1024 * 1024,
            peak_device_memory_bytes: 0,
            cpu_threads: 1,
            scratch_disk_bytes: 0,
            upload_bytes: 0,
            estimated_duration_ms: None,
        },
    }
}

fn run_plan() -> RunPlan {
    RunPlan {
        backend_id: "apple-vision".into(),
        backend_kind: BackendKind::ExternalLocal,
        precision: NumericPrecision::Float32,
        cpu_threads: 1,
        reserved_system_ram_bytes: 32 * 1024 * 1024,
        reserved_device_memory_bytes: 0,
    }
}

fn provider(operating_system_build: &str) -> AppleVisionFeaturePrintProvider {
    AppleVisionFeaturePrintProvider::new(
        operating_system_build,
        ExecutionPlanIdentity::from_plan(run_plan()).expect("valid Apple Vision plan"),
    )
}

#[test]
fn unlinked_provider_returns_unavailable_without_placeholder_distances() {
    let provider = provider("test-os-build");
    let execution = bind_system_execution(
        "vision-execution-1".into(),
        request(),
        provider.route().clone(),
        &AppleVisionFeaturePrintProvider::capabilities(),
        run_plan(),
        request().estimate,
        FallbackDisclosure::Primary,
    )
    .expect("system execution");

    let receipt = ExecutionLease::issue("vision-lease-1".into(), execution)
        .expect("lease")
        .execute(&provider, &CancellationToken::default());

    let expected = if cfg!(target_os = "macos") {
        ProviderUnavailable::AdapterNotLinked
    } else {
        ProviderUnavailable::PlatformUnsupported
    };
    assert!(matches!(
        receipt.outcome,
        RuntimeTerminalOutcome::Unavailable { reason } if reason == expected
    ));
    assert_eq!(receipt.usage, RuntimeUsage::default());
}

#[test]
fn route_pins_the_vision_request_revision_and_os_build() {
    let provider = provider("23F79");
    let route = provider.route();
    assert!(matches!(
        route.model,
        AdmittedModelIdentity::SystemFramework {
            request_revision: APPLE_VISION_FEATURE_PRINT_REQUEST_REVISION,
            ref operating_system_build,
            ..
        } if operating_system_build == "23F79"
    ));
}

#[test]
fn provider_pins_the_complete_admitted_execution_plan() {
    let provider = provider("23F79");
    assert_eq!(&provider.execution_plan().plan, &run_plan());
    provider
        .execution_plan()
        .validate()
        .expect("complete Apple Vision execution plan identity");
}

#[test]
fn feature_print_route_owns_burst_distance_grouping_not_quality_or_embeddings() {
    let capabilities = AppleVisionFeaturePrintProvider::capabilities();
    assert_eq!(capabilities, BTreeSet::from([AiCapability::BurstGrouping]));
    assert!(!capabilities.contains(&AiCapability::TechnicalQuality));
    assert!(!capabilities.contains(&AiCapability::SimilarityEmbedding));
}

#[test]
fn provider_refuses_an_admission_for_another_os_build() {
    let admitted_provider = provider("23F79");
    let execution = bind_system_execution(
        "vision-execution-route-mismatch".into(),
        request(),
        admitted_provider.route().clone(),
        &AppleVisionFeaturePrintProvider::capabilities(),
        run_plan(),
        request().estimate,
        FallbackDisclosure::Primary,
    )
    .expect("system execution");
    let actual_provider = provider("24A100");
    let receipt = ExecutionLease::issue("vision-lease-route-mismatch".into(), execution)
        .expect("lease")
        .execute(&actual_provider, &CancellationToken::default());
    assert!(matches!(
        receipt.outcome,
        RuntimeTerminalOutcome::Failed {
            failure: crate::RuntimeFailure::ExecutionRouteMismatch,
        }
    ));
}

#[test]
fn adapter_revision_describes_the_linkable_protocol_not_build_availability() {
    let provider = provider("23F79");
    assert_eq!(
        provider.route().provider.adapter_revision,
        APPLE_VISION_FEATURE_PRINT_ADAPTER_REVISION
    );
    assert!(
        !provider
            .route()
            .provider
            .adapter_revision
            .contains("unlinked")
    );
}
