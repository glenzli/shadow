use serde::{Deserialize, Serialize};
use shadow_domain::Platform;

use crate::{BackendRequirement, ModelAccess, ModelManifest, PrivacyClass, TaskPriority};

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BackendKind {
    Cpu,
    CoreMl,
    Metal,
    Cuda,
    WindowsMl,
    DirectMl,
    OpenVino,
    ExternalLocal,
    RemoteApi,
}

impl BackendKind {
    pub const fn is_remote(self) -> bool {
        matches!(self, Self::RemoteApi)
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum NumericPrecision {
    Float32,
    Float16,
    Bfloat16,
    Int8,
    Int4,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ExecutionBackend {
    pub id: String,
    pub kind: BackendKind,
    pub available: bool,
    pub supported_precisions: Vec<NumericPrecision>,
    /// `None` for CPU and remote providers where dedicated memory is not meaningful.
    pub total_device_memory_bytes: Option<u64>,
    pub available_device_memory_bytes: Option<u64>,
    pub maximum_concurrent_sessions: u32,
    pub active_sessions: u32,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct HardwareProfile {
    pub platform: Platform,
    pub total_system_ram_bytes: u64,
    pub available_system_ram_bytes: u64,
    pub logical_cpu_threads: u32,
    pub on_battery: bool,
    pub backends: Vec<ExecutionBackend>,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ModelAvailability {
    pub local: LocalModelAvailability,
    pub remote: RemoteModelAvailability,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "state")]
pub enum LocalModelAvailability {
    Missing,
    Installed {
        digest_verified: bool,
        license_accepted: bool,
    },
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "state")]
pub enum RemoteModelAvailability {
    Unavailable,
    Available { license_accepted: bool },
}

#[derive(Debug, Copy, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct ResourceEstimate {
    pub peak_system_ram_bytes: u64,
    pub peak_device_memory_bytes: u64,
    pub cpu_threads: u32,
    pub scratch_disk_bytes: u64,
    pub upload_bytes: u64,
    pub estimated_duration_ms: Option<u64>,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ResourcePolicy {
    pub maximum_ai_system_ram_bytes: u64,
    pub reserved_system_ram_bytes: u64,
    pub maximum_ai_cpu_threads: u32,
    /// Percent of currently available device memory that one new task may reserve.
    pub maximum_device_memory_percent: u8,
    pub remote_execution: RemoteExecutionPolicy,
    pub on_battery: OnBatteryPolicy,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RemoteExecutionPolicy {
    Disabled,
    PublicOnly,
    PersonalAllowed,
    SensitiveBiometricAllowed,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum OnBatteryPolicy {
    Continue,
    PauseBackground,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct AdmissionRequest {
    pub priority: TaskPriority,
    pub privacy: PrivacyClass,
    pub estimate: ResourceEstimate,
    pub availability: ModelAvailability,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct RunPlan {
    pub backend_id: String,
    pub backend_kind: BackendKind,
    pub precision: NumericPrecision,
    pub cpu_threads: u32,
    pub reserved_system_ram_bytes: u64,
    pub reserved_device_memory_bytes: u64,
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "reason")]
pub enum AdmissionBlocker {
    InvalidResourcePolicy,
    InvalidEstimate,
    ModelMissing,
    ArtifactDigestUnverified,
    LicenseAcceptanceRequired,
    BackgroundPausedOnBattery,
    RemoteExecutionDisabled,
    PersonalRemoteUploadDisabled,
    SensitiveBiometricRemoteUploadDisabled,
    BackendUnavailable {
        kind: BackendKind,
    },
    PrecisionUnsupported {
        kind: BackendKind,
    },
    BackendSessionLimit {
        backend_id: String,
    },
    InsufficientSystemRam {
        required: u64,
        available: u64,
    },
    InsufficientDeviceMemory {
        backend_id: String,
        required: u64,
        available: u64,
    },
    InsufficientCpuThreads {
        required: u32,
        available: u32,
    },
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "decision")]
pub enum AdmissionDecision {
    Run { plan: RunPlan },
    Defer { blockers: Vec<AdmissionBlocker> },
}

#[derive(Debug, Copy, Clone)]
struct AdmissionCapacity {
    needed_system_ram_bytes: u64,
    needed_device_memory_bytes: u64,
}

/// Selects an execution backend and admits a job without loading or running a model.
///
/// The result is deterministic for the supplied snapshots. Hardware detection,
/// model installation, and actual inference belong to platform/provider adapters.
pub fn admit(
    manifest: &ModelManifest,
    hardware: &HardwareProfile,
    policy: ResourcePolicy,
    request: AdmissionRequest,
) -> AdmissionDecision {
    let capacity = match admission_capacity(manifest, hardware, policy, request) {
        Ok(capacity) => capacity,
        Err(decision) => return decision,
    };
    let mut blockers = Vec::new();
    for target in &manifest.execution_targets {
        if let Some(blocker) = availability_blocker(target, manifest, policy, request) {
            push_unique(&mut blockers, blocker);
            continue;
        }
        match plan_for_target(target, hardware, policy, request, capacity) {
            Ok(plan) => return AdmissionDecision::Run { plan },
            Err(target_blockers) => {
                for blocker in target_blockers {
                    push_unique(&mut blockers, blocker);
                }
            }
        }
    }

    if blockers.is_empty() {
        blockers.push(AdmissionBlocker::ModelMissing);
    }
    blockers.sort();
    blockers.dedup();
    AdmissionDecision::Defer { blockers }
}

fn admission_capacity(
    manifest: &ModelManifest,
    hardware: &HardwareProfile,
    policy: ResourcePolicy,
    request: AdmissionRequest,
) -> Result<AdmissionCapacity, AdmissionDecision> {
    if policy.maximum_device_memory_percent == 0
        || policy.maximum_device_memory_percent > 100
        || policy.maximum_ai_cpu_threads == 0
    {
        return Err(defer(AdmissionBlocker::InvalidResourcePolicy));
    }
    if request.estimate.cpu_threads == 0 {
        return Err(defer(AdmissionBlocker::InvalidEstimate));
    }
    if policy.on_battery == OnBatteryPolicy::PauseBackground
        && hardware.on_battery
        && request.priority.is_background()
    {
        return Err(defer(AdmissionBlocker::BackgroundPausedOnBattery));
    }

    let needed_system_ram_bytes = request
        .estimate
        .peak_system_ram_bytes
        .max(manifest.minimum_ram_bytes);
    let available_system_ram_bytes = hardware
        .available_system_ram_bytes
        .min(policy.maximum_ai_system_ram_bytes)
        .saturating_sub(policy.reserved_system_ram_bytes);
    let available_cpu_threads = hardware
        .logical_cpu_threads
        .min(policy.maximum_ai_cpu_threads);
    let mut blockers = Vec::new();
    if needed_system_ram_bytes > available_system_ram_bytes {
        blockers.push(AdmissionBlocker::InsufficientSystemRam {
            required: needed_system_ram_bytes,
            available: available_system_ram_bytes,
        });
    }
    if request.estimate.cpu_threads > available_cpu_threads {
        blockers.push(AdmissionBlocker::InsufficientCpuThreads {
            required: request.estimate.cpu_threads,
            available: available_cpu_threads,
        });
    }
    if !blockers.is_empty() {
        return Err(AdmissionDecision::Defer { blockers });
    }
    Ok(AdmissionCapacity {
        needed_system_ram_bytes,
        needed_device_memory_bytes: request
            .estimate
            .peak_device_memory_bytes
            .max(manifest.minimum_device_memory_bytes),
    })
}

fn availability_blocker(
    target: &BackendRequirement,
    manifest: &ModelManifest,
    policy: ResourcePolicy,
    request: AdmissionRequest,
) -> Option<AdmissionBlocker> {
    if target.kind.is_remote() {
        return remote_blocker(policy, request);
    }
    match request.availability.local {
        LocalModelAvailability::Missing => Some(AdmissionBlocker::ModelMissing),
        LocalModelAvailability::Installed {
            digest_verified: false,
            ..
        } => Some(AdmissionBlocker::ArtifactDigestUnverified),
        LocalModelAvailability::Installed {
            license_accepted: false,
            ..
        } if manifest.licensing.access == ModelAccess::GatedWithAcceptance => {
            Some(AdmissionBlocker::LicenseAcceptanceRequired)
        }
        LocalModelAvailability::Installed { .. } => None,
    }
}

fn plan_for_target(
    target: &BackendRequirement,
    hardware: &HardwareProfile,
    policy: ResourcePolicy,
    request: AdmissionRequest,
    capacity: AdmissionCapacity,
) -> Result<RunPlan, Vec<AdmissionBlocker>> {
    let matching_backends: Vec<_> = hardware
        .backends
        .iter()
        .filter(|backend| backend.kind == target.kind && backend.available)
        .collect();
    if matching_backends.is_empty() {
        return Err(vec![AdmissionBlocker::BackendUnavailable {
            kind: target.kind,
        }]);
    }

    let mut blockers = Vec::new();
    for backend in matching_backends {
        if backend.active_sessions >= backend.maximum_concurrent_sessions {
            blockers.push(AdmissionBlocker::BackendSessionLimit {
                backend_id: backend.id.clone(),
            });
            continue;
        }
        let Some(precision) = target
            .precisions
            .iter()
            .find(|precision| backend.supported_precisions.contains(precision))
            .copied()
        else {
            blockers.push(AdmissionBlocker::PrecisionUnsupported { kind: target.kind });
            continue;
        };
        let reserved_device_memory_bytes =
            if let Some(available) = backend.available_device_memory_bytes {
                let permitted =
                    available.saturating_mul(u64::from(policy.maximum_device_memory_percent)) / 100;
                if capacity.needed_device_memory_bytes > permitted {
                    blockers.push(AdmissionBlocker::InsufficientDeviceMemory {
                        backend_id: backend.id.clone(),
                        required: capacity.needed_device_memory_bytes,
                        available: permitted,
                    });
                    continue;
                }
                capacity.needed_device_memory_bytes
            } else {
                0
            };
        return Ok(RunPlan {
            backend_id: backend.id.clone(),
            backend_kind: backend.kind,
            precision,
            cpu_threads: request.estimate.cpu_threads,
            reserved_system_ram_bytes: capacity.needed_system_ram_bytes,
            reserved_device_memory_bytes,
        });
    }
    Err(blockers)
}

fn remote_blocker(policy: ResourcePolicy, request: AdmissionRequest) -> Option<AdmissionBlocker> {
    if policy.remote_execution == RemoteExecutionPolicy::Disabled {
        return Some(AdmissionBlocker::RemoteExecutionDisabled);
    }
    match request.availability.remote {
        RemoteModelAvailability::Available {
            license_accepted: false,
        } => return Some(AdmissionBlocker::LicenseAcceptanceRequired),
        RemoteModelAvailability::Available { .. } => {}
        RemoteModelAvailability::Unavailable => {
            return Some(AdmissionBlocker::ModelMissing);
        }
    }
    match request.privacy {
        PrivacyClass::Public => None,
        PrivacyClass::Personal
            if matches!(
                policy.remote_execution,
                RemoteExecutionPolicy::PersonalAllowed
                    | RemoteExecutionPolicy::SensitiveBiometricAllowed
            ) =>
        {
            None
        }
        PrivacyClass::Personal => Some(AdmissionBlocker::PersonalRemoteUploadDisabled),
        PrivacyClass::SensitiveBiometric
            if policy.remote_execution == RemoteExecutionPolicy::SensitiveBiometricAllowed =>
        {
            None
        }
        PrivacyClass::SensitiveBiometric => {
            Some(AdmissionBlocker::SensitiveBiometricRemoteUploadDisabled)
        }
    }
}

fn defer(blocker: AdmissionBlocker) -> AdmissionDecision {
    AdmissionDecision::Defer {
        blockers: vec![blocker],
    }
}

fn push_unique(blockers: &mut Vec<AdmissionBlocker>, blocker: AdmissionBlocker) {
    if !blockers.contains(&blocker) {
        blockers.push(blocker);
    }
}

#[cfg(test)]
mod tests {
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
}
