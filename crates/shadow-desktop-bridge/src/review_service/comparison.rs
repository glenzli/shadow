//! Session-local Review comparison presentation and reversible feedback lifecycle.
//!
//! This owner keeps the complete pending-presentation aggregate: exact visual
//! tickets, verified encoded bytes, decoded-frame receipts, readiness,
//! append-only pairwise feedback, and the session-scoped forget boundary.
//! Signed grid/session handles remain with the [`super::visual_handles`]
//! sibling because Library and Review both consume that authorization
//! protocol.

use std::{
    collections::{HashMap, HashSet},
    sync::Mutex,
};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_ai::{
    FeedbackAction, LearningScope, NewFeedbackEvent, NewFeedbackForgetFact, PairwiseOutcome,
    PresentationContext, PresentedCandidate, PresentedFitMode, PresentedVisualArtifact,
    PresentedVisualFrame, PresentedVisualProvenance, PresentedVisualRole,
    UnitInterval as AiUnitInterval,
};
use shadow_catalog::CachedArtifactRole;
use uuid::Uuid;

use crate::{digest_hex::encode_hex, ffi, wall_clock::current_time_ms};

use super::{ReviewService, ReviewVisualSelection, visual_handles::is_lower_hex};

const MAX_PENDING_REVIEW_COMPARISONS: usize = 64;
pub(crate) const REVIEW_COMPARE_SURFACE_ID: &str = "shadow.desktop.review-compare";
pub(crate) const REVIEW_COMPARE_SURFACE_REVISION: u64 = 1;
pub(crate) const REVIEW_COMPARE_DECODER_ID: &str = "qt.qimagereader";
pub(crate) const REVIEW_COMPARE_PIXEL_FORMAT: &str = "rgba8888_unpremultiplied_row_major";
pub(crate) const REVIEW_COMPARE_PIXEL_HASH_ALGORITHM: &str = "sha256";
const REVIEW_FEEDBACK_FORGET_REASON: &str =
    "user removed this Review comparison from local preference learning";

/// All mutable state that exists only for one desktop Review session.
#[derive(Debug)]
pub(super) struct ReviewComparisonSession {
    feedback_session_id: String,
    registry: Mutex<ReviewComparisonRegistry>,
    active_feedback_event_ids: Mutex<HashSet<String>>,
}

impl ReviewComparisonSession {
    pub(super) fn new() -> Self {
        Self {
            feedback_session_id: Uuid::now_v7().to_string(),
            registry: Mutex::new(ReviewComparisonRegistry::default()),
            active_feedback_event_ids: Mutex::new(HashSet::new()),
        }
    }
}

#[derive(Debug, Default)]
struct ReviewComparisonRegistry {
    presentations: HashMap<String, PendingReviewComparison>,
}

#[derive(Debug)]
struct PendingReviewComparison {
    left: PendingReviewVisual,
    right: PendingReviewVisual,
    ready: bool,
}

#[derive(Debug)]
struct PendingReviewVisual {
    request_ticket: String,
    selection: ReviewVisualSelection,
    bytes_verified: bool,
    frame: Option<PresentedVisualFrame>,
}

impl ReviewService {
    #[cfg(test)]
    pub(crate) fn feedback_session_id(&self) -> &str {
        &self.comparison.feedback_session_id
    }

    /// Loads one exact comparison ticket after the parent rejects both signed
    /// grid-handle families. The record is cloned before filesystem I/O so a
    /// concurrent cancellation can still prevent a stale acknowledgement.
    pub(super) fn load_comparison_visual(
        &self,
        request_ticket: &str,
    ) -> AnyResult<ffi::FfiVisualPayload> {
        let selection = {
            let registry = self
                .comparison
                .registry
                .lock()
                .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
            pending_visual(&registry, request_ticket)
                .map(|slot| slot.selection.clone())
                .ok_or_else(|| anyhow!("unknown or expired Review visual request ticket"))?
        };
        let bytes = self.loader.load_bytes(&selection.record)?;
        {
            let mut registry = self
                .comparison
                .registry
                .lock()
                .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
            let slot = pending_visual_mut(&mut registry, request_ticket)
                .ok_or_else(|| anyhow!("Review visual request was canceled while loading"))?;
            if slot.selection.photo_id != selection.photo_id
                || slot.selection.record != selection.record
            {
                bail!("Review visual request identity changed while loading");
            }
            slot.bytes_verified = true;
        }
        Ok(ffi::FfiVisualPayload {
            bytes,
            requires_frame_receipt: true,
        })
    }

    pub(crate) fn prepare_comparison(
        &self,
        left_grid_handle: &str,
        right_grid_handle: &str,
    ) -> AnyResult<ffi::FfiReviewComparisonPresentation> {
        let left = self.decode_grid_visual_handle(left_grid_handle)?;
        let right = self.decode_grid_visual_handle(right_grid_handle)?;
        if left.photo_id == right.photo_id {
            bail!("Review comparison requires two different photos");
        }

        let mut registry = self
            .comparison
            .registry
            .lock()
            .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
        if registry.presentations.len() >= MAX_PENDING_REVIEW_COMPARISONS {
            bail!(
                "Review comparison registry is full; cancel an abandoned comparison before retrying"
            );
        }
        let presentation_id = unique_presentation_id(&registry);
        let left_request_ticket = unique_request_ticket(&registry);
        let right_request_ticket = unique_request_ticket_excluding(&registry, &left_request_ticket);
        registry.presentations.insert(
            presentation_id.clone(),
            PendingReviewComparison {
                left: PendingReviewVisual {
                    request_ticket: left_request_ticket.clone(),
                    selection: left,
                    bytes_verified: false,
                    frame: None,
                },
                right: PendingReviewVisual {
                    request_ticket: right_request_ticket.clone(),
                    selection: right,
                    bytes_verified: false,
                    frame: None,
                },
                ready: false,
            },
        );
        Ok(ffi::FfiReviewComparisonPresentation {
            presentation_id,
            left_request_ticket,
            right_request_ticket,
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn record_visual_frame(
        &self,
        request_ticket: &str,
        decoder_version: &str,
        requested_width: u32,
        requested_height: u32,
        decoded_width: u32,
        decoded_height: u32,
        pixel_hash_hex: &str,
    ) -> AnyResult<()> {
        validate_frame_receipt(
            decoder_version,
            requested_width,
            requested_height,
            decoded_width,
            decoded_height,
            pixel_hash_hex,
        )?;
        let frame = PresentedVisualFrame {
            surface_id: REVIEW_COMPARE_SURFACE_ID.to_owned(),
            surface_revision: REVIEW_COMPARE_SURFACE_REVISION,
            fit_mode: PresentedFitMode::PreserveAspectFit,
            decoder_id: REVIEW_COMPARE_DECODER_ID.to_owned(),
            decoder_version: decoder_version.to_owned(),
            auto_transform: true,
            requested_width,
            requested_height,
            decoded_width,
            decoded_height,
            pixel_format: REVIEW_COMPARE_PIXEL_FORMAT.to_owned(),
            pixel_hash_algorithm: REVIEW_COMPARE_PIXEL_HASH_ALGORITHM.to_owned(),
            pixel_hash_hex: pixel_hash_hex.to_owned(),
        };
        let mut registry = self
            .comparison
            .registry
            .lock()
            .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
        let slot = pending_visual_mut(&mut registry, request_ticket)
            .ok_or_else(|| anyhow!("unknown or expired Review visual request ticket"))?;
        if !slot.bytes_verified {
            bail!("Review visual bytes must load successfully before recording a frame receipt");
        }
        match &slot.frame {
            None => slot.frame = Some(frame),
            Some(existing) if existing == &frame => {}
            Some(_) => bail!("Review visual request already has a different frame receipt"),
        }
        Ok(())
    }

    pub(crate) fn confirm_comparison_ready(
        &self,
        presentation_id: &str,
        left_request_ticket: &str,
        right_request_ticket: &str,
    ) -> AnyResult<()> {
        let mut registry = self
            .comparison
            .registry
            .lock()
            .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
        let presentation = registry
            .presentations
            .get_mut(presentation_id)
            .ok_or_else(|| anyhow!("unknown or expired Review comparison presentation"))?;
        if presentation.left.request_ticket != left_request_ticket
            || presentation.right.request_ticket != right_request_ticket
        {
            bail!("Review comparison tickets do not belong to this presentation");
        }
        for (side, slot) in [("left", &presentation.left), ("right", &presentation.right)] {
            if !slot.bytes_verified || slot.frame.is_none() {
                bail!("{side} Review comparison visual is not fully presented");
            }
        }
        presentation.ready = true;
        Ok(())
    }

    pub(crate) fn cancel_comparison(&self, presentation_id: &str) -> AnyResult<()> {
        let mut registry = self
            .comparison
            .registry
            .lock()
            .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
        registry
            .presentations
            .remove(presentation_id)
            .ok_or_else(|| anyhow!("unknown or expired Review comparison presentation"))?;
        Ok(())
    }

    pub(crate) fn record_comparison(
        &self,
        presentation_id: &str,
        outcome: ffi::FfiPairwiseOutcome,
    ) -> AnyResult<ffi::FfiFeedbackReceipt> {
        let outcome = pairwise_outcome(outcome)?;
        // Acquire the undo set first so a poisoned lock cannot leave durable evidence that this
        // UI session cannot subsequently forget.
        let mut active_event_ids = self
            .comparison
            .active_feedback_event_ids
            .lock()
            .map_err(|_| anyhow!("Review feedback mutation lock is poisoned"))?;
        let mut registry = self
            .comparison
            .registry
            .lock()
            .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
        let presentation = registry
            .presentations
            .get(presentation_id)
            .ok_or_else(|| anyhow!("unknown or expired Review comparison presentation"))?;
        if !presentation.ready {
            bail!("Review comparison must be confirmed ready before recording feedback");
        }
        let left = presentation.left.selection.photo_id;
        let right = presentation.right.selection.photo_id;
        let left_visual = presented_visual(&presentation.left)?;
        let right_visual = presented_visual(&presentation.right)?;
        let occurred_at_unix_ms = current_time_ms()?;
        let event = self.catalog.append_feedback_event(&NewFeedbackEvent {
            event_id: Uuid::now_v7().to_string(),
            occurred_at_unix_ms,
            scope: LearningScope::Global,
            presentation: PresentationContext {
                session_id: self.comparison.feedback_session_id.clone(),
                group_id: None,
                candidates: vec![
                    PresentedCandidate {
                        photo_id: left,
                        position: 0,
                        visible_fraction: AiUnitInterval::ONE,
                        inspected_at_one_to_one: false,
                        feature: None,
                        visual: Some(left_visual),
                    },
                    PresentedCandidate {
                        photo_id: right,
                        position: 1,
                        visible_fraction: AiUnitInterval::ONE,
                        inspected_at_one_to_one: false,
                        feature: None,
                        visual: Some(right_visual),
                    },
                ],
                active_model: None,
            },
            action: FeedbackAction::PairwiseComparison {
                left,
                right,
                outcome,
            },
        })?;
        registry.presentations.remove(presentation_id);
        active_event_ids.insert(event.event_id.clone());
        Ok(ffi::FfiFeedbackReceipt {
            event_id: event.event_id,
            sequence: event.sequence,
            occurred_at_unix_ms: event.occurred_at_unix_ms,
        })
    }

    pub(crate) fn forget_feedback(&self, event_id: &str) -> AnyResult<ffi::FfiForgetReceipt> {
        let target_event_id = Uuid::parse_str(event_id)
            .with_context(|| format!("parse Review feedback event id {event_id}"))?
            .to_string();
        let mut active_event_ids = self
            .comparison
            .active_feedback_event_ids
            .lock()
            .map_err(|_| anyhow!("Review feedback mutation lock is poisoned"))?;
        if !active_event_ids.contains(&target_event_id) {
            bail!(
                "Review feedback event {target_event_id} is not an active comparison issued by this Review session"
            );
        }
        let occurred_at_unix_ms = current_time_ms()?;
        let fact = self
            .catalog
            .append_feedback_forget_fact(&NewFeedbackForgetFact {
                fact_id: Uuid::now_v7().to_string(),
                target_event_id: target_event_id.clone(),
                occurred_at_unix_ms,
                reason: Some(REVIEW_FEEDBACK_FORGET_REASON.to_owned()),
            })?;
        active_event_ids.remove(&target_event_id);
        Ok(ffi::FfiForgetReceipt {
            fact_id: fact.fact_id,
            target_event_id: fact.target_event_id,
            sequence: fact.sequence,
            occurred_at_unix_ms: fact.occurred_at_unix_ms,
        })
    }
}

fn pending_visual<'a>(
    registry: &'a ReviewComparisonRegistry,
    request_ticket: &str,
) -> Option<&'a PendingReviewVisual> {
    registry.presentations.values().find_map(|presentation| {
        if presentation.left.request_ticket == request_ticket {
            Some(&presentation.left)
        } else if presentation.right.request_ticket == request_ticket {
            Some(&presentation.right)
        } else {
            None
        }
    })
}

fn pending_visual_mut<'a>(
    registry: &'a mut ReviewComparisonRegistry,
    request_ticket: &str,
) -> Option<&'a mut PendingReviewVisual> {
    registry
        .presentations
        .values_mut()
        .find_map(|presentation| {
            if presentation.left.request_ticket == request_ticket {
                Some(&mut presentation.left)
            } else if presentation.right.request_ticket == request_ticket {
                Some(&mut presentation.right)
            } else {
                None
            }
        })
}

fn unique_presentation_id(registry: &ReviewComparisonRegistry) -> String {
    loop {
        let candidate = Uuid::now_v7().to_string();
        if !registry.presentations.contains_key(&candidate) {
            return candidate;
        }
    }
}

fn unique_request_ticket(registry: &ReviewComparisonRegistry) -> String {
    unique_request_ticket_excluding(registry, "")
}

fn unique_request_ticket_excluding(registry: &ReviewComparisonRegistry, excluded: &str) -> String {
    loop {
        let candidate = Uuid::now_v7().to_string();
        if candidate != excluded && pending_visual(registry, &candidate).is_none() {
            return candidate;
        }
    }
}

fn presented_visual(slot: &PendingReviewVisual) -> AnyResult<PresentedVisualProvenance> {
    if !slot.bytes_verified {
        bail!("Review visual bytes were not verified");
    }
    let frame = slot
        .frame
        .clone()
        .ok_or_else(|| anyhow!("Review visual has no decoded-frame receipt"))?;
    let record = &slot.selection.record;
    let role = match record.artifact.role {
        CachedArtifactRole::RecipePreview => PresentedVisualRole::RecipePreview,
        CachedArtifactRole::EmbeddedPreview => PresentedVisualRole::EmbeddedPreview,
        CachedArtifactRole::GeneratedProxy => PresentedVisualRole::GeneratedProxy,
    };
    Ok(PresentedVisualProvenance {
        artifact: PresentedVisualArtifact {
            representation_id: record.representation_id,
            source_byte_len: record.source.byte_len,
            source_modified_at_ms: record.source.modified_at_ms,
            role,
            variant_key: record.artifact.variant_key.clone(),
            generator_id: record.artifact.generator_id.clone(),
            generator_version: record.artifact.generator_version.clone(),
            provider_preview_id: record
                .artifact
                .provider_preview_id
                .map(u64::try_from)
                .transpose()
                .context("provider preview id does not fit Review provenance")?,
            blob_algorithm: record.artifact.blob_algorithm.clone(),
            blob_digest_hex: encode_hex(&record.artifact.blob_digest),
            blob_byte_len: record.artifact.blob_byte_len,
            codec: record.artifact.codec.as_str().to_owned(),
            byte_order: record.artifact.byte_order.as_str().to_owned(),
            width: record.artifact.dimensions.width,
            height: record.artifact.dimensions.height,
            bits_per_channel: record.artifact.bits_per_channel,
            channels: record.artifact.channels,
            created_at_ms: record.artifact.created_at_ms,
        },
        frame,
    })
}

fn validate_frame_receipt(
    decoder_version: &str,
    requested_width: u32,
    requested_height: u32,
    decoded_width: u32,
    decoded_height: u32,
    pixel_hash_hex: &str,
) -> AnyResult<()> {
    if decoder_version.trim().is_empty() || decoder_version.len() > 256 {
        bail!("Review visual decoder version must contain 1 through 256 bytes");
    }
    if [
        requested_width,
        requested_height,
        decoded_width,
        decoded_height,
    ]
    .contains(&0)
    {
        bail!("Review visual requested and decoded dimensions must be non-zero");
    }
    if pixel_hash_hex.len() != 64 || !is_lower_hex(pixel_hash_hex) {
        bail!("Review visual pixel hash must be 64 lowercase hexadecimal characters");
    }
    Ok(())
}

fn pairwise_outcome(outcome: ffi::FfiPairwiseOutcome) -> AnyResult<PairwiseOutcome> {
    match outcome {
        ffi::FfiPairwiseOutcome::LeftPreferred => Ok(PairwiseOutcome::LeftPreferred),
        ffi::FfiPairwiseOutcome::RightPreferred => Ok(PairwiseOutcome::RightPreferred),
        ffi::FfiPairwiseOutcome::KeepBoth => Ok(PairwiseOutcome::KeepBoth),
        ffi::FfiPairwiseOutcome::KeepNeither => Ok(PairwiseOutcome::KeepNeither),
        ffi::FfiPairwiseOutcome::CannotCompare => Ok(PairwiseOutcome::CannotCompare),
        _ => bail!("unsupported Review comparison outcome"),
    }
}
