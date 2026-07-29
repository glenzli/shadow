use thiserror::Error;

use crate::{AiArtifactContractError, AiCapability, BackendKind, ModelManifestError};

use super::ProviderExecutionClass;

#[derive(Debug, Clone, PartialEq, Error)]
pub enum RuntimeContractError {
    #[error("AI request contract version {0} is not supported")]
    UnsupportedRequestContract(u32),
    #[error("execution route identity contract version {0} is not supported")]
    UnsupportedRouteIdentityContract(u32),
    #[error("execution plan identity contract version {0} is not supported")]
    UnsupportedPlanIdentityContract(u32),
    #[error("model provenance contract version {0} is not supported")]
    UnsupportedProvenanceContract(u32),
    #[error("runtime field {0} must not be empty")]
    MissingText(&'static str),
    #[error("request privacy understates at least one input artifact")]
    RequestUnderstatesInputPrivacy,
    #[error("provider class {actual:?} cannot satisfy route class {expected:?}")]
    WrongProviderClass {
        expected: ProviderExecutionClass,
        actual: ProviderExecutionClass,
    },
    #[error("provider execution class and model identity disagree")]
    ProviderModelClassMismatch,
    #[error("capability {0:?} is not supported by the selected route")]
    CapabilityUnsupported(AiCapability),
    #[error("system-framework request revision must be non-zero")]
    InvalidSystemRequestRevision,
    #[error("artifact-set digest must be a 256-bit hexadecimal digest")]
    InvalidArtifactSetDigest,
    #[error("execution plan must name a backend and reserve at least one CPU thread")]
    InvalidExecutionPlan,
    #[error("execution plan identity does not match its complete admitted plan")]
    ExecutionPlanIdentityMismatch,
    #[error("runtime could not encode a canonical execution identity")]
    CanonicalIdentityEncodingFailed,
    #[error("model provenance cache key does not bind its request, route, and plan")]
    ProvenanceCacheKeyMismatch,
    #[error(
        "execution class {execution_class:?} is incompatible with admitted backend {backend_kind:?}"
    )]
    BackendExecutionClassMismatch {
        execution_class: ProviderExecutionClass,
        backend_kind: BackendKind,
    },
    #[error("provider route estimate must request at least one CPU thread")]
    InvalidRouteEstimate,
    #[error("admitted run plan reserves less than the selected provider route estimate")]
    RoutePlanUnderreserves,
    #[error("a fallback cannot name the already-selected route as its primary")]
    FallbackRepeatsSelectedRoute,
    #[error("a fallback may not select a remote route")]
    FallbackSelectsRemoteRoute,
    #[error(transparent)]
    InvalidTaskParameters(#[from] AiArtifactContractError),
    #[error(transparent)]
    InvalidManifest(#[from] ModelManifestError),
}
