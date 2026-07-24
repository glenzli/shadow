//! Stateful Review operations for the desktop bridge.
//!
//! This service owns only the session-local side of gallery review: signed
//! visual handles, comparison presentation receipts and reversible learning
//! evidence.  Qt DTO shaping remains in the bridge facade, while the Catalog
//! stays the durable source of photo decisions and feedback facts.

use std::{
    collections::{HashMap, HashSet},
    path::Path,
    sync::Mutex,
};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use serde::{Deserialize, Serialize};
use shadow_ai::{
    FeedbackAction, LearningScope, NewFeedbackEvent, NewFeedbackForgetFact, PairwiseOutcome,
    PresentationContext, PresentedCandidate, PresentedFitMode, PresentedVisualArtifact,
    PresentedVisualFrame, PresentedVisualProvenance, PresentedVisualRole,
    UnitInterval as AiUnitInterval,
};
use shadow_bridge::{edit_preview_generator_implementation_identity, photo_provider_version};
use shadow_catalog::{
    CachedArtifact, CachedArtifactGeneratorIdentity, CachedArtifactRecord, CachedArtifactRole,
    CatalogHandle, RepresentationFingerprint, ReviewCursor, ReviewItemRecord,
    TechnicalObservationRevision,
};
use shadow_core::{CachedArtifactLoader, technical_analysis_preprocessing_version};
use shadow_domain::{
    ImageDimensions, MAX_PHOTO_RATING, NewPhotoDecisionEvent, PhotoDecisionEvent,
    PhotoDecisionOrigin, PhotoDecisionState, PhotoFlag, PhotoId, PreviewByteOrder, PreviewCodec,
};
use uuid::Uuid;

use crate::{
    current_time_ms, ffi,
    preview_cache_identity::{
        EDIT_PREVIEW_GENERATOR_ID, current_source_environment_cache_identity,
        edit_preview_generator_version,
    },
};

const GRID_VISUAL_HANDLE_PREFIX: &str = "shadow-grid-visual-v1.";
const GRID_VISUAL_HANDLE_SCHEMA_VERSION: u8 = 2;
const MAX_GRID_VISUAL_PAYLOAD_BYTES: usize = 16 * 1_024;
const MAX_PENDING_REVIEW_COMPARISONS: usize = 64;
pub(crate) const REVIEW_COMPARE_SURFACE_ID: &str = "shadow.desktop.review-compare";
pub(crate) const REVIEW_COMPARE_SURFACE_REVISION: u64 = 1;
pub(crate) const REVIEW_COMPARE_DECODER_ID: &str = "qt.qimagereader";
pub(crate) const REVIEW_COMPARE_PIXEL_FORMAT: &str = "rgba8888_unpremultiplied_row_major";
pub(crate) const REVIEW_COMPARE_PIXEL_HASH_ALGORITHM: &str = "sha256";
const REVIEW_FEEDBACK_FORGET_REASON: &str =
    "user removed this Review comparison from local preference learning";

/// The exact cached artifact chosen for a Review visual.
#[derive(Debug, Clone)]
pub(crate) struct ReviewVisualSelection {
    pub(crate) photo_id: PhotoId,
    pub(crate) record: CachedArtifactRecord,
}

/// Session-local review state.  Every durable mutation still goes through the
/// Catalog actor, so this type can be discarded whenever a desktop session ends.
#[derive(Debug)]
pub(crate) struct ReviewService {
    catalog: CatalogHandle,
    loader: CachedArtifactLoader,
    feedback_session_id: String,
    visual_signing_key: [u8; 32],
    comparisons: Mutex<ReviewComparisonRegistry>,
    active_feedback_event_ids: Mutex<HashSet<String>>,
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
    pub(crate) fn new(catalog: CatalogHandle, loader: CachedArtifactLoader) -> Self {
        Self {
            catalog,
            loader,
            feedback_session_id: Uuid::now_v7().to_string(),
            visual_signing_key: new_visual_signing_key(),
            comparisons: Mutex::new(ReviewComparisonRegistry::default()),
            active_feedback_event_ids: Mutex::new(HashSet::new()),
        }
    }

    #[cfg(test)]
    pub(crate) fn feedback_session_id(&self) -> &str {
        &self.feedback_session_id
    }

    pub(crate) fn load_visual(&self, ticket: &str) -> AnyResult<ffi::FfiVisualPayload> {
        if ticket.starts_with(GRID_VISUAL_HANDLE_PREFIX) {
            let selection = self.decode_grid_visual_handle(ticket)?;
            return Ok(ffi::FfiVisualPayload {
                bytes: self.loader.load_bytes(&selection.record)?,
                requires_frame_receipt: false,
            });
        }

        // Clone the exact record before filesystem I/O. A concurrent cancellation then prevents
        // the following acknowledgement, so a frame receipt can never attach to stale bytes.
        let selection = {
            let registry = self
                .comparisons
                .lock()
                .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
            pending_visual(&registry, ticket)
                .map(|slot| slot.selection.clone())
                .ok_or_else(|| anyhow!("unknown or expired Review visual request ticket"))?
        };
        let bytes = self.loader.load_bytes(&selection.record)?;
        {
            let mut registry = self
                .comparisons
                .lock()
                .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
            let slot = pending_visual_mut(&mut registry, ticket)
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

    pub(crate) fn review_page(
        &self,
        cursor_path: &str,
        cursor_representation_id: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiReviewPage> {
        let cursor = parse_cursor(cursor_path, cursor_representation_id)?;
        let revision =
            TechnicalObservationRevision::current(technical_analysis_preprocessing_version());
        let source_environment =
            current_source_environment_cache_identity(&photo_provider_version());
        let recipe_preview_generator = CachedArtifactGeneratorIdentity {
            generator_id: EDIT_PREVIEW_GENERATOR_ID.to_owned(),
            generator_version: edit_preview_generator_version(
                &source_environment,
                &edit_preview_generator_implementation_identity(),
            ),
        };
        let page = self
            .catalog
            .review_page_with_technical_and_recipe_preview_generator(
                cursor.as_ref(),
                usize::try_from(limit).unwrap_or(usize::MAX),
                &revision,
                &recipe_preview_generator,
            )?;
        let (has_more, next_cursor_path, next_cursor_representation_id) =
            if let Some(cursor) = page.next_cursor {
                (
                    true,
                    cursor.display_path,
                    cursor.representation_id.to_string(),
                )
            } else {
                (false, String::new(), String::new())
            };
        Ok(ffi::FfiReviewPage {
            total_items: page.total_items,
            items: page
                .items
                .into_iter()
                .map(|record| self.review_item(record))
                .collect::<AnyResult<Vec<_>>>()?,
            has_more,
            next_cursor_path,
            next_cursor_representation_id,
        })
    }

    // Keep the complete gallery DTO mapping next to the visual-handle contract.
    // Adding one EXIF field then touches precisely this service and the CXX ABI.
    #[allow(clippy::too_many_lines)]
    fn review_item(&self, record: ReviewItemRecord) -> AnyResult<ffi::FfiReviewItem> {
        let visual_handle = record
            .visual
            .as_ref()
            .map(|visual| {
                self.encode_grid_visual_handle(&ReviewVisualSelection {
                    photo_id: record.photo_id,
                    record: visual.clone(),
                })
            })
            .transpose()?
            .unwrap_or_default();
        let (visual_role, visual_width, visual_height, has_visual) = record.visual.map_or_else(
            || (String::new(), 0, 0, false),
            |visual| {
                (
                    role_name(visual.artifact.role).to_owned(),
                    visual.artifact.dimensions.width,
                    visual.artifact.dimensions.height,
                    true,
                )
            },
        );
        let technical = record.technical;
        let metadata = record.metadata;
        let has_metadata = metadata.is_some();
        let (
            camera_make,
            camera_model,
            lens_make,
            lens_model,
            captured_at_unix_seconds,
            iso_speed,
            exposure_time_seconds,
            aperture_f_number,
            focal_length_mm,
            focal_length_35mm,
            raw_width,
            raw_height,
            sensor_bits,
            cfa_pattern,
            dng_version,
        ) = metadata.map_or_else(
            || {
                (
                    String::new(),
                    String::new(),
                    String::new(),
                    String::new(),
                    0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0,
                    0,
                    0,
                    String::new(),
                    String::new(),
                )
            },
            |metadata| {
                (
                    metadata.make,
                    metadata.model,
                    metadata.lens_make,
                    metadata.lens_model,
                    metadata.captured_at_unix_seconds,
                    metadata.iso_speed,
                    metadata.exposure_time_seconds,
                    metadata.aperture_f_number,
                    metadata.focal_length_mm,
                    metadata.focal_length_35mm,
                    metadata.raw_dimensions.width,
                    metadata.raw_dimensions.height,
                    metadata.sensor_bits,
                    metadata.cfa_pattern,
                    metadata.dng_version.unwrap_or_default(),
                )
            },
        );
        let has_technical_observation = technical.is_some();
        let (
            technical_input_width,
            technical_input_height,
            technical_preprocessing_version,
            technical_implementation_version,
            mean_luma,
            p01_luma,
            p50_luma,
            p99_luma,
            near_black_fraction,
            near_white_fraction,
            laplacian_variance,
            edge_energy,
        ) = technical.map_or_else(
            || {
                (
                    0,
                    0,
                    String::new(),
                    String::new(),
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                )
            },
            |technical| {
                (
                    technical.input_width,
                    technical.input_height,
                    technical.preprocessing_version,
                    technical.implementation_version,
                    technical.mean_luma,
                    technical.p01_luma,
                    technical.p50_luma,
                    technical.p99_luma,
                    technical.near_black_fraction,
                    technical.near_white_fraction,
                    technical.laplacian_variance,
                    technical.edge_energy,
                )
            },
        );
        Ok(ffi::FfiReviewItem {
            photo_id: record.photo_id.to_string(),
            representation_id: record.representation_id.to_string(),
            visual_handle,
            decision_head_sequence: record.decision.head_sequence,
            decision_flag: ffi_decision_flag(record.decision.flag),
            decision_rating: record.decision.rating,
            has_development_edits: record.has_development_edits,
            title: file_name(&record.location.display_path),
            source_path: record.location.display_path,
            visual_role,
            visual_width,
            visual_height,
            has_visual,
            has_metadata,
            camera_make,
            camera_model,
            lens_make,
            lens_model,
            captured_at_unix_seconds,
            iso_speed,
            exposure_time_seconds,
            aperture_f_number,
            focal_length_mm,
            focal_length_35mm,
            raw_width,
            raw_height,
            sensor_bits,
            cfa_pattern,
            dng_version,
            has_technical_observation,
            technical_input_width,
            technical_input_height,
            technical_preprocessing_version,
            technical_implementation_version,
            mean_luma,
            p01_luma,
            p50_luma,
            p99_luma,
            near_black_fraction,
            near_white_fraction,
            laplacian_variance,
            edge_energy,
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
            .comparisons
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
            .comparisons
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
            .comparisons
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
            .comparisons
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
            .active_feedback_event_ids
            .lock()
            .map_err(|_| anyhow!("Review feedback mutation lock is poisoned"))?;
        let mut registry = self
            .comparisons
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
                session_id: self.feedback_session_id.clone(),
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

    pub(crate) fn photo_decision_state(
        &self,
        photo_id: &str,
    ) -> AnyResult<ffi::FfiPhotoDecisionState> {
        let photo_id: PhotoId = photo_id
            .parse()
            .with_context(|| format!("parse Review decision photo id {photo_id}"))?;
        Ok(ffi_photo_decision_state(
            photo_id,
            self.catalog.photo_decision_state(photo_id)?,
        ))
    }

    pub(crate) fn set_photo_decision(
        &self,
        photo_id: &str,
        expected_head_sequence: u64,
        flag: ffi::FfiDecisionFlag,
        rating: u8,
    ) -> AnyResult<ffi::FfiReviewDecisionMutationReceipt> {
        let photo_id: PhotoId = photo_id
            .parse()
            .with_context(|| format!("parse Review decision photo id {photo_id}"))?;
        if rating > MAX_PHOTO_RATING {
            bail!("Review decision rating must be in 0 through {MAX_PHOTO_RATING}");
        }
        let before = self.catalog.photo_decision_state(photo_id)?;
        if before.head_sequence != expected_head_sequence {
            bail!(
                "stale Review decision head: expected {expected_head_sequence}, current {}",
                before.head_sequence
            );
        }
        let event = self
            .catalog
            .append_photo_decision_event(&NewPhotoDecisionEvent {
                event_id: Uuid::now_v7().to_string(),
                photo_id,
                occurred_at_unix_ms: current_time_ms()?,
                origin: PhotoDecisionOrigin::Human,
                expected_head_sequence,
                before_flag: before.flag,
                before_rating: before.rating,
                after_flag: photo_flag(flag)?,
                after_rating: rating,
            })?;
        Ok(ffi_photo_decision_receipt(event))
    }

    pub(crate) fn encode_grid_visual_handle(
        &self,
        selection: &ReviewVisualSelection,
    ) -> AnyResult<String> {
        let payload = SignedGridVisualPayload::from_selection(selection)?;
        let payload = serde_json::to_vec(&payload).context("encode Review grid visual handle")?;
        if payload.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES {
            bail!("Review grid visual handle payload exceeds its size limit");
        }
        let signature = blake3::keyed_hash(&self.visual_signing_key, &payload);
        Ok(format!(
            "{GRID_VISUAL_HANDLE_PREFIX}{}.{}",
            encode_hex(&payload),
            signature.to_hex()
        ))
    }

    fn decode_grid_visual_handle(&self, handle: &str) -> AnyResult<ReviewVisualSelection> {
        let encoded = handle
            .strip_prefix(GRID_VISUAL_HANDLE_PREFIX)
            .ok_or_else(|| anyhow!("invalid Review grid visual handle prefix"))?;
        let (payload_hex, signature_hex) = encoded
            .split_once('.')
            .ok_or_else(|| anyhow!("malformed Review grid visual handle"))?;
        if payload_hex.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES.saturating_mul(2) {
            bail!("Review grid visual handle payload exceeds its size limit");
        }
        if signature_hex.len() != 64 || !is_lower_hex(signature_hex) {
            bail!("malformed Review grid visual handle signature");
        }
        let payload = decode_hex(payload_hex).context("decode Review grid visual handle")?;
        if payload.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES {
            bail!("Review grid visual handle payload exceeds its size limit");
        }
        let supplied_signature =
            decode_hex_32(signature_hex).context("decode Review grid visual handle signature")?;
        let expected_signature = blake3::keyed_hash(&self.visual_signing_key, &payload);
        if !constant_time_eq(expected_signature.as_bytes(), &supplied_signature) {
            bail!("Review grid visual handle signature is invalid for this session");
        }
        let payload: SignedGridVisualPayload =
            serde_json::from_slice(&payload).context("parse Review grid visual handle")?;
        payload.into_selection()
    }
}

/// Serializable mirror of the Catalog record carried by a signed grid handle.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct SignedGridVisualPayload {
    schema_version: u8,
    photo_id: String,
    representation_id: String,
    source_byte_len: u64,
    source_modified_at_ms: Option<i64>,
    role: String,
    variant_key: String,
    generator_id: String,
    generator_version: String,
    recipe_snapshot_digest_hex: Option<String>,
    provider_preview_id: Option<u64>,
    blob_algorithm: String,
    blob_digest_hex: String,
    blob_byte_len: u64,
    codec: String,
    byte_order: String,
    width: u32,
    height: u32,
    bits_per_channel: u16,
    channels: u16,
    created_at_ms: i64,
}

impl SignedGridVisualPayload {
    fn from_selection(selection: &ReviewVisualSelection) -> AnyResult<Self> {
        let record = &selection.record;
        Ok(Self {
            schema_version: GRID_VISUAL_HANDLE_SCHEMA_VERSION,
            photo_id: selection.photo_id.to_string(),
            representation_id: record.representation_id.to_string(),
            source_byte_len: record.source.byte_len,
            source_modified_at_ms: record.source.modified_at_ms,
            role: record.artifact.role.as_str().to_owned(),
            variant_key: record.artifact.variant_key.clone(),
            generator_id: record.artifact.generator_id.clone(),
            generator_version: record.artifact.generator_version.clone(),
            recipe_snapshot_digest_hex: record
                .artifact
                .recipe_snapshot_digest
                .as_ref()
                .map(|digest| encode_hex(digest)),
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
        })
    }

    fn into_selection(self) -> AnyResult<ReviewVisualSelection> {
        if self.schema_version != GRID_VISUAL_HANDLE_SCHEMA_VERSION {
            bail!(
                "unsupported Review grid visual handle schema {}",
                self.schema_version
            );
        }
        let photo_id = self
            .photo_id
            .parse()
            .context("parse photo id in Review grid visual handle")?;
        let representation_id = self
            .representation_id
            .parse()
            .context("parse representation id in Review grid visual handle")?;
        let role = match self.role.as_str() {
            "recipe_preview" => CachedArtifactRole::RecipePreview,
            "embedded_preview" => CachedArtifactRole::EmbeddedPreview,
            "generated_proxy" => CachedArtifactRole::GeneratedProxy,
            other => bail!("unsupported Review visual artifact role {other:?}"),
        };
        let codec = match self.codec.as_str() {
            "unknown" => PreviewCodec::Unknown,
            "jpeg" => PreviewCodec::Jpeg,
            "bitmap" => PreviewCodec::Bitmap,
            "jpeg_xl" => PreviewCodec::JpegXl,
            "h265" => PreviewCodec::H265,
            other => bail!("unsupported Review visual codec {other:?}"),
        };
        let byte_order = match self.byte_order.as_str() {
            "not_applicable" => PreviewByteOrder::NotApplicable,
            "native" => PreviewByteOrder::Native,
            "little_endian" => PreviewByteOrder::LittleEndian,
            "big_endian" => PreviewByteOrder::BigEndian,
            other => bail!("unsupported Review visual byte order {other:?}"),
        };
        let recipe_snapshot_digest = self
            .recipe_snapshot_digest_hex
            .as_deref()
            .map(decode_hex_32)
            .transpose()
            .context("decode Recipe snapshot digest in Review visual handle")?;
        Ok(ReviewVisualSelection {
            photo_id,
            record: CachedArtifactRecord {
                representation_id,
                source: RepresentationFingerprint {
                    byte_len: self.source_byte_len,
                    modified_at_ms: self.source_modified_at_ms,
                },
                artifact: CachedArtifact {
                    role,
                    variant_key: self.variant_key,
                    generator_id: self.generator_id,
                    generator_version: self.generator_version,
                    recipe_snapshot_digest,
                    provider_preview_id: self
                        .provider_preview_id
                        .map(usize::try_from)
                        .transpose()
                        .context("provider preview id does not fit this platform")?,
                    blob_algorithm: self.blob_algorithm,
                    blob_digest: decode_hex_32(&self.blob_digest_hex)
                        .context("decode Review visual blob digest")?,
                    blob_byte_len: self.blob_byte_len,
                    codec,
                    byte_order,
                    dimensions: ImageDimensions {
                        width: self.width,
                        height: self.height,
                    },
                    bits_per_channel: self.bits_per_channel,
                    channels: self.channels,
                    created_at_ms: self.created_at_ms,
                },
            },
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

fn photo_flag(flag: ffi::FfiDecisionFlag) -> AnyResult<PhotoFlag> {
    match flag {
        ffi::FfiDecisionFlag::Unflagged => Ok(PhotoFlag::Unflagged),
        ffi::FfiDecisionFlag::Picked => Ok(PhotoFlag::Picked),
        ffi::FfiDecisionFlag::Rejected => Ok(PhotoFlag::Rejected),
        _ => bail!("unsupported Review decision flag"),
    }
}

pub(crate) const fn ffi_decision_flag(flag: PhotoFlag) -> ffi::FfiDecisionFlag {
    match flag {
        PhotoFlag::Unflagged => ffi::FfiDecisionFlag::Unflagged,
        PhotoFlag::Picked => ffi::FfiDecisionFlag::Picked,
        PhotoFlag::Rejected => ffi::FfiDecisionFlag::Rejected,
    }
}

fn ffi_photo_decision_state(
    photo_id: PhotoId,
    state: PhotoDecisionState,
) -> ffi::FfiPhotoDecisionState {
    ffi::FfiPhotoDecisionState {
        photo_id: photo_id.to_string(),
        head_sequence: state.head_sequence,
        flag: ffi_decision_flag(state.flag),
        rating: state.rating,
    }
}

fn ffi_photo_decision_receipt(event: PhotoDecisionEvent) -> ffi::FfiReviewDecisionMutationReceipt {
    ffi::FfiReviewDecisionMutationReceipt {
        event_id: event.event_id,
        sequence: event.sequence,
        photo_id: event.photo_id.to_string(),
        occurred_at_unix_ms: event.occurred_at_unix_ms,
        before_head_sequence: event.before_head_sequence,
        before_flag: ffi_decision_flag(event.before_flag),
        before_rating: event.before_rating,
        after_flag: ffi_decision_flag(event.after_flag),
        after_rating: event.after_rating,
    }
}

fn new_visual_signing_key() -> [u8; 32] {
    let first = Uuid::now_v7();
    let second = Uuid::now_v7();
    let mut key = [0_u8; 32];
    key[..16].copy_from_slice(first.as_bytes());
    key[16..].copy_from_slice(second.as_bytes());
    key
}

fn encode_hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len().saturating_mul(2));
    for byte in bytes {
        encoded.push(char::from(DIGITS[usize::from(byte >> 4)]));
        encoded.push(char::from(DIGITS[usize::from(byte & 0x0f)]));
    }
    encoded
}

fn decode_hex(encoded: &str) -> AnyResult<Vec<u8>> {
    if !encoded.len().is_multiple_of(2) || !is_lower_hex(encoded) {
        bail!("hex value must contain an even number of lowercase hexadecimal characters");
    }
    encoded
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| Ok((hex_nibble(pair[0])? << 4) | hex_nibble(pair[1])?))
        .collect()
}

fn decode_hex_32(encoded: &str) -> AnyResult<[u8; 32]> {
    if encoded.len() != 64 {
        bail!("digest must contain exactly 64 hexadecimal characters");
    }
    let bytes = decode_hex(encoded)?;
    bytes
        .try_into()
        .map_err(|_| anyhow!("digest must contain exactly 32 bytes"))
}

fn hex_nibble(byte: u8) -> AnyResult<u8> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        _ => Err(anyhow::Error::msg("invalid lowercase hexadecimal digit")),
    }
}

fn is_lower_hex(value: &str) -> bool {
    value
        .bytes()
        .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn constant_time_eq(left: &[u8], right: &[u8]) -> bool {
    left.len() == right.len()
        && left
            .iter()
            .zip(right)
            .fold(0_u8, |difference, (left, right)| {
                difference | (left ^ right)
            })
            == 0
}

pub(crate) fn parse_cursor(path: &str, representation_id: &str) -> AnyResult<Option<ReviewCursor>> {
    match (path.is_empty(), representation_id.is_empty()) {
        (true, true) => Ok(None),
        (false, false) => Ok(Some(ReviewCursor {
            display_path: path.to_owned(),
            representation_id: representation_id
                .parse()
                .with_context(|| format!("parse Review cursor id {representation_id}"))?,
        })),
        _ => bail!("Review cursor path and representation id must both be present"),
    }
}

const fn role_name(role: CachedArtifactRole) -> &'static str {
    match role {
        CachedArtifactRole::RecipePreview => "recipe",
        CachedArtifactRole::EmbeddedPreview => "embedded",
        CachedArtifactRole::GeneratedProxy => "proxy",
    }
}

pub(crate) fn file_name(display_path: &str) -> String {
    Path::new(display_path)
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or(display_path)
        .to_owned()
}
