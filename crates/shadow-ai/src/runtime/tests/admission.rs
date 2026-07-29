use std::collections::BTreeSet;

use crate::{
    AdmittedModelIdentity, AiCapability, AiJobRequest, BackendKind, BackendRequirement,
    EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION, ExecutionBackend, ExecutionRouteIdentity,
    FallbackDisclosure, LocalExecutionAdmission, LocalExecutionBinding, NumericPrecision,
    ProviderExecutionClass, ProviderIdentity, RunPlan, RuntimeContractError, admit_local_execution,
    bind_system_execution,
};

use super::fixtures::{
    admitted_execution, artifact_set, availability, hardware, manifest, policy, request,
    route_estimate,
};

#[test]
fn request_intent_contains_no_provider_or_model_selection() {
    let json = serde_json::to_value(request()).expect("serialize request");
    assert!(json.get("model_id").is_none());
    assert!(json.get("model_revision").is_none());
    assert!(json.get("provider_id").is_none());

    let mut stale = json;
    stale
        .as_object_mut()
        .expect("request object")
        .insert("model_id".into(), serde_json::Value::String("stale".into()));
    assert!(serde_json::from_value::<AiJobRequest>(stale).is_err());
}

#[test]
fn admission_binds_the_request_to_one_exact_artifact_set() {
    let provider = ProviderIdentity {
        provider_id: "shadow.onnx".into(),
        adapter_revision: "v1".into(),
        execution_class: ProviderExecutionClass::LocalModel,
    };
    let decision = admit_local_execution(
        LocalExecutionBinding {
            execution_id: "execution-1".into(),
            request: request(),
            provider,
            route_estimate: route_estimate(),
            fallback: FallbackDisclosure::Primary,
        },
        &manifest(),
        &hardware(),
        policy(),
        availability(),
    )
    .expect("valid admission");
    let LocalExecutionAdmission::Admitted { execution } = decision else {
        panic!("expected admitted execution");
    };

    assert!(matches!(
        execution.route().model,
        AdmittedModelIdentity::LocalArtifactSet {
            ref artifact_set_blake3,
            ..
        } if artifact_set_blake3 == &artifact_set().inventory_blake3
    ));
    assert_eq!(execution.route_estimate(), route_estimate());
    assert_eq!(execution.plan().reserved_system_ram_bytes, 96);
}

#[test]
fn local_execution_can_never_bind_a_remote_backend_plan() {
    let mut remote_manifest = manifest();
    remote_manifest.execution_targets = vec![BackendRequirement {
        kind: BackendKind::RemoteApi,
        minimum_runtime_version: None,
        precisions: vec![NumericPrecision::Float32],
    }];
    let mut remote_hardware = hardware();
    remote_hardware.backends = vec![ExecutionBackend {
        id: "remote".into(),
        kind: BackendKind::RemoteApi,
        available: true,
        supported_precisions: vec![NumericPrecision::Float32],
        total_device_memory_bytes: None,
        available_device_memory_bytes: None,
        maximum_concurrent_sessions: 1,
        active_sessions: 0,
    }];
    let decision = admit_local_execution(
        LocalExecutionBinding {
            execution_id: "execution-remote-backend".into(),
            request: request(),
            provider: ProviderIdentity {
                provider_id: "shadow.onnx".into(),
                adapter_revision: "v1".into(),
                execution_class: ProviderExecutionClass::LocalModel,
            },
            route_estimate: route_estimate(),
            fallback: FallbackDisclosure::Primary,
        },
        &remote_manifest,
        &remote_hardware,
        policy(),
        availability(),
    )
    .expect("remote routing is a policy deferral, not a malformed binding");
    assert!(matches!(
        decision,
        LocalExecutionAdmission::Deferred { blockers }
            if blockers == vec![crate::AdmissionBlocker::RemoteExecutionRequiresGrant]
    ));
}

#[test]
fn admitted_execution_exposes_the_exact_plan_identity() {
    let execution = admitted_execution();
    assert_eq!(&execution.plan_identity().plan, execution.plan());
    assert_eq!(
        execution.plan_identity().contract_version,
        crate::EXECUTION_PLAN_IDENTITY_CONTRACT_VERSION
    );
    assert_eq!(execution.plan_identity().plan_blake3.len(), 64);
    assert!(execution.plan_identity().validate().is_ok());
}

#[test]
fn route_and_plan_wire_contracts_reject_unknown_or_tampered_identity() {
    let execution = admitted_execution();

    let mut unknown_provider_fact =
        serde_json::to_value(execution.route()).expect("serialize route");
    unknown_provider_fact["provider"]["endpoint"] = serde_json::json!("https://untrusted.invalid");
    assert!(serde_json::from_value::<ExecutionRouteIdentity>(unknown_provider_fact).is_err());

    let mut unsupported_route = serde_json::to_value(execution.route()).expect("serialize route");
    unsupported_route["contract_version"] =
        serde_json::json!(EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION + 1);
    assert!(serde_json::from_value::<ExecutionRouteIdentity>(unsupported_route).is_err());

    let mut omitted_plan_fact =
        serde_json::to_value(execution.plan_identity()).expect("serialize plan");
    omitted_plan_fact["plan"]["untracked_budget"] = serde_json::json!(1);
    assert!(serde_json::from_value::<crate::ExecutionPlanIdentity>(omitted_plan_fact).is_err());

    let mut stale_plan_digest =
        serde_json::to_value(execution.plan_identity()).expect("serialize plan");
    stale_plan_digest["plan"]["cpu_threads"] = serde_json::json!(3);
    assert!(serde_json::from_value::<crate::ExecutionPlanIdentity>(stale_plan_digest).is_err());
}

#[test]
fn local_execution_binding_preserves_the_full_resource_plan() {
    let execution = admitted_execution();
    assert_eq!(
        execution.plan(),
        &RunPlan {
            backend_id: "cpu".into(),
            backend_kind: BackendKind::Cpu,
            precision: NumericPrecision::Float32,
            cpu_threads: 2,
            reserved_system_ram_bytes: 96,
            reserved_device_memory_bytes: 0,
        }
    );
}

#[test]
fn system_framework_execution_can_never_bind_a_remote_backend_plan() {
    let route = ExecutionRouteIdentity {
        contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
        provider: ProviderIdentity {
            provider_id: "system.framework".into(),
            adapter_revision: "adapter-v1".into(),
            execution_class: ProviderExecutionClass::SystemFramework,
        },
        model: AdmittedModelIdentity::SystemFramework {
            framework: "SystemVision".into(),
            request_name: "FeatureRequest".into(),
            request_revision: 1,
            operating_system_build: "test-build".into(),
        },
    };
    let error = bind_system_execution(
        "system-remote-backend".into(),
        request(),
        route,
        &BTreeSet::from([AiCapability::SimilarityEmbedding]),
        RunPlan {
            backend_id: "remote".into(),
            backend_kind: BackendKind::RemoteApi,
            precision: NumericPrecision::Float32,
            cpu_threads: 2,
            reserved_system_ram_bytes: 96,
            reserved_device_memory_bytes: 0,
        },
        route_estimate(),
        FallbackDisclosure::Primary,
    )
    .expect_err("system route cannot use RemoteApi");
    assert_eq!(
        error,
        RuntimeContractError::BackendExecutionClassMismatch {
            execution_class: ProviderExecutionClass::SystemFramework,
            backend_kind: BackendKind::RemoteApi,
        }
    );
}

#[test]
fn fallback_selection_can_never_upgrade_to_a_remote_route() {
    let selected = ExecutionRouteIdentity {
        contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
        provider: ProviderIdentity {
            provider_id: "example.remote".into(),
            adapter_revision: "remote-v1".into(),
            execution_class: ProviderExecutionClass::RemoteService,
        },
        model: AdmittedModelIdentity::RemoteService {
            service_revision: "2026-07".into(),
            model_id: "remote-model".into(),
            model_revision: "v1".into(),
            api_contract_revision: "v1".into(),
        },
    };
    let primary = admitted_execution().route().clone();
    assert_eq!(
        crate::runtime::admission::validate_fallback(
            &selected,
            &FallbackDisclosure::DeclaredFallback {
                primary: Box::new(primary),
                reason_code: "local_unavailable".into(),
            },
        ),
        Err(RuntimeContractError::FallbackSelectsRemoteRoute)
    );
}
