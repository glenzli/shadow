//! Desktop-session ownership for AI-mask jobs and staged proposals.
//!
//! Native preview cancellation, provider cancellation, and move-only proposal
//! authority stay behind this service. The CXX facade receives opaque numeric
//! tokens and can never manufacture a durable managed-raster reference.

use std::{
    collections::BTreeMap,
    io::Read,
    path::PathBuf,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, Ordering},
    },
};

use shadow_ai::{
    AiGeneratedPayload, CancellationToken, ProviderUnavailable, RasterExtent, RuntimeFailure,
    SoftMaskEncoding,
};
use shadow_core::{
    CurrentDerivedRasterPromotionFailure, DerivedRasterStageOutcome, DerivedRasterStageReceipt,
    DerivedRasterStoreError, FilesystemDerivedRasterStore, StagedDerivedRasterProposal,
    managed_soft_mask_definition, promote_staged_derived_raster_if_current,
};
use shadow_domain::MaskDefinition;
use thiserror::Error;

use crate::{
    recipe_v1::GradeStackDraft,
    subject_mask_people::{
        FaceRegionSet, MAX_SUBJECT_MASK_PEOPLE, ParsedSubjectMaskPerson, SubjectMaskPersonCandidate,
    },
};

const MAX_ACTIVE_SUBJECT_MASK_JOBS: usize = 16;
const MAX_ACTIVE_SUBJECT_MASK_INPUT_SESSIONS: usize = 16;
const MAX_STAGED_SUBJECT_MASK_PROPOSALS: usize = 32;
const MAX_SUBJECT_MASK_INPUT_BYTES: usize = 64 * 1024 * 1024;
const MAX_SUBJECT_MASK_SESSION_RESIDENT_BYTES: usize = 64 * 1024 * 1024;
const MAX_SUBJECT_MASK_PREVIEW_BYTES: usize = 64 * 1024 * 1024;

#[derive(Debug)]
pub(crate) struct SubjectMaskService {
    store: FilesystemDerivedRasterStore,
    next_job_token: AtomicU64,
    jobs: Mutex<BTreeMap<u64, SubjectMaskJob>>,
    next_input_session_token: AtomicU64,
    input_sessions: Mutex<BTreeMap<u64, SubjectMaskInputSession>>,
    next_proposal_token: AtomicU64,
    proposals: Mutex<BTreeMap<u64, StagedDerivedRasterProposal>>,
}

#[derive(Debug)]
pub(crate) enum SubjectMaskCompletion {
    Staged {
        request_id: String,
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

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct SubjectMaskProposalPreview {
    pub(crate) generation: u64,
    pub(crate) raster_extent: RasterExtent,
    pub(crate) coordinate_extent: RasterExtent,
    pub(crate) encoding: SoftMaskEncoding,
    pub(crate) samples: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct SubjectMaskInputIdentity {
    pub(crate) photo_id: String,
    pub(crate) source_path: String,
    pub(crate) base_commit_id: String,
    pub(crate) grade_stack: GradeStackDraft,
    pub(crate) target_grade_node_index: u32,
    pub(crate) target_grade_node_id: String,
}

#[derive(Debug, Clone)]
pub(crate) struct PreparedSubjectMaskInput {
    pub(crate) bytes: Arc<[u8]>,
    pub(crate) content_hash: String,
    pub(crate) coordinate_extent: RasterExtent,
}

#[derive(Debug)]
pub(crate) enum SubjectMaskInputAdmission {
    Prepare,
    Reuse(PreparedSubjectMaskInput),
}

#[derive(Debug, Clone)]
pub(crate) struct SubjectMaskPersonSnapshot {
    pub(crate) candidate: SubjectMaskPersonCandidate,
    pub(crate) available_regions: Option<FaceRegionSet>,
}

impl SubjectMaskService {
    pub(crate) fn open(store_root: impl Into<PathBuf>) -> Result<Self, SubjectMaskServiceError> {
        Ok(Self {
            store: FilesystemDerivedRasterStore::open(store_root.into())?,
            next_job_token: AtomicU64::new(0),
            jobs: Mutex::new(BTreeMap::new()),
            next_input_session_token: AtomicU64::new(0),
            input_sessions: Mutex::new(BTreeMap::new()),
            next_proposal_token: AtomicU64::new(0),
            proposals: Mutex::new(BTreeMap::new()),
        })
    }

    pub(crate) fn store(&self) -> &FilesystemDerivedRasterStore {
        &self.store
    }

    pub(crate) fn begin_job(&self) -> Result<u64, SubjectMaskServiceError> {
        let mut jobs = self
            .jobs
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        if jobs.len() >= MAX_ACTIVE_SUBJECT_MASK_JOBS {
            return Err(SubjectMaskServiceError::TooManyActiveJobs);
        }
        let token = next_token(&self.next_job_token)?;
        jobs.insert(
            token,
            SubjectMaskJob {
                cancellation: CancellationToken::default(),
                preview_render_token: None,
            },
        );
        Ok(token)
    }

    pub(crate) fn begin_input_session(&self) -> Result<u64, SubjectMaskServiceError> {
        let mut sessions = self
            .input_sessions
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        if sessions.len() >= MAX_ACTIVE_SUBJECT_MASK_INPUT_SESSIONS {
            return Err(SubjectMaskServiceError::TooManyActiveInputSessions);
        }
        let token = next_token(&self.next_input_session_token)?;
        sessions.insert(token, SubjectMaskInputSession::Vacant);
        Ok(token)
    }

    pub(crate) fn finish_input_session(
        &self,
        input_session_token: u64,
    ) -> Result<(), SubjectMaskServiceError> {
        self.input_sessions
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?
            .remove(&input_session_token)
            .map(|_| ())
            .ok_or(SubjectMaskServiceError::UnknownInputSession(
                input_session_token,
            ))
    }

    /// Reserves preparation exactly once, or returns the immutable input
    /// already prepared for the same photo, Recipe and target identity.
    pub(crate) fn admit_input(
        &self,
        input_session_token: u64,
        identity: &SubjectMaskInputIdentity,
    ) -> Result<SubjectMaskInputAdmission, SubjectMaskServiceError> {
        let mut sessions = self
            .input_sessions
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        let session = sessions.get_mut(&input_session_token).ok_or(
            SubjectMaskServiceError::UnknownInputSession(input_session_token),
        )?;
        match session {
            SubjectMaskInputSession::Vacant => {
                *session = SubjectMaskInputSession::Preparing(identity.clone());
                Ok(SubjectMaskInputAdmission::Prepare)
            }
            SubjectMaskInputSession::Preparing(current) => {
                if current == identity {
                    Err(SubjectMaskServiceError::InputPreparationInFlight(
                        input_session_token,
                    ))
                } else {
                    Err(SubjectMaskServiceError::InputIdentityChanged(
                        input_session_token,
                    ))
                }
            }
            SubjectMaskInputSession::Ready {
                identity: current,
                input,
                ..
            } => {
                if current != identity {
                    return Err(SubjectMaskServiceError::InputIdentityChanged(
                        input_session_token,
                    ));
                }
                Ok(SubjectMaskInputAdmission::Reuse(input.clone()))
            }
        }
    }

    pub(crate) fn complete_input_preparation(
        &self,
        input_session_token: u64,
        identity: &SubjectMaskInputIdentity,
        bytes: Vec<u8>,
        coordinate_extent: RasterExtent,
    ) -> Result<PreparedSubjectMaskInput, SubjectMaskServiceError> {
        if bytes.is_empty() || bytes.len() > MAX_SUBJECT_MASK_INPUT_BYTES {
            return Err(SubjectMaskServiceError::InvalidInputSize(bytes.len()));
        }
        let mut sessions = self
            .input_sessions
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        let resident_bytes = resident_input_session_bytes(&sessions)?;
        if resident_bytes
            .checked_add(bytes.len())
            .is_none_or(|total| total > MAX_SUBJECT_MASK_SESSION_RESIDENT_BYTES)
        {
            return Err(SubjectMaskServiceError::InputBudgetExceeded);
        }
        let session = sessions.get_mut(&input_session_token).ok_or(
            SubjectMaskServiceError::UnknownInputSession(input_session_token),
        )?;
        match session {
            SubjectMaskInputSession::Preparing(current) if current == identity => {}
            SubjectMaskInputSession::Preparing(_) | SubjectMaskInputSession::Ready { .. } => {
                return Err(SubjectMaskServiceError::InputIdentityChanged(
                    input_session_token,
                ));
            }
            SubjectMaskInputSession::Vacant => {
                return Err(SubjectMaskServiceError::InputPreparationNotReserved(
                    input_session_token,
                ));
            }
        }
        let bytes: Arc<[u8]> = bytes.into();
        let input = PreparedSubjectMaskInput {
            content_hash: blake3::hash(&bytes).to_hex().to_string(),
            bytes,
            coordinate_extent,
        };
        *session = SubjectMaskInputSession::Ready {
            identity: identity.clone(),
            input: input.clone(),
            people: None,
            parsed_people: BTreeMap::new(),
        };
        Ok(input)
    }

    pub(crate) fn people_snapshot(
        &self,
        input_session_token: u64,
        identity: &SubjectMaskInputIdentity,
    ) -> Result<Option<Vec<SubjectMaskPersonSnapshot>>, SubjectMaskServiceError> {
        let sessions = self
            .input_sessions
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        let session = ready_input_session(&sessions, input_session_token, identity)?;
        let Some(people) = session.people.as_ref() else {
            return Ok(None);
        };
        Ok(Some(
            people
                .iter()
                .enumerate()
                .map(|(index, candidate)| SubjectMaskPersonSnapshot {
                    candidate: candidate.clone(),
                    available_regions: session
                        .parsed_people
                        .get(&(index as u32))
                        .map(|parsed| parsed.available_regions),
                })
                .collect(),
        ))
    }

    pub(crate) fn cache_people(
        &self,
        input_session_token: u64,
        identity: &SubjectMaskInputIdentity,
        people: Vec<SubjectMaskPersonCandidate>,
    ) -> Result<Vec<SubjectMaskPersonSnapshot>, SubjectMaskServiceError> {
        if people.len() > MAX_SUBJECT_MASK_PEOPLE {
            return Err(SubjectMaskServiceError::TooManyDetectedPeople(people.len()));
        }
        let mut sessions = self
            .input_sessions
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        {
            let session = ready_input_session(&sessions, input_session_token, identity)?;
            if let Some(cached) = &session.people {
                return Ok(cached
                    .iter()
                    .enumerate()
                    .map(|(index, candidate)| SubjectMaskPersonSnapshot {
                        candidate: candidate.clone(),
                        available_regions: session
                            .parsed_people
                            .get(&(index as u32))
                            .map(|parsed| parsed.available_regions),
                    })
                    .collect());
            }
        }
        let added_bytes = people
            .iter()
            .map(SubjectMaskPersonCandidate::resident_byte_len)
            .try_fold(0_usize, usize::checked_add)
            .ok_or(SubjectMaskServiceError::InputBudgetExceeded)?;
        if resident_input_session_bytes(&sessions)?
            .checked_add(added_bytes)
            .is_none_or(|total| total > MAX_SUBJECT_MASK_SESSION_RESIDENT_BYTES)
        {
            return Err(SubjectMaskServiceError::InputBudgetExceeded);
        }
        let session = ready_input_session_mut(&mut sessions, input_session_token, identity)?;
        if session.people.is_none() {
            *session.people = Some(people.into());
        }
        Ok(session
            .people
            .as_ref()
            .expect("people cache was just initialized")
            .iter()
            .enumerate()
            .map(|(index, candidate)| SubjectMaskPersonSnapshot {
                candidate: candidate.clone(),
                available_regions: session
                    .parsed_people
                    .get(&(index as u32))
                    .map(|parsed| parsed.available_regions),
            })
            .collect())
    }

    pub(crate) fn person_candidate(
        &self,
        input_session_token: u64,
        identity: &SubjectMaskInputIdentity,
        person_index: u32,
    ) -> Result<SubjectMaskPersonCandidate, SubjectMaskServiceError> {
        let sessions = self
            .input_sessions
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        let session = ready_input_session(&sessions, input_session_token, identity)?;
        session
            .people
            .as_ref()
            .ok_or(SubjectMaskServiceError::PeopleNotDiscovered(
                input_session_token,
            ))?
            .get(person_index as usize)
            .cloned()
            .ok_or(SubjectMaskServiceError::UnknownPerson(person_index))
    }

    pub(crate) fn parsed_person(
        &self,
        input_session_token: u64,
        identity: &SubjectMaskInputIdentity,
        person_index: u32,
    ) -> Result<Option<Arc<ParsedSubjectMaskPerson>>, SubjectMaskServiceError> {
        let sessions = self
            .input_sessions
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        let session = ready_input_session(&sessions, input_session_token, identity)?;
        Ok(session.parsed_people.get(&person_index).cloned())
    }

    pub(crate) fn cache_parsed_person(
        &self,
        input_session_token: u64,
        identity: &SubjectMaskInputIdentity,
        person_index: u32,
        parsed: ParsedSubjectMaskPerson,
    ) -> Result<Arc<ParsedSubjectMaskPerson>, SubjectMaskServiceError> {
        let mut sessions = self
            .input_sessions
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        {
            let session = ready_input_session(&sessions, input_session_token, identity)?;
            if let Some(cached) = session.parsed_people.get(&person_index) {
                return Ok(cached.clone());
            }
        }
        let added_bytes = parsed.resident_byte_len();
        if resident_input_session_bytes(&sessions)?
            .checked_add(added_bytes)
            .is_none_or(|total| total > MAX_SUBJECT_MASK_SESSION_RESIDENT_BYTES)
        {
            return Err(SubjectMaskServiceError::InputBudgetExceeded);
        }
        let session = ready_input_session_mut(&mut sessions, input_session_token, identity)?;
        let people =
            session
                .people
                .as_ref()
                .ok_or(SubjectMaskServiceError::PeopleNotDiscovered(
                    input_session_token,
                ))?;
        if people.get(person_index as usize).is_none() {
            return Err(SubjectMaskServiceError::UnknownPerson(person_index));
        }
        Ok(session
            .parsed_people
            .entry(person_index)
            .or_insert_with(|| Arc::new(parsed))
            .clone())
    }

    pub(crate) fn abort_input_preparation(
        &self,
        input_session_token: u64,
        identity: &SubjectMaskInputIdentity,
    ) -> Result<(), SubjectMaskServiceError> {
        let mut sessions = self
            .input_sessions
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        let session = sessions.get_mut(&input_session_token).ok_or(
            SubjectMaskServiceError::UnknownInputSession(input_session_token),
        )?;
        match session {
            SubjectMaskInputSession::Preparing(current) if current == identity => {
                *session = SubjectMaskInputSession::Vacant;
                Ok(())
            }
            SubjectMaskInputSession::Preparing(_) | SubjectMaskInputSession::Ready { .. } => Err(
                SubjectMaskServiceError::InputIdentityChanged(input_session_token),
            ),
            SubjectMaskInputSession::Vacant => Ok(()),
        }
    }

    pub(crate) fn cancellation(
        &self,
        job_token: u64,
    ) -> Result<CancellationToken, SubjectMaskServiceError> {
        let jobs = self
            .jobs
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        jobs.get(&job_token)
            .map(|job| job.cancellation.clone())
            .ok_or(SubjectMaskServiceError::UnknownJob(job_token))
    }

    pub(crate) fn attach_preview_render(
        &self,
        job_token: u64,
        preview_render_token: u64,
    ) -> Result<(), SubjectMaskServiceError> {
        if preview_render_token == 0 {
            return Err(SubjectMaskServiceError::InvalidPreviewToken);
        }
        let mut jobs = self
            .jobs
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        let job = jobs
            .get_mut(&job_token)
            .ok_or(SubjectMaskServiceError::UnknownJob(job_token))?;
        if job.cancellation.is_cancelled() {
            return Err(SubjectMaskServiceError::JobCancelled(job_token));
        }
        if job.preview_render_token.is_some() {
            return Err(SubjectMaskServiceError::PreviewAlreadyAttached(job_token));
        }
        job.preview_render_token = Some(preview_render_token);
        Ok(())
    }

    /// Cancels provider work and returns the native preview token, if one is
    /// already attached, so the session facade can signal its existing render
    /// registry without coupling this service to preview internals.
    pub(crate) fn cancel_job(
        &self,
        job_token: u64,
    ) -> Result<Option<u64>, SubjectMaskServiceError> {
        let jobs = self
            .jobs
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        let job = jobs
            .get(&job_token)
            .ok_or(SubjectMaskServiceError::UnknownJob(job_token))?;
        job.cancellation.cancel();
        Ok(job.preview_render_token)
    }

    pub(crate) fn finish_job(&self, job_token: u64) -> Result<(), SubjectMaskServiceError> {
        self.take_job(job_token)?
            .map(|_| ())
            .ok_or(SubjectMaskServiceError::UnknownJob(job_token))
    }

    /// Retires one job and turns its runtime receipt into a desktop proposal
    /// token or an explicit terminal state.
    ///
    /// Cancellation wins if it races provider success before publication: the
    /// move-only staged authority is dropped and never reaches the proposal
    /// registry.
    pub(crate) fn complete_job(
        &self,
        job_token: u64,
        receipt: DerivedRasterStageReceipt,
    ) -> Result<SubjectMaskCompletion, SubjectMaskServiceError> {
        let job = self
            .take_job(job_token)?
            .ok_or(SubjectMaskServiceError::UnknownJob(job_token))?;
        let cancelled = job.cancellation.is_cancelled();
        let completion = match receipt.outcome {
            DerivedRasterStageOutcome::Staged(staged) if cancelled => {
                drop(staged);
                SubjectMaskCompletion::Cancelled
            }
            DerivedRasterStageOutcome::Staged(staged) => SubjectMaskCompletion::Staged {
                request_id: receipt.request_id,
                generation: receipt.generation,
                proposal_token: self.register_proposal(staged)?,
            },
            DerivedRasterStageOutcome::Unavailable { reason } => {
                SubjectMaskCompletion::Unavailable { reason }
            }
            DerivedRasterStageOutcome::Cancelled => SubjectMaskCompletion::Cancelled,
            DerivedRasterStageOutcome::Failed { failure } => {
                SubjectMaskCompletion::Failed { failure }
            }
        };
        Ok(completion)
    }

    pub(crate) fn register_proposal(
        &self,
        staged: StagedDerivedRasterProposal,
    ) -> Result<u64, SubjectMaskServiceError> {
        let mut proposals = self
            .proposals
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
        if proposals.len() >= MAX_STAGED_SUBJECT_MASK_PROPOSALS {
            return Err(SubjectMaskServiceError::TooManyStagedProposals);
        }
        let token = next_token(&self.next_proposal_token)?;
        proposals.insert(token, staged);
        Ok(token)
    }

    /// Reads one current staged soft mask for transient presentation without
    /// consuming its move-only apply authority or publishing durable bytes.
    pub(crate) fn proposal_preview(
        &self,
        proposal_token: u64,
    ) -> Result<SubjectMaskProposalPreview, SubjectMaskServiceError> {
        let (generation, mask) = {
            let proposals = self
                .proposals
                .lock()
                .map_err(|_| SubjectMaskServiceError::StatePoisoned)?;
            let staged = proposals
                .get(&proposal_token)
                .ok_or(SubjectMaskServiceError::UnknownProposal(proposal_token))?;
            let AiGeneratedPayload::SoftMask(mask) = staged.payload() else {
                return Err(SubjectMaskServiceError::ExpectedSoftMaskProposal);
            };
            (staged.generation(), mask.clone())
        };

        let sample_bytes = match mask.encoding {
            SoftMaskEncoding::Gray8Unorm => 1_u64,
            SoftMaskEncoding::Gray16Float => 2_u64,
        };
        let expected_byte_len = u64::from(mask.raster_extent.width)
            .checked_mul(u64::from(mask.raster_extent.height))
            .and_then(|pixels| pixels.checked_mul(sample_bytes))
            .ok_or(SubjectMaskServiceError::ProposalPreviewTooLarge)?;
        if expected_byte_len != mask.artifact.byte_len() {
            return Err(SubjectMaskServiceError::InvalidProposalByteLength {
                expected: expected_byte_len,
                actual: mask.artifact.byte_len(),
            });
        }
        let preview_len = usize::try_from(expected_byte_len)
            .map_err(|_| SubjectMaskServiceError::ProposalPreviewTooLarge)?;
        if preview_len > MAX_SUBJECT_MASK_PREVIEW_BYTES {
            return Err(SubjectMaskServiceError::ProposalPreviewTooLarge);
        }

        let file = self.store.open_staged_proposal(&mask.artifact)?;
        let mut samples = Vec::with_capacity(preview_len);
        file.take(expected_byte_len.saturating_add(1))
            .read_to_end(&mut samples)
            .map_err(SubjectMaskServiceError::ReadProposalPreview)?;
        if samples.len() != preview_len {
            return Err(SubjectMaskServiceError::InvalidProposalByteLength {
                expected: expected_byte_len,
                actual: u64::try_from(samples.len()).unwrap_or(u64::MAX),
            });
        }
        Ok(SubjectMaskProposalPreview {
            generation,
            raster_extent: mask.raster_extent,
            coordinate_extent: mask.coordinate_extent,
            encoding: mask.encoding,
            samples,
        })
    }

    /// Consumes runtime authority without publishing a durable object.
    ///
    /// The rebuildable, content-addressed proposal bytes may remain until
    /// ordinary cache maintenance; dropping this move-only token guarantees
    /// they can no longer enter a Recipe through this desktop session.
    pub(crate) fn discard_proposal(
        &self,
        proposal_token: u64,
    ) -> Result<(), SubjectMaskServiceError> {
        self.proposals
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?
            .remove(&proposal_token)
            .map(|_| ())
            .ok_or(SubjectMaskServiceError::UnknownProposal(proposal_token))
    }

    /// Consumes one opaque proposal token. Stale generation consumes the
    /// runtime authority without publishing a durable object.
    pub(crate) fn promote_proposal(
        &self,
        proposal_token: u64,
        current_generation: u64,
        invert: bool,
    ) -> Result<MaskDefinition, SubjectMaskServiceError> {
        let staged = self
            .proposals
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?
            .remove(&proposal_token)
            .ok_or(SubjectMaskServiceError::UnknownProposal(proposal_token))?;
        let mut store = self.store.clone();
        let managed =
            promote_staged_derived_raster_if_current(&mut store, current_generation, staged)?;
        managed_soft_mask_definition(&managed, invert).map_err(SubjectMaskServiceError::Store)
    }

    fn take_job(&self, job_token: u64) -> Result<Option<SubjectMaskJob>, SubjectMaskServiceError> {
        Ok(self
            .jobs
            .lock()
            .map_err(|_| SubjectMaskServiceError::StatePoisoned)?
            .remove(&job_token))
    }
}

#[derive(Debug)]
struct SubjectMaskJob {
    cancellation: CancellationToken,
    preview_render_token: Option<u64>,
}

#[derive(Debug)]
enum SubjectMaskInputSession {
    Vacant,
    Preparing(SubjectMaskInputIdentity),
    Ready {
        identity: SubjectMaskInputIdentity,
        input: PreparedSubjectMaskInput,
        people: Option<Arc<[SubjectMaskPersonCandidate]>>,
        parsed_people: BTreeMap<u32, Arc<ParsedSubjectMaskPerson>>,
    },
}

#[derive(Debug)]
struct ReadySubjectMaskInputSession<'a> {
    people: &'a Option<Arc<[SubjectMaskPersonCandidate]>>,
    parsed_people: &'a BTreeMap<u32, Arc<ParsedSubjectMaskPerson>>,
}

#[derive(Debug)]
struct ReadySubjectMaskInputSessionMut<'a> {
    people: &'a mut Option<Arc<[SubjectMaskPersonCandidate]>>,
    parsed_people: &'a mut BTreeMap<u32, Arc<ParsedSubjectMaskPerson>>,
}

fn ready_input_session<'a>(
    sessions: &'a BTreeMap<u64, SubjectMaskInputSession>,
    input_session_token: u64,
    identity: &SubjectMaskInputIdentity,
) -> Result<ReadySubjectMaskInputSession<'a>, SubjectMaskServiceError> {
    match sessions.get(&input_session_token) {
        Some(SubjectMaskInputSession::Ready {
            identity: current,
            people,
            parsed_people,
            ..
        }) if current == identity => Ok(ReadySubjectMaskInputSession {
            people,
            parsed_people,
        }),
        Some(SubjectMaskInputSession::Ready { .. })
        | Some(SubjectMaskInputSession::Preparing(_)) => Err(
            SubjectMaskServiceError::InputIdentityChanged(input_session_token),
        ),
        Some(SubjectMaskInputSession::Vacant) => Err(
            SubjectMaskServiceError::InputPreparationNotReserved(input_session_token),
        ),
        None => Err(SubjectMaskServiceError::UnknownInputSession(
            input_session_token,
        )),
    }
}

fn ready_input_session_mut<'a>(
    sessions: &'a mut BTreeMap<u64, SubjectMaskInputSession>,
    input_session_token: u64,
    identity: &SubjectMaskInputIdentity,
) -> Result<ReadySubjectMaskInputSessionMut<'a>, SubjectMaskServiceError> {
    match sessions.get_mut(&input_session_token) {
        Some(SubjectMaskInputSession::Ready {
            identity: current,
            people,
            parsed_people,
            ..
        }) if current == identity => Ok(ReadySubjectMaskInputSessionMut {
            people,
            parsed_people,
        }),
        Some(SubjectMaskInputSession::Ready { .. })
        | Some(SubjectMaskInputSession::Preparing(_)) => Err(
            SubjectMaskServiceError::InputIdentityChanged(input_session_token),
        ),
        Some(SubjectMaskInputSession::Vacant) => Err(
            SubjectMaskServiceError::InputPreparationNotReserved(input_session_token),
        ),
        None => Err(SubjectMaskServiceError::UnknownInputSession(
            input_session_token,
        )),
    }
}

fn resident_input_session_bytes(
    sessions: &BTreeMap<u64, SubjectMaskInputSession>,
) -> Result<usize, SubjectMaskServiceError> {
    sessions
        .values()
        .filter_map(|session| match session {
            SubjectMaskInputSession::Ready {
                input,
                people,
                parsed_people,
                ..
            } => Some(
                std::iter::once(input.bytes.len())
                    .chain(
                        people
                            .iter()
                            .flat_map(|people| people.iter())
                            .map(SubjectMaskPersonCandidate::resident_byte_len),
                    )
                    .chain(
                        parsed_people
                            .values()
                            .map(|parsed| parsed.resident_byte_len()),
                    )
                    .try_fold(0_usize, usize::checked_add),
            ),
            SubjectMaskInputSession::Vacant | SubjectMaskInputSession::Preparing(_) => None,
        })
        .try_fold(0_usize, |total, session_bytes| {
            session_bytes.and_then(|session_bytes| total.checked_add(session_bytes))
        })
        .ok_or(SubjectMaskServiceError::InputBudgetExceeded)
}

fn next_token(sequence: &AtomicU64) -> Result<u64, SubjectMaskServiceError> {
    sequence
        .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |current| {
            current.checked_add(1)
        })
        .map(|previous| previous + 1)
        .map_err(|_| SubjectMaskServiceError::TokenExhausted)
}

#[derive(Debug, Error)]
pub(crate) enum SubjectMaskServiceError {
    #[error("subject-mask service state is poisoned")]
    StatePoisoned,
    #[error("too many subject-mask jobs are active")]
    TooManyActiveJobs,
    #[error("too many subject-mask input sessions are active")]
    TooManyActiveInputSessions,
    #[error("too many subject-mask proposals are awaiting an apply decision")]
    TooManyStagedProposals,
    #[error("subject-mask token space is exhausted")]
    TokenExhausted,
    #[error("subject-mask job {0} is unknown")]
    UnknownJob(u64),
    #[error("subject-mask input session {0} is unknown")]
    UnknownInputSession(u64),
    #[error("subject-mask input session {0} already has preparation in flight")]
    InputPreparationInFlight(u64),
    #[error("subject-mask input session {0} identity changed")]
    InputIdentityChanged(u64),
    #[error("subject-mask input session {0} did not reserve preparation")]
    InputPreparationNotReserved(u64),
    #[error("subject-mask input JPEG size {0} is outside the bounded contract")]
    InvalidInputSize(usize),
    #[error("subject-mask input sessions exceed the aggregate resident byte budget")]
    InputBudgetExceeded,
    #[error("subject-mask people discovery returned {0} people, exceeding the session bound")]
    TooManyDetectedPeople(usize),
    #[error("subject-mask people were not discovered for input session {0}")]
    PeopleNotDiscovered(u64),
    #[error("subject-mask person {0} is unknown")]
    UnknownPerson(u32),
    #[error("subject-mask proposal {0} is unknown or was already consumed")]
    UnknownProposal(u64),
    #[error("subject-mask preview render token must be non-zero")]
    InvalidPreviewToken,
    #[error("subject-mask job {0} already has a preview render")]
    PreviewAlreadyAttached(u64),
    #[error("subject-mask job {0} was cancelled before preview attachment")]
    JobCancelled(u64),
    #[error("subject-mask proposal is not a soft mask")]
    ExpectedSoftMaskProposal,
    #[error("subject-mask proposal preview exceeds the bounded in-memory presentation budget")]
    ProposalPreviewTooLarge,
    #[error(
        "subject-mask proposal byte length is invalid: expected {expected} bytes, found {actual}"
    )]
    InvalidProposalByteLength { expected: u64, actual: u64 },
    #[error("failed to read verified subject-mask proposal bytes")]
    ReadProposalPreview(#[source] std::io::Error),
    #[error(transparent)]
    Store(#[from] DerivedRasterStoreError),
    #[error(transparent)]
    Promotion(#[from] CurrentDerivedRasterPromotionFailure),
}

#[cfg(test)]
mod tests;
