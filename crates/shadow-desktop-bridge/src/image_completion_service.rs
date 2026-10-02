//! Session-local job and proposal authority for AI image completion.

use std::{
    collections::BTreeMap,
    io::Read,
    path::PathBuf,
    sync::{
        Mutex,
        atomic::{AtomicU64, Ordering},
    },
};

use shadow_ai::{AiGeneratedPayload, CancellationToken, ProviderUnavailable, RuntimeFailure};
use shadow_core::{
    CurrentDerivedRasterPromotionFailure, DerivedRasterStageOutcome, DerivedRasterStageReceipt,
    DerivedRasterStoreError, FilesystemDerivedRasterStore, StagedDerivedRasterProposal,
    managed_image_completion_region, promote_staged_derived_raster_if_current,
};
use shadow_domain::{ImageCompletionRegion, RecipeValidationError, UnitInterval};
use thiserror::Error;

const MAX_ACTIVE_JOBS: usize = 8;
const MAX_STAGED_PROPOSALS: usize = 16;
const MAX_PREVIEW_BYTES: usize = 64 * 1_024 * 1_024;

#[derive(Debug)]
pub(crate) struct ImageCompletionService {
    store: FilesystemDerivedRasterStore,
    next_job_token: AtomicU64,
    jobs: Mutex<BTreeMap<u64, ImageCompletionJob>>,
    next_proposal_token: AtomicU64,
    proposals: Mutex<BTreeMap<u64, ImageCompletionProposal>>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct ImageCompletionPlacement {
    pub(crate) bounds_left: UnitInterval,
    pub(crate) bounds_top: UnitInterval,
    pub(crate) bounds_right: UnitInterval,
    pub(crate) bounds_bottom: UnitInterval,
}

#[derive(Debug)]
pub(crate) enum ImageCompletionCompletion {
    Staged {
        generation: u64,
        proposal_token: u64,
    },
    Unavailable {
        reason: ProviderUnavailable,
    },
    Cancelled,
    Failed {
        failure: RuntimeFailure,
    },
}

#[derive(Debug, Clone)]
pub(crate) struct ImageCompletionProposalPreview {
    pub(crate) generation: u64,
    pub(crate) raster_width: u32,
    pub(crate) raster_height: u32,
    pub(crate) linear_rgba_f32: bool,
    pub(crate) source_color_basis: Option<shadow_domain::ImageCompletionColorBasis>,
    pub(crate) rgba8: Vec<u8>,
    pub(crate) placement: ImageCompletionPlacement,
}

impl ImageCompletionService {
    pub(crate) fn open(
        store_root: impl Into<PathBuf>,
    ) -> Result<Self, ImageCompletionServiceError> {
        Ok(Self {
            store: FilesystemDerivedRasterStore::open(store_root.into())?,
            next_job_token: AtomicU64::new(0),
            jobs: Mutex::new(BTreeMap::new()),
            next_proposal_token: AtomicU64::new(0),
            proposals: Mutex::new(BTreeMap::new()),
        })
    }

    pub(crate) fn store(&self) -> &FilesystemDerivedRasterStore {
        &self.store
    }

    pub(crate) fn begin_job(&self) -> Result<u64, ImageCompletionServiceError> {
        let mut jobs = self
            .jobs
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?;
        if jobs.len() >= MAX_ACTIVE_JOBS {
            return Err(ImageCompletionServiceError::TooManyActiveJobs);
        }
        let token = next_token(&self.next_job_token)?;
        jobs.insert(
            token,
            ImageCompletionJob {
                cancellation: CancellationToken::default(),
                preview_render_token: None,
            },
        );
        Ok(token)
    }

    pub(crate) fn cancellation(
        &self,
        job_token: u64,
    ) -> Result<CancellationToken, ImageCompletionServiceError> {
        self.jobs
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?
            .get(&job_token)
            .map(|job| job.cancellation.clone())
            .ok_or(ImageCompletionServiceError::UnknownJob(job_token))
    }

    pub(crate) fn attach_preview_render(
        &self,
        job_token: u64,
        preview_render_token: u64,
    ) -> Result<(), ImageCompletionServiceError> {
        if preview_render_token == 0 {
            return Err(ImageCompletionServiceError::InvalidPreviewToken);
        }
        let mut jobs = self
            .jobs
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?;
        let job = jobs
            .get_mut(&job_token)
            .ok_or(ImageCompletionServiceError::UnknownJob(job_token))?;
        if job.cancellation.is_cancelled() {
            return Err(ImageCompletionServiceError::JobCancelled(job_token));
        }
        if job.preview_render_token.is_some() {
            return Err(ImageCompletionServiceError::PreviewAlreadyAttached(
                job_token,
            ));
        }
        job.preview_render_token = Some(preview_render_token);
        Ok(())
    }

    pub(crate) fn replace_preview_render(
        &self,
        job_token: u64,
        preview_render_token: u64,
    ) -> Result<(), ImageCompletionServiceError> {
        if preview_render_token == 0 {
            return Err(ImageCompletionServiceError::InvalidPreviewToken);
        }
        let mut jobs = self
            .jobs
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?;
        let job = jobs
            .get_mut(&job_token)
            .ok_or(ImageCompletionServiceError::UnknownJob(job_token))?;
        if job.cancellation.is_cancelled() {
            return Err(ImageCompletionServiceError::JobCancelled(job_token));
        }
        job.preview_render_token = Some(preview_render_token);
        Ok(())
    }

    pub(crate) fn cancel_job(
        &self,
        job_token: u64,
    ) -> Result<Option<u64>, ImageCompletionServiceError> {
        let jobs = self
            .jobs
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?;
        let job = jobs
            .get(&job_token)
            .ok_or(ImageCompletionServiceError::UnknownJob(job_token))?;
        job.cancellation.cancel();
        Ok(job.preview_render_token)
    }

    pub(crate) fn finish_job(&self, job_token: u64) -> Result<(), ImageCompletionServiceError> {
        self.jobs
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?
            .remove(&job_token)
            .map(|_| ())
            .ok_or(ImageCompletionServiceError::UnknownJob(job_token))
    }

    pub(crate) fn complete_job(
        &self,
        job_token: u64,
        receipt: DerivedRasterStageReceipt,
        placement: ImageCompletionPlacement,
    ) -> Result<ImageCompletionCompletion, ImageCompletionServiceError> {
        let jobs = self
            .jobs
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?;
        let job = jobs
            .get(&job_token)
            .ok_or(ImageCompletionServiceError::UnknownJob(job_token))?;
        let cancelled = job.cancellation.is_cancelled();
        drop(jobs);
        Ok(match receipt.outcome {
            DerivedRasterStageOutcome::Staged(staged) if cancelled => {
                drop(staged);
                ImageCompletionCompletion::Cancelled
            }
            DerivedRasterStageOutcome::Staged(staged) => ImageCompletionCompletion::Staged {
                generation: receipt.generation,
                proposal_token: self.register_proposal(staged, placement)?,
            },
            DerivedRasterStageOutcome::Unavailable { reason } => {
                ImageCompletionCompletion::Unavailable { reason }
            }
            DerivedRasterStageOutcome::Cancelled => ImageCompletionCompletion::Cancelled,
            DerivedRasterStageOutcome::Failed { failure } => {
                ImageCompletionCompletion::Failed { failure }
            }
        })
    }

    fn register_proposal(
        &self,
        staged: StagedDerivedRasterProposal,
        placement: ImageCompletionPlacement,
    ) -> Result<u64, ImageCompletionServiceError> {
        let mut proposals = self
            .proposals
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?;
        if proposals.len() >= MAX_STAGED_PROPOSALS {
            return Err(ImageCompletionServiceError::TooManyStagedProposals);
        }
        let token = next_token(&self.next_proposal_token)?;
        proposals.insert(token, ImageCompletionProposal { staged, placement });
        Ok(token)
    }

    pub(crate) fn proposal_preview(
        &self,
        proposal_token: u64,
    ) -> Result<ImageCompletionProposalPreview, ImageCompletionServiceError> {
        let (generation, patch, placement) = {
            let proposals = self
                .proposals
                .lock()
                .map_err(|_| ImageCompletionServiceError::StatePoisoned)?;
            let proposal = proposals
                .get(&proposal_token)
                .ok_or(ImageCompletionServiceError::UnknownProposal(proposal_token))?;
            let AiGeneratedPayload::ImageCompletionPatch(patch) = proposal.staged.payload() else {
                return Err(ImageCompletionServiceError::ExpectedCompletionPatch);
            };
            (
                proposal.staged.generation(),
                patch.clone(),
                proposal.placement,
            )
        };
        let expected = u64::from(patch.raster_extent.width)
            .checked_mul(u64::from(patch.raster_extent.height))
            .and_then(|pixels| pixels.checked_mul(if patch.linear_rgba_f32() { 16 } else { 4 }))
            .ok_or(ImageCompletionServiceError::ProposalPreviewTooLarge)?;
        if expected != patch.artifact.byte_len() || expected > MAX_PREVIEW_BYTES as u64 {
            return Err(ImageCompletionServiceError::ProposalPreviewTooLarge);
        }
        let capacity = usize::try_from(expected)
            .map_err(|_| ImageCompletionServiceError::ProposalPreviewTooLarge)?;
        let mut rgba8 = Vec::with_capacity(capacity);
        self.store
            .open_staged_proposal(&patch.artifact)?
            .take(expected.saturating_add(1))
            .read_to_end(&mut rgba8)
            .map_err(ImageCompletionServiceError::ReadProposalPreview)?;
        if u64::try_from(rgba8.len()).ok() != Some(expected) {
            return Err(ImageCompletionServiceError::InvalidProposalByteLength);
        }
        Ok(ImageCompletionProposalPreview {
            generation,
            raster_width: patch.raster_extent.width,
            raster_height: patch.raster_extent.height,
            linear_rgba_f32: patch.linear_rgba_f32(),
            source_color_basis: patch
                .source_context
                .as_ref()
                .and_then(|s| s.color_basis.clone()),
            rgba8,
            placement,
        })
    }

    pub(crate) fn discard_proposal(
        &self,
        proposal_token: u64,
    ) -> Result<(), ImageCompletionServiceError> {
        self.proposals
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?
            .remove(&proposal_token)
            .map(|_| ())
            .ok_or(ImageCompletionServiceError::UnknownProposal(proposal_token))
    }

    /// Validate the lease-bound Photo before consuming proposal authority.
    /// Tokens are never reused and proposals are immutable; concurrent removal
    /// after this check can only make promotion fail with UnknownProposal.
    pub(crate) fn validate_proposal_photo(
        &self,
        proposal_token: u64,
        photo_id: shadow_domain::PhotoId,
    ) -> Result<(), ImageCompletionServiceError> {
        let proposals = self
            .proposals
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?;
        let proposal = proposals
            .get(&proposal_token)
            .ok_or(ImageCompletionServiceError::UnknownProposal(proposal_token))?;
        if proposal.staged.target() != &(shadow_ai::ObservationTarget::Photo { photo_id }) {
            return Err(ImageCompletionServiceError::ProposalPhotoMismatch);
        }
        Ok(())
    }

    pub(crate) fn promote_proposal(
        &self,
        proposal_token: u64,
        current_generation: u64,
    ) -> Result<ImageCompletionRegion, ImageCompletionServiceError> {
        let proposal = self
            .proposals
            .lock()
            .map_err(|_| ImageCompletionServiceError::StatePoisoned)?
            .remove(&proposal_token)
            .ok_or(ImageCompletionServiceError::UnknownProposal(proposal_token))?;
        let mut store = self.store.clone();
        let managed = promote_staged_derived_raster_if_current(
            &mut store,
            current_generation,
            proposal.staged,
        )?;
        Ok(managed_image_completion_region(
            &managed,
            proposal.placement.bounds_left,
            proposal.placement.bounds_top,
            proposal.placement.bounds_right,
            proposal.placement.bounds_bottom,
        )?)
    }
}

#[derive(Debug)]
struct ImageCompletionJob {
    cancellation: CancellationToken,
    preview_render_token: Option<u64>,
}

#[derive(Debug)]
struct ImageCompletionProposal {
    staged: StagedDerivedRasterProposal,
    placement: ImageCompletionPlacement,
}

fn next_token(sequence: &AtomicU64) -> Result<u64, ImageCompletionServiceError> {
    sequence
        .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |current| {
            current.checked_add(1)
        })
        .map(|previous| previous + 1)
        .map_err(|_| ImageCompletionServiceError::TokenExhausted)
}

#[derive(Debug, Error)]
pub(crate) enum ImageCompletionServiceError {
    #[error("AI proposal belongs to a different photo")]
    ProposalPhotoMismatch,
    #[error("image-completion service state is poisoned")]
    StatePoisoned,
    #[error("too many image-completion jobs are active")]
    TooManyActiveJobs,
    #[error("too many image-completion proposals await a decision")]
    TooManyStagedProposals,
    #[error("image-completion token space is exhausted")]
    TokenExhausted,
    #[error("image-completion job {0} is unknown")]
    UnknownJob(u64),
    #[error("image-completion proposal {0} is unknown or already consumed")]
    UnknownProposal(u64),
    #[error("image-completion preview render token must be non-zero")]
    InvalidPreviewToken,
    #[error("image-completion job {0} already has an input render")]
    PreviewAlreadyAttached(u64),
    #[error("image-completion job {0} was cancelled before input render attachment")]
    JobCancelled(u64),
    #[error("image-completion proposal did not contain an RGBA8 patch")]
    ExpectedCompletionPatch,
    #[error("image-completion preview exceeds its bounded memory contract")]
    ProposalPreviewTooLarge,
    #[error("image-completion proposal byte length is invalid")]
    InvalidProposalByteLength,
    #[error("failed to read verified image-completion proposal bytes")]
    ReadProposalPreview(#[source] std::io::Error),
    #[error(transparent)]
    Store(#[from] DerivedRasterStoreError),
    #[error(transparent)]
    Promotion(#[from] CurrentDerivedRasterPromotionFailure),
    #[error(transparent)]
    Recipe(#[from] RecipeValidationError),
}

#[cfg(test)]
mod tests;
