//! Session-local lifecycle for bounded anonymous-person analysis.
//!
//! The service owns only job state: opaque identity, cooperative cancellation,
//! monotonic progress, and terminal retirement. Face evidence and grouping stay
//! in `shadow-core`; Qt receives only the existing vector-free report.

use std::{
    collections::BTreeMap,
    fmt,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
};

use shadow_core::{
    PeopleAnalysisControl, PeopleAnalysisPhase, PeopleAnalysisProgress, PeopleAnalysisReport,
};
use thiserror::Error;

const MAX_PEOPLE_ANALYSIS_JOBS: usize = 4;

#[derive(Debug)]
pub(crate) struct PeopleAnalysisService {
    next_job_token: AtomicU64,
    jobs: Mutex<BTreeMap<u64, PeopleAnalysisJob>>,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) enum PeopleAnalysisJobPhase {
    Queued,
    Reviewing,
    Grouping,
    Ready,
    Cancelled,
    Failed,
}

impl PeopleAnalysisJobPhase {
    pub(crate) const fn is_terminal(self) -> bool {
        matches!(self, Self::Ready | Self::Cancelled | Self::Failed)
    }

    pub(crate) const fn code(self) -> &'static str {
        match self {
            Self::Queued => "queued",
            Self::Reviewing => "reviewing",
            Self::Grouping => "grouping",
            Self::Ready => "ready",
            Self::Cancelled => "cancelled",
            Self::Failed => "failed",
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct PeopleAnalysisJobSnapshot {
    pub(crate) token: u64,
    pub(crate) phase: PeopleAnalysisJobPhase,
    pub(crate) analyzed_photos: usize,
    pub(crate) maximum_photos: usize,
    pub(crate) detected_faces: usize,
    pub(crate) compared_faces: usize,
    pub(crate) cancellation_requested: bool,
}

#[derive(Debug)]
pub(crate) enum PeopleAnalysisJobOutcome {
    Ready(PeopleAnalysisReport),
    Cancelled,
    Failed(String),
}

#[derive(Debug)]
struct PeopleAnalysisJob {
    phase: PeopleAnalysisJobPhase,
    analyzed_photos: usize,
    maximum_photos: usize,
    detected_faces: usize,
    compared_faces: usize,
    cancellation: Arc<AtomicBool>,
}

impl PeopleAnalysisService {
    pub(crate) fn new() -> Self {
        Self {
            next_job_token: AtomicU64::new(0),
            jobs: Mutex::new(BTreeMap::new()),
        }
    }

    pub(crate) fn begin_job(
        &self,
        maximum_photos: usize,
    ) -> Result<u64, PeopleAnalysisServiceError> {
        if maximum_photos == 0 {
            return Err(PeopleAnalysisServiceError::InvalidMaximumPhotos);
        }
        let mut jobs = self.jobs_guard()?;
        if jobs.values().any(|job| !job.phase.is_terminal()) {
            return Err(PeopleAnalysisServiceError::JobAlreadyActive);
        }
        if jobs.len() >= MAX_PEOPLE_ANALYSIS_JOBS {
            return Err(PeopleAnalysisServiceError::TooManyJobs);
        }
        let token = self
            .next_job_token
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |current| {
                current.checked_add(1)
            })
            .map_err(|_| PeopleAnalysisServiceError::TokenExhausted)?
            + 1;
        jobs.insert(
            token,
            PeopleAnalysisJob {
                phase: PeopleAnalysisJobPhase::Queued,
                analyzed_photos: 0,
                maximum_photos,
                detected_faces: 0,
                compared_faces: 0,
                cancellation: Arc::new(AtomicBool::new(false)),
            },
        );
        Ok(token)
    }

    pub(crate) fn execute_job<E>(
        &self,
        token: u64,
        execute: impl FnOnce(&dyn PeopleAnalysisControl) -> Result<PeopleAnalysisReport, E>,
    ) -> Result<PeopleAnalysisJobOutcome, PeopleAnalysisServiceError>
    where
        E: fmt::Display,
    {
        let cancellation = {
            let mut jobs = self.jobs_guard()?;
            let job = jobs
                .get_mut(&token)
                .ok_or(PeopleAnalysisServiceError::UnknownJob(token))?;
            if job.phase != PeopleAnalysisJobPhase::Queued {
                return Err(PeopleAnalysisServiceError::JobAlreadyExecuted(token));
            }
            job.phase = PeopleAnalysisJobPhase::Reviewing;
            Arc::clone(&job.cancellation)
        };
        let control = PeopleAnalysisJobControl {
            service: self,
            token,
            cancellation: Arc::clone(&cancellation),
        };
        let outcome = if cancellation.load(Ordering::Acquire) {
            PeopleAnalysisJobOutcome::Cancelled
        } else {
            match execute(&control) {
                Ok(report) => PeopleAnalysisJobOutcome::Ready(report),
                Err(_error) if cancellation.load(Ordering::Acquire) => {
                    PeopleAnalysisJobOutcome::Cancelled
                }
                Err(error) => PeopleAnalysisJobOutcome::Failed(error.to_string()),
            }
        };
        let mut jobs = self.jobs_guard()?;
        let job = jobs
            .get_mut(&token)
            .ok_or(PeopleAnalysisServiceError::UnknownJob(token))?;
        if job.cancellation.load(Ordering::Acquire) {
            job.phase = PeopleAnalysisJobPhase::Cancelled;
            return Ok(PeopleAnalysisJobOutcome::Cancelled);
        }
        job.phase = match &outcome {
            PeopleAnalysisJobOutcome::Ready(_) => PeopleAnalysisJobPhase::Ready,
            PeopleAnalysisJobOutcome::Cancelled => PeopleAnalysisJobPhase::Cancelled,
            PeopleAnalysisJobOutcome::Failed(_) => PeopleAnalysisJobPhase::Failed,
        };
        Ok(outcome)
    }

    pub(crate) fn cancel_job(&self, token: u64) -> Result<bool, PeopleAnalysisServiceError> {
        let jobs = self.jobs_guard()?;
        let job = jobs
            .get(&token)
            .ok_or(PeopleAnalysisServiceError::UnknownJob(token))?;
        if !job.phase.is_terminal() {
            job.cancellation.store(true, Ordering::Release);
            return Ok(true);
        }
        Ok(false)
    }

    pub(crate) fn snapshot(
        &self,
        token: u64,
    ) -> Result<PeopleAnalysisJobSnapshot, PeopleAnalysisServiceError> {
        let jobs = self.jobs_guard()?;
        let job = jobs
            .get(&token)
            .ok_or(PeopleAnalysisServiceError::UnknownJob(token))?;
        Ok(PeopleAnalysisJobSnapshot {
            token,
            phase: job.phase,
            analyzed_photos: job.analyzed_photos,
            maximum_photos: job.maximum_photos,
            detected_faces: job.detected_faces,
            compared_faces: job.compared_faces,
            cancellation_requested: job.cancellation.load(Ordering::Acquire),
        })
    }

    pub(crate) fn retire_job(&self, token: u64) -> Result<(), PeopleAnalysisServiceError> {
        let mut jobs = self.jobs_guard()?;
        let job = jobs
            .get(&token)
            .ok_or(PeopleAnalysisServiceError::UnknownJob(token))?;
        if !job.phase.is_terminal() {
            return Err(PeopleAnalysisServiceError::JobStillActive(token));
        }
        jobs.remove(&token);
        Ok(())
    }

    fn publish_progress(
        &self,
        token: u64,
        progress: PeopleAnalysisProgress,
    ) -> Result<(), PeopleAnalysisServiceError> {
        let mut jobs = self.jobs_guard()?;
        let job = jobs
            .get_mut(&token)
            .ok_or(PeopleAnalysisServiceError::UnknownJob(token))?;
        if job.phase.is_terminal() {
            return Ok(());
        }
        if progress.maximum_photos != job.maximum_photos
            || progress.analyzed_photos < job.analyzed_photos
            || progress.detected_faces < job.detected_faces
            || progress.compared_faces < job.compared_faces
        {
            return Err(PeopleAnalysisServiceError::ProgressRegression);
        }
        job.phase = match progress.phase {
            PeopleAnalysisPhase::Reviewing => PeopleAnalysisJobPhase::Reviewing,
            PeopleAnalysisPhase::Grouping => PeopleAnalysisJobPhase::Grouping,
        };
        job.analyzed_photos = progress.analyzed_photos;
        job.detected_faces = progress.detected_faces;
        job.compared_faces = progress.compared_faces;
        Ok(())
    }

    fn jobs_guard(
        &self,
    ) -> Result<
        std::sync::MutexGuard<'_, BTreeMap<u64, PeopleAnalysisJob>>,
        PeopleAnalysisServiceError,
    > {
        self.jobs
            .lock()
            .map_err(|_| PeopleAnalysisServiceError::StatePoisoned)
    }
}

struct PeopleAnalysisJobControl<'a> {
    service: &'a PeopleAnalysisService,
    token: u64,
    cancellation: Arc<AtomicBool>,
}

impl PeopleAnalysisControl for PeopleAnalysisJobControl<'_> {
    fn cancellation_requested(&self) -> bool {
        self.cancellation.load(Ordering::Acquire)
    }

    fn publish(&self, progress: PeopleAnalysisProgress) {
        let _ = self.service.publish_progress(self.token, progress);
    }
}

#[derive(Debug, Error)]
pub(crate) enum PeopleAnalysisServiceError {
    #[error("people analysis requires a positive photo bound")]
    InvalidMaximumPhotos,
    #[error("another people-analysis job is still active")]
    JobAlreadyActive,
    #[error("too many people-analysis jobs are awaiting retirement")]
    TooManyJobs,
    #[error("people-analysis job token space is exhausted")]
    TokenExhausted,
    #[error("unknown people-analysis job {0}")]
    UnknownJob(u64),
    #[error("people-analysis job {0} already executed")]
    JobAlreadyExecuted(u64),
    #[error("people-analysis job {0} is still active")]
    JobStillActive(u64),
    #[error("people-analysis progress regressed or changed its declared bound")]
    ProgressRegression,
    #[error("people-analysis job state is poisoned")]
    StatePoisoned,
}

#[cfg(test)]
mod tests;
