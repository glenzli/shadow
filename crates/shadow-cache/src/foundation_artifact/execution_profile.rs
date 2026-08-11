//! Exact execution-profile admission for verified `RawNIND` foundations.
//!
//! Artifact framing is provider-neutral, but an implementation revision may
//! never float across ONNX Runtime builds. Keep each accepted pairing explicit
//! so a newer runtime cannot silently inherit an older cache identity.

use super::FoundationExecution;

pub(super) const LEGACY_IMPLEMENTATION_REVISION: &str = "rawnind-public-bayer-foundation-v1";
pub(super) const EXPERIMENTAL_ORT127_IMPLEMENTATION_REVISION: &str =
    "rawnind-public-bayer-foundation-ort127-exp1";

struct ExecutionProfile {
    implementation_revision: &'static str,
    runtime_version: &'static str,
}

const SUPPORTED_PROFILES: [ExecutionProfile; 2] = [
    ExecutionProfile {
        implementation_revision: LEGACY_IMPLEMENTATION_REVISION,
        runtime_version: "1.24.4",
    },
    ExecutionProfile {
        implementation_revision: EXPERIMENTAL_ORT127_IMPLEMENTATION_REVISION,
        runtime_version: "onnxruntime-1.27.0",
    },
];

pub(super) fn is_supported(implementation_revision: &str, execution: &FoundationExecution) -> bool {
    execution.engine == "onnxruntime"
        && execution.requested_provider == "cpu"
        && execution.active_providers.len() == 1
        && execution.active_providers[0] == "CPUExecutionProvider"
        && SUPPORTED_PROFILES.iter().any(|profile| {
            implementation_revision == profile.implementation_revision
                && execution.runtime_version == profile.runtime_version
        })
}
