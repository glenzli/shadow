//! Session-local job ownership for AI RAW foundation materialization.
//!
//! Qt may run the blocking runtime call on its worker pool while polling this
//! small registry from the UI thread. Cancellation, progress, terminal state,
//! and the verified ready descriptor remain keyed by an opaque token. The
//! rebuildable cache path never crosses into Recipe persistence.

use std::{
    collections::{BTreeMap, VecDeque},
    fmt,
    path::Path,
    sync::{
        Mutex,
        atomic::{AtomicU64, Ordering},
    },
};

use shadow_ai::{
    CancellationToken, RUNTIME_PROGRESS_COMPLETE, RawFoundationMaterializationDisposition,
    RuntimeProgress, RuntimeProgressSink,
};
use shadow_catalog::RepresentationFingerprint;
use thiserror::Error;

use crate::raw_foundation_runtime::{
    RawFoundationInvocation, RawFoundationReady, RawFoundationRuntime, RawFoundationRuntimeError,
    RawFoundationRuntimeOutcome,
};

const MAX_RAW_FOUNDATION_JOBS: usize = 32;
const MAX_READY_RAW_FOUNDATIONS: usize = 16;

#[derive(Debug)]
pub(crate) struct RawFoundationService {
    next_job_token: AtomicU64,
    jobs: Mutex<BTreeMap<u64, RawFoundationJob>>,
    ready_foundations: Mutex<VecDeque<RawFoundationReady>>,
    cache_resolution: Mutex<()>,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) enum RawFoundationJobPhase {
    Queued,
    Planning,
    Running,
    Ready,
    Unavailable,
    Cancelled,
    Failed,
}

impl RawFoundationJobPhase {
    pub(crate) const fn is_terminal(self) -> bool {
        matches!(
            self,
            Self::Ready | Self::Unavailable | Self::Cancelled | Self::Failed
        )
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationJobSnapshot {
    pub(crate) token: u64,
    pub(crate) request_id: String,
    pub(crate) generation: u64,
    pub(crate) phase: RawFoundationJobPhase,
    pub(crate) phase_code: String,
    pub(crate) completed_basis_points: u16,
    pub(crate) cancellation_requested: bool,
    pub(crate) disposition: Option<RawFoundationMaterializationDisposition>,
    pub(crate) diagnostic: String,
}

#[derive(Debug)]
struct RawFoundationJob {
    request_id: String,
    generation: u64,
    cancellation: CancellationToken,
    phase: RawFoundationJobPhase,
    phase_code: String,
    completed_basis_points: u16,
    result: Option<RawFoundationJobResult>,
}

#[derive(Debug)]
enum RawFoundationJobResult {
    Ready(Box<RawFoundationReady>),
    Unavailable(String),
    Cancelled,
    Failed(String),
}

impl RawFoundationService {
    pub(crate) fn new() -> Self {
        Self {
            next_job_token: AtomicU64::new(0),
            jobs: Mutex::new(BTreeMap::new()),
            ready_foundations: Mutex::new(VecDeque::new()),
            cache_resolution: Mutex::new(()),
        }
    }

    pub(crate) fn begin_job(
        &self,
        request_id: String,
        generation: u64,
    ) -> Result<u64, RawFoundationServiceError> {
        if request_id.trim().is_empty() {
            return Err(RawFoundationServiceError::InvalidRequestIdentity);
        }
        let mut jobs = self.jobs_guard()?;
        if jobs.len() >= MAX_RAW_FOUNDATION_JOBS {
            return Err(RawFoundationServiceError::TooManyJobs);
        }
        if jobs.values().any(|job| {
            job.request_id == request_id && job.generation == generation && !job.phase.is_terminal()
        }) {
            return Err(RawFoundationServiceError::DuplicateActiveRequest);
        }
        let token = self
            .next_job_token
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |current| {
                current.checked_add(1)
            })
            .map_err(|_| RawFoundationServiceError::TokenExhausted)?
            + 1;
        jobs.insert(
            token,
            RawFoundationJob {
                request_id,
                generation,
                cancellation: CancellationToken::default(),
                phase: RawFoundationJobPhase::Queued,
                phase_code: "queued".into(),
                completed_basis_points: 0,
                result: None,
            },
        );
        Ok(token)
    }

    pub(crate) fn execute_job(
        &self,
        runtime: &RawFoundationRuntime,
        token: u64,
        invocation: RawFoundationInvocation,
    ) -> Result<RawFoundationJobSnapshot, RawFoundationServiceError> {
        self.execute_job_with(token, invocation, |invocation, cancellation, progress| {
            runtime.materialize(&invocation, cancellation, progress)
        })
    }

    pub(crate) fn cancel_job(&self, token: u64) -> Result<(), RawFoundationServiceError> {
        let jobs = self.jobs_guard()?;
        let job = jobs
            .get(&token)
            .ok_or(RawFoundationServiceError::UnknownJob(token))?;
        if !job.phase.is_terminal() {
            job.cancellation.cancel();
        }
        Ok(())
    }

    pub(crate) fn snapshot(
        &self,
        token: u64,
    ) -> Result<RawFoundationJobSnapshot, RawFoundationServiceError> {
        let jobs = self.jobs_guard()?;
        let job = jobs
            .get(&token)
            .ok_or(RawFoundationServiceError::UnknownJob(token))?;
        Ok(job_snapshot(token, job))
    }

    pub(crate) fn ready_foundation(
        &self,
        token: u64,
    ) -> Result<Option<RawFoundationReady>, RawFoundationServiceError> {
        let jobs = self.jobs_guard()?;
        let job = jobs
            .get(&token)
            .ok_or(RawFoundationServiceError::UnknownJob(token))?;
        Ok(match &job.result {
            Some(RawFoundationJobResult::Ready(ready)) => Some((**ready).clone()),
            Some(
                RawFoundationJobResult::Unavailable(_)
                | RawFoundationJobResult::Cancelled
                | RawFoundationJobResult::Failed(_),
            )
            | None => None,
        })
    }

    /// Returns the newest verified result for this exact Catalog source.
    ///
    /// This small session registry deliberately outlives the UI job token. It
    /// holds only rebuildable descriptor/path state; Recipe persistence never
    /// receives the cache path.
    pub(crate) fn ready_for_source(
        &self,
        source_path: &Path,
        source: RepresentationFingerprint,
    ) -> Result<Option<RawFoundationReady>, RawFoundationServiceError> {
        let ready = self.ready_foundations_guard()?;
        Ok(ready
            .iter()
            .rev()
            .find(|candidate| candidate.source_path == source_path && candidate.source == source)
            .cloned())
    }

    /// Resolves an exact session-ready value, consulting the persistent
    /// verified cache at most once concurrently on an in-memory miss.
    pub(crate) fn resolve_ready_for_source(
        &self,
        runtime: &RawFoundationRuntime,
        source_path: &Path,
        source: RepresentationFingerprint,
        cancellation: &CancellationToken,
    ) -> Result<Option<RawFoundationReady>, RawFoundationServiceError> {
        if let Some(ready) = self.ready_for_source(source_path, source)? {
            return Ok(Some(ready));
        }
        let _resolution = self
            .cache_resolution
            .lock()
            .map_err(|_| RawFoundationServiceError::StatePoisoned)?;
        if let Some(ready) = self.ready_for_source(source_path, source)? {
            return Ok(Some(ready));
        }
        let Some(ready) = runtime.resolve_cached(source_path, cancellation)? else {
            return Ok(None);
        };
        if ready.source_path != source_path || ready.source != source {
            return Err(RawFoundationServiceError::ResolvedSourceMismatch);
        }
        self.remember_ready(ready.clone())?;
        Ok(Some(ready))
    }

    pub(crate) fn retire_job(&self, token: u64) -> Result<(), RawFoundationServiceError> {
        let mut jobs = self.jobs_guard()?;
        let job = jobs
            .get(&token)
            .ok_or(RawFoundationServiceError::UnknownJob(token))?;
        if !job.phase.is_terminal() {
            return Err(RawFoundationServiceError::JobStillActive(token));
        }
        jobs.remove(&token);
        Ok(())
    }

    /// Completes a registered job when Catalog/source admission fails before
    /// the runtime invocation can begin. Once Qt receives a token, every
    /// outcome must become pollable and retireable; otherwise one bad source
    /// can leak registry capacity for the rest of the desktop session.
    pub(crate) fn complete_preflight_failure(
        &self,
        token: u64,
        diagnostic: String,
    ) -> Result<RawFoundationJobSnapshot, RawFoundationServiceError> {
        let mut jobs = self.jobs_guard()?;
        let job = jobs
            .get_mut(&token)
            .ok_or(RawFoundationServiceError::UnknownJob(token))?;
        if job.phase != RawFoundationJobPhase::Queued {
            return Err(RawFoundationServiceError::JobAlreadyExecuted(token));
        }
        let result = if job.cancellation.is_cancelled() {
            RawFoundationJobResult::Cancelled
        } else {
            RawFoundationJobResult::Failed(diagnostic)
        };
        apply_terminal(job, result);
        Ok(job_snapshot(token, job))
    }

    fn execute_job_with<E>(
        &self,
        token: u64,
        invocation: RawFoundationInvocation,
        execute: impl FnOnce(
            RawFoundationInvocation,
            &CancellationToken,
            &dyn RuntimeProgressSink,
        ) -> Result<RawFoundationRuntimeOutcome, E>,
    ) -> Result<RawFoundationJobSnapshot, RawFoundationServiceError>
    where
        E: fmt::Display,
    {
        let cancellation = {
            let mut jobs = self.jobs_guard()?;
            let job = jobs
                .get_mut(&token)
                .ok_or(RawFoundationServiceError::UnknownJob(token))?;
            if job.phase != RawFoundationJobPhase::Queued {
                return Err(RawFoundationServiceError::JobAlreadyExecuted(token));
            }
            if job.request_id != invocation.request_id || job.generation != invocation.generation {
                return Err(RawFoundationServiceError::InvocationIdentityMismatch);
            }
            job.phase = RawFoundationJobPhase::Planning;
            job.phase_code = "planning".into();
            job.cancellation.clone()
        };
        let progress = RawFoundationJobProgress {
            service: self,
            token,
        };
        let outcome = if cancellation.is_cancelled() {
            Ok(RawFoundationRuntimeOutcome::Cancelled)
        } else {
            execute(invocation, &cancellation, &progress)
        };
        self.complete_job(token, cancellation.is_cancelled(), outcome)
    }

    fn complete_job<E>(
        &self,
        token: u64,
        cancellation_requested: bool,
        outcome: Result<RawFoundationRuntimeOutcome, E>,
    ) -> Result<RawFoundationJobSnapshot, RawFoundationServiceError>
    where
        E: fmt::Display,
    {
        let mut jobs = self.jobs_guard()?;
        let job = jobs
            .get_mut(&token)
            .ok_or(RawFoundationServiceError::UnknownJob(token))?;
        let result = if cancellation_requested {
            RawFoundationJobResult::Cancelled
        } else {
            match outcome {
                Ok(RawFoundationRuntimeOutcome::Ready(ready)) => {
                    RawFoundationJobResult::Ready(ready)
                }
                Ok(RawFoundationRuntimeOutcome::Unavailable { diagnostic }) => {
                    RawFoundationJobResult::Unavailable(diagnostic)
                }
                Ok(RawFoundationRuntimeOutcome::Cancelled) => RawFoundationJobResult::Cancelled,
                Ok(RawFoundationRuntimeOutcome::Failed { diagnostic }) => {
                    RawFoundationJobResult::Failed(diagnostic)
                }
                Err(error) => RawFoundationJobResult::Failed(error.to_string()),
            }
        };
        if let RawFoundationJobResult::Ready(ready) = &result {
            self.remember_ready((**ready).clone())?;
        }
        apply_terminal(job, result);
        Ok(job_snapshot(token, job))
    }

    fn publish_progress(
        &self,
        token: u64,
        progress: RuntimeProgress,
    ) -> Result<(), RawFoundationServiceError> {
        let mut jobs = self.jobs_guard()?;
        let job = jobs
            .get_mut(&token)
            .ok_or(RawFoundationServiceError::UnknownJob(token))?;
        if job.phase.is_terminal() {
            return Ok(());
        }
        if progress.completed_basis_points < job.completed_basis_points {
            return Err(RawFoundationServiceError::ProgressRegression);
        }
        job.phase = RawFoundationJobPhase::Running;
        job.phase_code = progress.phase_code;
        job.completed_basis_points = progress.completed_basis_points;
        Ok(())
    }

    fn jobs_guard(
        &self,
    ) -> Result<std::sync::MutexGuard<'_, BTreeMap<u64, RawFoundationJob>>, RawFoundationServiceError>
    {
        self.jobs
            .lock()
            .map_err(|_| RawFoundationServiceError::StatePoisoned)
    }

    fn ready_foundations_guard(
        &self,
    ) -> Result<std::sync::MutexGuard<'_, VecDeque<RawFoundationReady>>, RawFoundationServiceError>
    {
        self.ready_foundations
            .lock()
            .map_err(|_| RawFoundationServiceError::StatePoisoned)
    }

    pub(crate) fn remember_ready(
        &self,
        ready_foundation: RawFoundationReady,
    ) -> Result<(), RawFoundationServiceError> {
        let mut ready = self.ready_foundations_guard()?;
        ready.retain(|candidate| {
            candidate.source_path != ready_foundation.source_path
                || candidate.source != ready_foundation.source
        });
        ready.push_back(ready_foundation);
        while ready.len() > MAX_READY_RAW_FOUNDATIONS {
            ready.pop_front();
        }
        Ok(())
    }
}

fn apply_terminal(job: &mut RawFoundationJob, result: RawFoundationJobResult) {
    let (phase, phase_code, completed_basis_points) = match &result {
        RawFoundationJobResult::Ready(_) => (
            RawFoundationJobPhase::Ready,
            "ready",
            RUNTIME_PROGRESS_COMPLETE,
        ),
        RawFoundationJobResult::Unavailable(_) => (
            RawFoundationJobPhase::Unavailable,
            "unavailable",
            job.completed_basis_points,
        ),
        RawFoundationJobResult::Cancelled => (
            RawFoundationJobPhase::Cancelled,
            "cancelled",
            job.completed_basis_points,
        ),
        RawFoundationJobResult::Failed(_) => (
            RawFoundationJobPhase::Failed,
            "failed",
            job.completed_basis_points,
        ),
    };
    job.phase = phase;
    job.phase_code = phase_code.into();
    job.completed_basis_points = completed_basis_points;
    job.result = Some(result);
}

fn job_snapshot(token: u64, job: &RawFoundationJob) -> RawFoundationJobSnapshot {
    let (disposition, diagnostic) = match &job.result {
        Some(RawFoundationJobResult::Ready(ready)) => (Some(ready.disposition), String::new()),
        Some(
            RawFoundationJobResult::Unavailable(diagnostic)
            | RawFoundationJobResult::Failed(diagnostic),
        ) => (None, diagnostic.clone()),
        Some(RawFoundationJobResult::Cancelled) | None => (None, String::new()),
    };
    RawFoundationJobSnapshot {
        token,
        request_id: job.request_id.clone(),
        generation: job.generation,
        phase: job.phase,
        phase_code: job.phase_code.clone(),
        completed_basis_points: job.completed_basis_points,
        cancellation_requested: job.cancellation.is_cancelled(),
        disposition,
        diagnostic,
    }
}

struct RawFoundationJobProgress<'service> {
    service: &'service RawFoundationService,
    token: u64,
}

impl RuntimeProgressSink for RawFoundationJobProgress<'_> {
    fn publish(&self, progress: RuntimeProgress) {
        let _ = self.service.publish_progress(self.token, progress);
    }
}

#[derive(Debug, Error)]
pub(crate) enum RawFoundationServiceError {
    #[error("RAW foundation request identity is empty")]
    InvalidRequestIdentity,
    #[error("RAW foundation job registry is full")]
    TooManyJobs,
    #[error("an active RAW foundation job already owns this request generation")]
    DuplicateActiveRequest,
    #[error("RAW foundation job token space is exhausted")]
    TokenExhausted,
    #[error("RAW foundation job state is poisoned")]
    StatePoisoned,
    #[error("unknown RAW foundation job {0}")]
    UnknownJob(u64),
    #[error("RAW foundation job {0} was already executed")]
    JobAlreadyExecuted(u64),
    #[error("RAW foundation invocation does not match its registered request generation")]
    InvocationIdentityMismatch,
    #[error("RAW foundation progress regressed")]
    ProgressRegression,
    #[error("RAW foundation job {0} is still active")]
    JobStillActive(u64),
    #[error("resolved RAW foundation does not belong to the requested Catalog source")]
    ResolvedSourceMismatch,
    #[error(transparent)]
    Runtime(#[from] RawFoundationRuntimeError),
}

#[cfg(test)]
mod tests;
