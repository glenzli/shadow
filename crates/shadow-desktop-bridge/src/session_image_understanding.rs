//! Desktop projection for durable Qwen image understanding and explicit review.

use std::path::Path;

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_ai::{
    ClassificationReviewCategory, ClassificationReviewDisposition, InferRuntimeClient,
};
use shadow_core::{
    AdvancedClassificationReviewRequest, ClassificationReviewProposal,
    ClassificationReviewProposalDisposition, ImageUnderstandingKeywordAcceptance,
    ImageUnderstandingProposal, ImageUnderstandingProposalDisposition, ImageUnderstandingRequest,
    ImageUnderstandingRunSnapshot, ImageUnderstandingRunStatus, ImageUnderstandingScanPolicy,
    ImageUnderstandingScanScope, accept_advanced_classification_review,
    advanced_classification_review, apply_image_understanding_keywords,
    dismiss_advanced_classification_review, image_understanding_proposal,
    image_understanding_snapshot, pause_image_understanding, process_image_understanding_batch,
    review_smart_classification_with_model,
};
use shadow_domain::{PhotoId, RepresentationId};

use super::{DesktopSession, ffi};

impl DesktopSession {
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn process_image_understanding_batch(
        &self,
        infer_base_url: &str,
        credential_file: &str,
        scan_scope: &str,
        minimum_rating: u8,
        generation: &str,
        start_new: bool,
        auto_apply_keywords: bool,
    ) -> AnyResult<ffi::FfiImageUnderstandingBatch> {
        let provider = local_provider(infer_base_url, credential_file)?;
        let scope = scan_scope_from_wire(scan_scope)?;
        let minimum_rating = matches!(
            scope,
            ImageUnderstandingScanScope::MinimumRating
                | ImageUnderstandingScanScope::LikedOrMinimumRating
        )
        .then_some(minimum_rating);
        let policy = ImageUnderstandingScanPolicy::new(scope, minimum_rating, 4)
            .context("validate photo-understanding scan policy")?;
        let batch = process_image_understanding_batch(
            &self.catalog,
            &self.cache_root,
            &provider,
            policy,
            &ImageUnderstandingRequest {
                generation: (!generation.is_empty()).then(|| generation.into()),
                start_new,
                auto_apply_keywords,
            },
        )
        .map_err(|error| anyhow!("analyze eligible Library visuals: {error}"))?;
        Ok(ffi::FfiImageUnderstandingBatch {
            generation: batch.snapshot.generation,
            status: run_status(batch.snapshot.status).into(),
            processed_photos: batch.snapshot.processed_photos,
            total_photos: batch.snapshot.total_photos,
            analyzed_photos: bounded_u32(batch.analyzed_photos, "analyzed photo count")?,
            reused_photos: bounded_u32(batch.reused_photos, "reused photo count")?,
            skipped_photos: bounded_u32(batch.skipped_photos, "skipped photo count")?,
        })
    }

    pub(crate) fn image_understanding_snapshot(
        &self,
    ) -> AnyResult<ffi::FfiImageUnderstandingSnapshot> {
        image_understanding_snapshot(&self.cache_root)
            .map_err(|error| anyhow!("load photo-understanding checkpoint: {error}"))
            .map(snapshot_to_ffi)
    }

    pub(crate) fn pause_image_understanding(
        &self,
        generation: &str,
    ) -> AnyResult<ffi::FfiImageUnderstandingSnapshot> {
        pause_image_understanding(&self.cache_root, generation)
            .map_err(|error| anyhow!("pause photo understanding: {error}"))
            .map(|snapshot| snapshot_to_ffi(Some(snapshot)))
    }

    pub(crate) fn image_understanding_proposal(
        &self,
        photo_id: &str,
        representation_id: &str,
    ) -> AnyResult<ffi::FfiImageUnderstandingProposal> {
        let (photo_id, representation_id) = parse_photo_target(photo_id, representation_id)?;
        image_understanding_proposal(&self.cache_root, photo_id, representation_id)
            .map_err(|error| anyhow!("load photo-understanding proposal: {error}"))
            .map(proposal_to_ffi)
    }

    pub(crate) fn apply_image_understanding_keywords(
        &self,
        photo_id: &str,
        representation_id: &str,
        source_revision: &str,
    ) -> AnyResult<()> {
        let (photo_id, representation_id) = parse_photo_target(photo_id, representation_id)?;
        apply_image_understanding_keywords(
            &self.catalog,
            &self.cache_root,
            photo_id,
            representation_id,
            source_revision,
            ImageUnderstandingKeywordAcceptance::User,
        )
        .map_err(|error| anyhow!("accept photo keyword suggestions: {error}"))?;
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn review_smart_classification_with_model(
        &self,
        infer_base_url: &str,
        credential_file: &str,
        photo_id: &str,
        representation_id: &str,
        taxonomy_revision: &str,
        categories: Vec<ffi::FfiClassificationReviewCategory>,
    ) -> AnyResult<ffi::FfiClassificationReviewProposal> {
        let provider = local_provider(infer_base_url, credential_file)?;
        let (photo_id, representation_id) = parse_photo_target(photo_id, representation_id)?;
        let categories = categories
            .into_iter()
            .map(|category| ClassificationReviewCategory {
                id: category.id,
                name: category.name,
                description: (!category.description.is_empty()).then_some(category.description),
            })
            .collect::<Vec<_>>();
        review_smart_classification_with_model(
            &self.catalog,
            &self.cache_root,
            &provider,
            &AdvancedClassificationReviewRequest {
                photo_id,
                representation_id,
                taxonomy_revision,
                categories: &categories,
            },
        )
        .map_err(|error| anyhow!("review smart classification with local model: {error}"))
        .map(|proposal| review_to_ffi(Some(proposal)))
    }

    pub(crate) fn advanced_classification_review(
        &self,
        photo_id: &str,
        representation_id: &str,
    ) -> AnyResult<ffi::FfiClassificationReviewProposal> {
        let (photo_id, representation_id) = parse_photo_target(photo_id, representation_id)?;
        advanced_classification_review(&self.cache_root, photo_id, representation_id)
            .map_err(|error| anyhow!("load advanced classification proposal: {error}"))
            .map(review_to_ffi)
    }

    pub(crate) fn accept_advanced_classification_review(
        &self,
        photo_id: &str,
        representation_id: &str,
        source_revision: &str,
    ) -> AnyResult<String> {
        let (photo_id, representation_id) = parse_photo_target(photo_id, representation_id)?;
        accept_advanced_classification_review(
            &self.cache_root,
            photo_id,
            representation_id,
            source_revision,
        )
        .map_err(|error| anyhow!("accept advanced classification proposal: {error}"))
    }

    pub(crate) fn dismiss_advanced_classification_review(
        &self,
        photo_id: &str,
        representation_id: &str,
        source_revision: &str,
    ) -> AnyResult<()> {
        let (photo_id, representation_id) = parse_photo_target(photo_id, representation_id)?;
        dismiss_advanced_classification_review(
            &self.cache_root,
            photo_id,
            representation_id,
            source_revision,
        )
        .map_err(|error| anyhow!("dismiss advanced classification proposal: {error}"))
    }
}

fn local_provider(infer_base_url: &str, credential_file: &str) -> AnyResult<InferRuntimeClient> {
    InferRuntimeClient::from_credential_file_with_discovery(
        (!infer_base_url.is_empty()).then_some(infer_base_url),
        Path::new(credential_file),
    )
    .context("configure local image-understanding provider")
}

fn scan_scope_from_wire(value: &str) -> AnyResult<ImageUnderstandingScanScope> {
    match value {
        "all" => Ok(ImageUnderstandingScanScope::All),
        "liked" => Ok(ImageUnderstandingScanScope::Liked),
        "minimum_rating" => Ok(ImageUnderstandingScanScope::MinimumRating),
        "liked_or_minimum_rating" => Ok(ImageUnderstandingScanScope::LikedOrMinimumRating),
        _ => Err(anyhow!("photo-understanding scan scope is invalid")),
    }
}

fn snapshot_to_ffi(
    snapshot: Option<ImageUnderstandingRunSnapshot>,
) -> ffi::FfiImageUnderstandingSnapshot {
    snapshot.map_or_else(
        || ffi::FfiImageUnderstandingSnapshot {
            available: false,
            policy_revision: String::new(),
            generation: String::new(),
            status: "empty".into(),
            processed_photos: 0,
            total_photos: 0,
            updated_at_ms: 0,
        },
        |snapshot| ffi::FfiImageUnderstandingSnapshot {
            available: true,
            policy_revision: snapshot.policy_revision,
            generation: snapshot.generation,
            status: run_status(snapshot.status).into(),
            processed_photos: snapshot.processed_photos,
            total_photos: snapshot.total_photos,
            updated_at_ms: snapshot.updated_at_ms,
        },
    )
}

fn proposal_to_ffi(
    proposal: Option<ImageUnderstandingProposal>,
) -> ffi::FfiImageUnderstandingProposal {
    proposal.map_or_else(
        || ffi::FfiImageUnderstandingProposal {
            available: false,
            photo_id: String::new(),
            representation_id: String::new(),
            source_revision: String::new(),
            description: String::new(),
            language: String::new(),
            keywords: Vec::new(),
            disposition: String::new(),
            model_profile: String::new(),
            model_build: String::new(),
        },
        |proposal| ffi::FfiImageUnderstandingProposal {
            available: true,
            photo_id: proposal.photo_id.to_string(),
            representation_id: proposal.representation_id.to_string(),
            source_revision: proposal.source_revision,
            description: proposal.analysis.short_caption.text,
            language: proposal.analysis.short_caption.language_tag,
            keywords: proposal
                .analysis
                .suggestions
                .into_iter()
                .map(|suggestion| suggestion.display_label)
                .collect(),
            disposition: match proposal.disposition {
                ImageUnderstandingProposalDisposition::Suggested => "suggested",
                ImageUnderstandingProposalDisposition::AutoApplied => "auto_applied",
                ImageUnderstandingProposalDisposition::Accepted => "accepted",
                ImageUnderstandingProposalDisposition::Dismissed => "dismissed",
            }
            .into(),
            model_profile: proposal.provenance.model_profile,
            model_build: proposal.provenance.model_build,
        },
    )
}

fn review_to_ffi(
    proposal: Option<ClassificationReviewProposal>,
) -> ffi::FfiClassificationReviewProposal {
    proposal.map_or_else(
        || ffi::FfiClassificationReviewProposal {
            available: false,
            photo_id: String::new(),
            representation_id: String::new(),
            source_revision: String::new(),
            taxonomy_revision: String::new(),
            disposition: String::new(),
            category_id: String::new(),
            proposal_status: String::new(),
            model_profile: String::new(),
            model_build: String::new(),
        },
        |proposal| ffi::FfiClassificationReviewProposal {
            available: true,
            photo_id: proposal.photo_id.to_string(),
            representation_id: proposal.representation_id.to_string(),
            source_revision: proposal.source_revision,
            taxonomy_revision: proposal.taxonomy_revision,
            disposition: match proposal.suggestion.disposition {
                ClassificationReviewDisposition::Matched => "matched",
                ClassificationReviewDisposition::None => "none",
                ClassificationReviewDisposition::Uncertain => "uncertain",
            }
            .into(),
            category_id: proposal.suggestion.category_id.unwrap_or_default(),
            proposal_status: match proposal.disposition {
                ClassificationReviewProposalDisposition::Suggested => "suggested",
                ClassificationReviewProposalDisposition::Accepted => "accepted",
                ClassificationReviewProposalDisposition::Dismissed => "dismissed",
            }
            .into(),
            model_profile: proposal.provenance.model_profile,
            model_build: proposal.provenance.model_build,
        },
    )
}

fn parse_photo_target(
    photo_id: &str,
    representation_id: &str,
) -> AnyResult<(PhotoId, RepresentationId)> {
    Ok((
        photo_id.parse().context("parse photo identity")?,
        representation_id
            .parse()
            .context("parse representation identity")?,
    ))
}

const fn run_status(status: ImageUnderstandingRunStatus) -> &'static str {
    match status {
        ImageUnderstandingRunStatus::Running => "running",
        ImageUnderstandingRunStatus::Paused => "paused",
        ImageUnderstandingRunStatus::Complete => "complete",
        ImageUnderstandingRunStatus::Failed => "failed",
    }
}

fn bounded_u32(value: usize, field: &'static str) -> AnyResult<u32> {
    u32::try_from(value).with_context(|| format!("{field} exceeds the desktop ABI bound"))
}
