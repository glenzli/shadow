use std::fmt;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};

use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::{
    AiCapability, AiObservation, AiObservationContractError, ExplanationSignal,
    ProposalReviewLevel, UnitInterval,
};

use super::{
    AdmittedExecution, ExecutionPlanIdentity, ExecutionRouteIdentity, FallbackDisclosure,
    ModelProvenance, RuntimeContractError, identity::validate_text,
};

/// Move-only permission to execute one admitted request exactly once.
#[derive(Debug)]
pub struct ExecutionLease {
    lease_id: String,
    execution: AdmittedExecution,
}

impl ExecutionLease {
    /// Issues one move-only lease after validating the serialized admission
    /// again at the runtime boundary.
    ///
    /// # Errors
    ///
    /// Returns an error for an empty lease identity or a structurally invalid
    /// admitted execution value. Application scheduling owns freshness and
    /// revocation; this portable contract does not authenticate them.
    pub fn issue(
        lease_id: String,
        execution: AdmittedExecution,
    ) -> Result<Self, RuntimeContractError> {
        validate_text(&lease_id, "lease_id")?;
        execution.validate()?;
        Ok(Self {
            lease_id,
            execution,
        })
    }

    /// Consumes this lease and emits exactly one terminal receipt.
    pub fn execute<P: RuntimeProvider>(
        self,
        provider: &P,
        cancellation: &CancellationToken,
    ) -> RuntimeTerminalReceipt<P::Output> {
        self.execute_with_progress(provider, cancellation, &DiscardProgress)
    }

    /// Consumes this lease while forwarding provider progress through a
    /// monotonic validator.
    pub fn execute_with_progress<P: RuntimeProvider>(
        self,
        provider: &P,
        cancellation: &CancellationToken,
        progress_sink: &dyn RuntimeProgressSink,
    ) -> RuntimeTerminalReceipt<P::Output> {
        let route = provider.route();
        let execution_plan = provider.execution_plan();
        let progress = RuntimeProgressReporter::new(progress_sink);
        let outcome = if route != &self.execution.route {
            ProviderTerminal::Failed {
                failure: RuntimeFailure::ExecutionRouteMismatch,
                usage: RuntimeUsage::default(),
            }
        } else if execution_plan != &self.execution.plan {
            ProviderTerminal::Failed {
                failure: RuntimeFailure::ExecutionPlanMismatch,
                usage: RuntimeUsage::default(),
            }
        } else if !provider.supports(self.execution.request.task.capability()) {
            ProviderTerminal::Failed {
                failure: RuntimeFailure::CapabilityUnsupported,
                usage: RuntimeUsage::default(),
            }
        } else if cancellation.is_cancelled() {
            ProviderTerminal::Cancelled {
                usage: RuntimeUsage::default(),
            }
        } else {
            provider.execute(&self.execution, cancellation, &progress)
        };

        let (outcome, usage) = match outcome {
            ProviderTerminal::Succeeded { output, usage } => {
                match ModelProvenance::from_admitted(&self.execution) {
                    Ok(provenance) => (
                        RuntimeTerminalOutcome::Succeeded {
                            output: Box::new(LeaseBoundOutput {
                                payload: output,
                                provenance,
                            }),
                        },
                        usage,
                    ),
                    Err(_) => (
                        RuntimeTerminalOutcome::Failed {
                            failure: RuntimeFailure::ProvenanceBindingFailed,
                        },
                        usage,
                    ),
                }
            }
            ProviderTerminal::Unavailable { reason, usage } => {
                (RuntimeTerminalOutcome::Unavailable { reason }, usage)
            }
            ProviderTerminal::Cancelled { usage } => (RuntimeTerminalOutcome::Cancelled, usage),
            ProviderTerminal::Failed { failure, usage } => {
                (RuntimeTerminalOutcome::Failed { failure }, usage)
            }
        };

        RuntimeTerminalReceipt {
            lease_id: self.lease_id,
            execution_id: self.execution.execution_id,
            request_id: self.execution.request.request_id,
            generation: self.execution.request.generation,
            route: self.execution.route,
            execution_plan: self.execution.plan,
            fallback: self.execution.fallback,
            outcome,
            usage,
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct CancellationToken {
    cancelled: Arc<AtomicBool>,
}

impl CancellationToken {
    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Release);
    }

    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }
}

/// Provider implementation boundary. Providers return data only after real
/// execution; an unlinked adapter returns `Unavailable`, never placeholder
/// output.
pub trait RuntimeProvider {
    type Output;

    /// Exact provider plus local artifact/framework/service identity this
    /// implementation is prepared to execute.
    fn route(&self) -> &ExecutionRouteIdentity;
    /// Exact backend, precision, and full admitted plan this implementation is
    /// prepared to execute.
    fn execution_plan(&self) -> &ExecutionPlanIdentity;
    fn supports(&self, capability: AiCapability) -> bool;
    fn execute(
        &self,
        execution: &AdmittedExecution,
        cancellation: &CancellationToken,
        progress: &RuntimeProgressReporter<'_>,
    ) -> ProviderTerminal<Self::Output>;
}

pub const RUNTIME_PROGRESS_COMPLETE: u16 = 10_000;

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct RuntimeProgress {
    pub phase_code: String,
    /// Normalized progress in `[0, 10_000]`, where `10_000` is terminal work
    /// completion rather than successful publication.
    pub completed_basis_points: u16,
}

pub trait RuntimeProgressSink: Send + Sync {
    fn publish(&self, progress: RuntimeProgress);
}

/// Enforces monotonic progress before an application adapter receives it.
pub struct RuntimeProgressReporter<'a> {
    sink: &'a dyn RuntimeProgressSink,
    last_basis_points: Mutex<u16>,
}

impl fmt::Debug for RuntimeProgressReporter<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let last_basis_points = self
            .last_basis_points
            .lock()
            .map_or_else(|_| "<poisoned>".to_owned(), |value| value.to_string());
        formatter
            .debug_struct("RuntimeProgressReporter")
            .field("last_basis_points", &last_basis_points)
            .finish_non_exhaustive()
    }
}

impl<'a> RuntimeProgressReporter<'a> {
    pub(super) fn new(sink: &'a dyn RuntimeProgressSink) -> Self {
        Self {
            sink,
            last_basis_points: Mutex::new(0),
        }
    }

    /// Publishes a non-regressing progress event.
    ///
    /// # Errors
    ///
    /// Returns an error for an empty phase, values above completion, or a
    /// regression relative to the previous accepted event.
    pub fn publish(&self, progress: RuntimeProgress) -> Result<(), RuntimeProgressError> {
        if progress.phase_code.trim().is_empty() {
            return Err(RuntimeProgressError::MissingPhaseCode);
        }
        if progress.completed_basis_points > RUNTIME_PROGRESS_COMPLETE {
            return Err(RuntimeProgressError::OutOfRange(
                progress.completed_basis_points,
            ));
        }
        let mut previous = self
            .last_basis_points
            .lock()
            .map_err(|_| RuntimeProgressError::StatePoisoned)?;
        if progress.completed_basis_points < *previous {
            return Err(RuntimeProgressError::Regression {
                previous: *previous,
                next: progress.completed_basis_points,
            });
        }
        *previous = progress.completed_basis_points;
        // Keep publication under the same small lock so concurrent provider
        // workers cannot pass validation in order and reach the sink reversed.
        self.sink.publish(progress);
        Ok(())
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Error)]
pub enum RuntimeProgressError {
    #[error("runtime progress phase_code must not be empty")]
    MissingPhaseCode,
    #[error("runtime progress {0} exceeds 10,000 basis points")]
    OutOfRange(u16),
    #[error("runtime progress regressed from {previous} to {next} basis points")]
    Regression { previous: u16, next: u16 },
    #[error("runtime progress state is poisoned")]
    StatePoisoned,
}

#[derive(Debug)]
struct DiscardProgress;

impl RuntimeProgressSink for DiscardProgress {
    fn publish(&self, _progress: RuntimeProgress) {}
}

#[derive(Debug, Clone, PartialEq)]
pub enum ProviderTerminal<T> {
    Succeeded {
        output: T,
        usage: RuntimeUsage,
    },
    Unavailable {
        reason: ProviderUnavailable,
        usage: RuntimeUsage,
    },
    Cancelled {
        usage: RuntimeUsage,
    },
    Failed {
        failure: RuntimeFailure,
        usage: RuntimeUsage,
    },
}

#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Serialize, Deserialize)]
pub struct RuntimeUsage {
    pub elapsed_ms: u64,
    pub peak_system_ram_bytes: u64,
    pub peak_device_memory_bytes: u64,
    pub uploaded_bytes: u64,
    pub downloaded_bytes: u64,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "reason")]
pub enum ProviderUnavailable {
    PlatformUnsupported,
    AdapterNotLinked,
    ModelNotInstalled,
    Offline,
    LicenseNotAdmitted,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "failure")]
pub enum RuntimeFailure {
    ExecutionRouteMismatch,
    ExecutionPlanMismatch,
    CapabilityUnsupported,
    ProvenanceBindingFailed,
    InvalidProviderOutput { detail: String },
    ProviderError { code: String },
}

/// Successful provider bytes plus runtime-issued provenance authority.
///
/// This type is move-only and non-deserializable. A provider returns only its
/// payload; the consumed [`ExecutionLease`] constructs this envelope.
#[derive(Debug)]
pub struct LeaseBoundOutput<T> {
    payload: T,
    provenance: ModelProvenance,
}

impl<T> LeaseBoundOutput<T> {
    pub const fn payload(&self) -> &T {
        &self.payload
    }

    pub const fn provenance(&self) -> &ModelProvenance {
        &self.provenance
    }

    /// Consumes successful runtime authority and constructs one exact-v1
    /// observation whose request identity cannot be supplied independently.
    ///
    /// # Errors
    ///
    /// Returns an error for malformed or oversized explanation signals.
    pub fn into_observation(
        self,
        confidence: UnitInterval,
        review_level: ProposalReviewLevel,
        explanation_signals: Vec<ExplanationSignal>,
    ) -> Result<AiObservation<T>, AiObservationContractError> {
        AiObservation::from_runtime_output(self, confidence, review_level, explanation_signals)
    }

    pub(crate) fn into_parts(self) -> (T, ModelProvenance) {
        (self.payload, self.provenance)
    }
}

#[derive(Debug)]
pub enum RuntimeTerminalOutcome<T> {
    Succeeded { output: Box<LeaseBoundOutput<T>> },
    Unavailable { reason: ProviderUnavailable },
    Cancelled,
    Failed { failure: RuntimeFailure },
}

/// One and only one terminal publication for a consumed execution lease.
#[derive(Debug)]
pub struct RuntimeTerminalReceipt<T> {
    pub lease_id: String,
    pub execution_id: String,
    pub request_id: String,
    pub generation: u64,
    pub route: ExecutionRouteIdentity,
    pub execution_plan: ExecutionPlanIdentity,
    pub fallback: FallbackDisclosure,
    pub outcome: RuntimeTerminalOutcome<T>,
    pub usage: RuntimeUsage,
}
