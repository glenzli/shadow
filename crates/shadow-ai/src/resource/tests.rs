use std::collections::BTreeSet;

use crate::{
    AiCapability, BackendRequirement, DistributionTerms, LicensePermission, LicenseTerms,
    ModelAccess, ModelArtifact, ModelFormat, Quantization,
};

use super::*;

const GIB: u64 = 1024 * 1024 * 1024;

fn manifest(targets: Vec<BackendKind>) -> ModelManifest {
    ModelManifest {
        schema_version: 1,
        model_id: "test".into(),
        exact_revision: "r1".into(),
        artifact: ModelArtifact {
            filename: "model.onnx".into(),
            byte_len: 1,
            sha256: "a".repeat(64),
        },
        capabilities: BTreeSet::from([AiCapability::SimilarityEmbedding]),
        format: ModelFormat::Onnx,
        opset: Some(18),
        quantization: Quantization::Float16,
        preprocessing_version: "v1".into(),
        inputs: vec![],
        outputs: vec![],
        execution_targets: targets
            .into_iter()
            .map(|kind| BackendRequirement {
                kind,
                minimum_runtime_version: None,
                precisions: vec![NumericPrecision::Float16, NumericPrecision::Float32],
            })
            .collect(),
        minimum_ram_bytes: GIB,
        recommended_ram_bytes: 2 * GIB,
        minimum_device_memory_bytes: 2 * GIB,
        recommended_device_memory_bytes: 3 * GIB,
        licensing: LicenseTerms {
            code_license: "MIT".into(),
            weight_license: "MIT".into(),
            training_data_notes: None,
            redistribution: LicensePermission::Allowed,
            commercial_use: LicensePermission::Allowed,
            access: ModelAccess::Open,
            attribution_files: vec![],
        },
        distribution: DistributionTerms {
            bundled_by_default: false,
            automatic_download_allowed: false,
            side_load_allowed: true,
            upstream_url: "https://example.invalid".into(),
        },
    }
}

fn hardware(backends: Vec<ExecutionBackend>) -> HardwareProfile {
    HardwareProfile {
        platform: Platform::MacOs,
        total_system_ram_bytes: 32 * GIB,
        available_system_ram_bytes: 20 * GIB,
        logical_cpu_threads: 10,
        on_battery: false,
        backends,
    }
}

fn backend(id: &str, kind: BackendKind, available_memory: Option<u64>) -> ExecutionBackend {
    ExecutionBackend {
        id: id.into(),
        kind,
        available: true,
        supported_precisions: vec![NumericPrecision::Float16, NumericPrecision::Float32],
        total_device_memory_bytes: available_memory,
        available_device_memory_bytes: available_memory,
        maximum_concurrent_sessions: 1,
        active_sessions: 0,
    }
}

fn policy() -> ResourcePolicy {
    ResourcePolicy {
        maximum_ai_system_ram_bytes: 10 * GIB,
        reserved_system_ram_bytes: 2 * GIB,
        maximum_ai_cpu_threads: 8,
        maximum_device_memory_percent: 75,
        remote_execution: RemoteExecutionPolicy::Disabled,
        on_battery: OnBatteryPolicy::PauseBackground,
    }
}

fn request() -> AdmissionRequest {
    AdmissionRequest {
        priority: TaskPriority::CurrentCollectionAnalysis,
        privacy: PrivacyClass::Personal,
        estimate: ResourceEstimate {
            peak_system_ram_bytes: 2 * GIB,
            peak_device_memory_bytes: 2 * GIB,
            cpu_threads: 4,
            ..ResourceEstimate::default()
        },
        availability: ModelAvailability {
            local: LocalModelAvailability::Installed {
                digest_verified: true,
                license_accepted: true,
            },
            remote: RemoteModelAvailability::Unavailable,
        },
    }
}

#[test]
fn chooses_first_compatible_local_backend() {
    let decision = admit(
        &manifest(vec![BackendKind::CoreMl, BackendKind::Cpu]),
        &hardware(vec![
            backend("coreml:0", BackendKind::CoreMl, Some(10 * GIB)),
            backend("cpu", BackendKind::Cpu, None),
        ]),
        policy(),
        request(),
    );
    assert!(matches!(
        decision,
        AdmissionDecision::Run {
            plan: RunPlan {
                backend_kind: BackendKind::CoreMl,
                ..
            }
        }
    ));
}

#[test]
fn can_fall_back_from_an_unavailable_local_target_to_remote() {
    let mut fallback_request = request();
    fallback_request.availability.remote = RemoteModelAvailability::Available {
        license_accepted: true,
    };
    let mut fallback_policy = policy();
    fallback_policy.remote_execution = RemoteExecutionPolicy::PersonalAllowed;
    let decision = admit(
        &manifest(vec![BackendKind::CoreMl, BackendKind::RemoteApi]),
        &hardware(vec![backend("api", BackendKind::RemoteApi, None)]),
        fallback_policy,
        fallback_request,
    );
    assert!(matches!(
        decision,
        AdmissionDecision::Run {
            plan: RunPlan {
                backend_kind: BackendKind::RemoteApi,
                ..
            }
        }
    ));
}

#[test]
fn leaves_headroom_in_device_memory() {
    let decision = admit(
        &manifest(vec![BackendKind::Cuda]),
        &hardware(vec![backend("cuda:0", BackendKind::Cuda, Some(2 * GIB))]),
        policy(),
        request(),
    );
    assert!(matches!(
        decision,
        AdmissionDecision::Defer { blockers }
            if blockers.iter().any(|blocker| matches!(
                blocker,
                AdmissionBlocker::InsufficientDeviceMemory { .. }
            ))
    ));
}

#[test]
fn never_uploads_sensitive_faces_without_explicit_policy() {
    let mut remote_request = request();
    remote_request.privacy = PrivacyClass::SensitiveBiometric;
    remote_request.availability = ModelAvailability {
        local: LocalModelAvailability::Missing,
        remote: RemoteModelAvailability::Available {
            license_accepted: true,
        },
    };
    let mut remote_policy = policy();
    remote_policy.remote_execution = RemoteExecutionPolicy::PublicOnly;
    let decision = admit(
        &manifest(vec![BackendKind::RemoteApi]),
        &hardware(vec![backend("api", BackendKind::RemoteApi, None)]),
        remote_policy,
        remote_request,
    );
    assert!(matches!(
        decision,
        AdmissionDecision::Defer { blockers }
            if blockers.contains(&AdmissionBlocker::SensitiveBiometricRemoteUploadDisabled)
    ));
}

#[test]
fn pauses_library_work_on_battery() {
    let mut profile = hardware(vec![backend(
        "coreml:0",
        BackendKind::CoreMl,
        Some(10 * GIB),
    )]);
    profile.on_battery = true;
    let mut background = request();
    background.priority = TaskPriority::LibraryBackgroundAnalysis;
    assert_eq!(
        admit(
            &manifest(vec![BackendKind::CoreMl]),
            &profile,
            policy(),
            background,
        ),
        AdmissionDecision::Defer {
            blockers: vec![AdmissionBlocker::BackgroundPausedOnBattery]
        }
    );
}
