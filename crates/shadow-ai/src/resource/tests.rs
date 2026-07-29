use std::collections::BTreeSet;

use crate::{
    AiCapability, BackendRequirement, DistributionTerms, LicensePermission, LicenseTerms,
    ModelAccess, ModelArtifact, ModelArtifactRole, ModelArtifactSet, ModelFormat, Quantization,
};

use super::*;

const GIB: u64 = 1024 * 1024 * 1024;

fn artifact_set() -> ModelArtifactSet {
    let mut set = ModelArtifactSet {
        set_id: "test-r1".into(),
        inventory_blake3: String::new(),
        artifacts: vec![ModelArtifact {
            relative_path: "model.onnx".into(),
            role: ModelArtifactRole::ModelDefinition,
            byte_len: 1,
            sha256: "a".repeat(64),
        }],
    };
    set.inventory_blake3 = set
        .computed_inventory_blake3()
        .expect("valid artifact fixture");
    set
}

fn manifest(targets: Vec<BackendKind>) -> ModelManifest {
    ModelManifest {
        schema_version: 1,
        model_id: "test".into(),
        exact_revision: "r1".into(),
        artifact_set: artifact_set(),
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
                artifact_set_blake3: artifact_set().inventory_blake3,
                license_accepted: true,
            },
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
        &request(),
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
fn local_manifest_planner_never_authorizes_a_remote_backend() {
    let decision = admit(
        &manifest(vec![BackendKind::CoreMl, BackendKind::RemoteApi]),
        &hardware(vec![backend("api", BackendKind::RemoteApi, None)]),
        policy(),
        &request(),
    );
    assert_eq!(
        decision,
        AdmissionDecision::Defer {
            blockers: vec![
                AdmissionBlocker::RemoteExecutionRequiresGrant,
                AdmissionBlocker::BackendUnavailable {
                    kind: BackendKind::CoreMl,
                },
            ],
        }
    );
}

#[test]
fn leaves_headroom_in_device_memory() {
    let decision = admit(
        &manifest(vec![BackendKind::Cuda]),
        &hardware(vec![backend("cuda:0", BackendKind::Cuda, Some(2 * GIB))]),
        policy(),
        &request(),
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
fn remote_backend_is_blocked_even_when_local_model_bytes_are_installed() {
    let decision = admit(
        &manifest(vec![BackendKind::RemoteApi]),
        &hardware(vec![backend("api", BackendKind::RemoteApi, None)]),
        policy(),
        &request(),
    );
    assert_eq!(
        decision,
        AdmissionDecision::Defer {
            blockers: vec![AdmissionBlocker::RemoteExecutionRequiresGrant],
        }
    );
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
            &background,
        ),
        AdmissionDecision::Defer {
            blockers: vec![AdmissionBlocker::BackgroundPausedOnBattery]
        }
    );
}

#[test]
fn installed_bytes_for_another_artifact_set_are_not_admitted() {
    let mut wrong = request();
    wrong.availability.local = LocalModelAvailability::Installed {
        artifact_set_blake3: "0".repeat(64),
        license_accepted: true,
    };
    let expected = artifact_set().inventory_blake3;
    assert_eq!(
        admit(
            &manifest(vec![BackendKind::Cpu]),
            &hardware(vec![backend("cpu", BackendKind::Cpu, None)]),
            policy(),
            &wrong,
        ),
        AdmissionDecision::Defer {
            blockers: vec![AdmissionBlocker::ArtifactSetIdentityMismatch {
                expected,
                installed: "0".repeat(64),
            }]
        }
    );
}

#[test]
fn scratch_upload_and_duration_are_metadata_not_false_run_plan_reservations() {
    let mut metadata_heavy = request();
    metadata_heavy.estimate.scratch_disk_bytes = u64::MAX;
    metadata_heavy.estimate.upload_bytes = u64::MAX;
    metadata_heavy.estimate.estimated_duration_ms = Some(u64::MAX);
    let decision = admit(
        &manifest(vec![BackendKind::Cpu]),
        &hardware(vec![backend("cpu", BackendKind::Cpu, None)]),
        policy(),
        &metadata_heavy,
    );
    assert!(matches!(
        decision,
        AdmissionDecision::Run {
            plan: RunPlan {
                backend_kind: BackendKind::Cpu,
                cpu_threads: 4,
                reserved_system_ram_bytes,
                reserved_device_memory_bytes: 0,
                ..
            }
        } if reserved_system_ram_bytes == 2 * GIB
    ));
}
