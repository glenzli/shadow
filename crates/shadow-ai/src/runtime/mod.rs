//! Provider-neutral runtime identity, admission, execution, and terminal receipts.
//!
//! - [`identity`] pins the selected provider/model/framework route;
//! - [`admission`] binds a provider-neutral request to resources and that route;
//! - [`execution`] owns the move-only lease, cancellation, progress, and terminal;
//! - [`provenance`] binds successful output evidence to request, route, and plan;
//! - [`error`] keeps the shared fail-closed contract errors.

mod admission;
mod error;
mod execution;
mod identity;
mod provenance;

pub use admission::{
    AdmittedExecution, LocalExecutionAdmission, LocalExecutionBinding, admit_local_execution,
    bind_system_execution,
};
pub use error::RuntimeContractError;
pub use execution::{
    CancellationToken, ExecutionLease, LeaseBoundOutput, ProviderTerminal, ProviderUnavailable,
    RUNTIME_PROGRESS_COMPLETE, RuntimeFailure, RuntimeProgress, RuntimeProgressError,
    RuntimeProgressReporter, RuntimeProgressSink, RuntimeProvider, RuntimeTerminalOutcome,
    RuntimeTerminalReceipt, RuntimeUsage,
};
pub use identity::{
    AdmittedModelIdentity, EXECUTION_PLAN_IDENTITY_CONTRACT_VERSION,
    EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION, ExecutionPlanIdentity, ExecutionRouteIdentity,
    FallbackDisclosure, ProviderExecutionClass, ProviderIdentity,
};
pub use provenance::{MODEL_PROVENANCE_CONTRACT_VERSION, ModelProvenance};

#[cfg(test)]
mod tests;
