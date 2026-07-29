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

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ModelAvailability {
    pub local: LocalModelAvailability,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "state")]
pub enum LocalModelAvailability {
    Missing,
    /// Bytes are present but no package authority has verified the complete
    /// canonical inventory.
    PresentUnverified,
    Installed {
        /// Exact inventory identity verified by the package authority.
        artifact_set_blake3: String,
        license_accepted: bool,
    },
}

/// Provider estimate used by the runtime and scheduling contracts.
///
/// Current deterministic resource admission budgets only peak system RAM,
/// peak device memory, and CPU threads. Scratch bytes, upload bytes, and
/// duration remain route metadata until their own storage/network schedulers
/// admit them; a [`RunPlan`] never claims to reserve those values.
#[derive(Debug, Copy, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
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
    pub on_battery: OnBatteryPolicy,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum OnBatteryPolicy {
    Continue,
    PauseBackground,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AdmissionRequest {
    pub priority: TaskPriority,
    pub privacy: PrivacyClass,
    pub estimate: ResourceEstimate,
    pub availability: ModelAvailability,
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
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
    ArtifactSetIdentityMismatch {
        expected: String,
        installed: String,
    },
    LicenseAcceptanceRequired,
    BackgroundPausedOnBattery,
    RemoteExecutionRequiresGrant,
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
/// The result is deterministic for the supplied snapshots. It reserves only
/// RAM, device memory, and CPU threads. Hardware detection, exact artifact
/// installation, scratch/upload admission, and actual inference belong to
/// their respective application/provider authorities.
pub fn admit(
    manifest: &ModelManifest,
    hardware: &HardwareProfile,
    policy: ResourcePolicy,
    request: &AdmissionRequest,
) -> AdmissionDecision {
    let capacity = match admission_capacity(manifest, hardware, policy, request) {
        Ok(capacity) => capacity,
        Err(decision) => return decision,
    };
    let mut blockers = Vec::new();
    for target in &manifest.execution_targets {
        if let Some(blocker) = availability_blocker(target, manifest, request) {
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
    request: &AdmissionRequest,
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
    request: &AdmissionRequest,
) -> Option<AdmissionBlocker> {
    if target.kind.is_remote() {
        return Some(AdmissionBlocker::RemoteExecutionRequiresGrant);
    }
    match &request.availability.local {
        LocalModelAvailability::Missing => Some(AdmissionBlocker::ModelMissing),
        LocalModelAvailability::PresentUnverified => {
            Some(AdmissionBlocker::ArtifactDigestUnverified)
        }
        LocalModelAvailability::Installed {
            license_accepted: false,
            ..
        } if manifest.licensing.access == ModelAccess::GatedWithAcceptance => {
            Some(AdmissionBlocker::LicenseAcceptanceRequired)
        }
        LocalModelAvailability::Installed {
            artifact_set_blake3,
            ..
        } if artifact_set_blake3 != &manifest.artifact_set.inventory_blake3 => {
            Some(AdmissionBlocker::ArtifactSetIdentityMismatch {
                expected: manifest.artifact_set.inventory_blake3.clone(),
                installed: artifact_set_blake3.clone(),
            })
        }
        LocalModelAvailability::Installed { .. } => None,
    }
}

fn plan_for_target(
    target: &BackendRequirement,
    hardware: &HardwareProfile,
    policy: ResourcePolicy,
    request: &AdmissionRequest,
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
mod tests;
