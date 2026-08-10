//! Explicit heavyweight review for one uncertain smart-classification sample.
//!
//! The model may only choose from the supplied category set. Its result stays
//! a durable assistant proposal until a user accepts it; only that acceptance
//! enters the existing personalization feedback path.

use shadow_ai::{
    ClassificationReviewCategory, ClassificationReviewDisposition, ClassificationReviewProvider,
    ClassificationReviewRequest, ImageUnderstandingQuality, InferRuntimeClientError,
    SemanticRequestPriority,
};
use shadow_catalog::{CatalogError, CatalogHandle};
use shadow_domain::{PhotoId, PreviewCodec, RepresentationId};
use thiserror::Error;

use crate::{
    CachedArtifactLoadError, CachedArtifactLoader, SmartCategoryFeedbackDecision,
    SmartClassificationError, set_smart_category_feedback,
};

use super::store::{
    ClassificationReviewProposal, ClassificationReviewProposalDisposition, ImageUnderstandingStore,
    StoreClassificationReview,
};
use super::{ImageUnderstandingStoreError, workflow::source_revision};

#[derive(Debug, Clone, Copy)]
pub struct AdvancedClassificationReviewRequest<'a> {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub taxonomy_revision: &'a str,
    pub categories: &'a [ClassificationReviewCategory],
}

/// Runs one explicit, general-quality closed-set review and durably stores its
/// assistant-only proposal.
///
/// # Errors
///
/// Returns missing/stale visual, provider-contract, Catalog, cache, or sidecar
/// errors. No smart-category feedback is written by this operation.
pub fn review_smart_classification_with_model(
    catalog: &CatalogHandle,
    cache_root: impl AsRef<std::path::Path>,
    provider: &impl ClassificationReviewProvider,
    request: &AdvancedClassificationReviewRequest<'_>,
) -> Result<ClassificationReviewProposal, AdvancedClassificationReviewError> {
    let cache_root = cache_root.as_ref();
    let record = catalog
        .preferred_cached_artifact(request.representation_id)?
        .ok_or(AdvancedClassificationReviewError::VisualUnavailable)?;
    if record.artifact.codec != PreviewCodec::Jpeg {
        return Err(AdvancedClassificationReviewError::VisualUnavailable);
    }
    let expected_revision = source_revision(request.photo_id, &record);
    let loader = CachedArtifactLoader::open(catalog.clone(), cache_root)?;
    let bytes = loader.load_bytes(&record)?;
    let evidence = provider.review_classification(&ClassificationReviewRequest {
        image: &bytes,
        media_type: "image/jpeg",
        source_revision: &expected_revision,
        taxonomy_revision: request.taxonomy_revision,
        categories: request.categories,
        quality: ImageUnderstandingQuality::General,
        priority: SemanticRequestPriority::Interactive,
    })?;
    if evidence.source_revision != expected_revision
        || evidence.taxonomy_revision != request.taxonomy_revision
        || evidence.width != record.artifact.dimensions.width
        || evidence.height != record.artifact.dimensions.height
        || evidence.orientation != "display_pixels_orientation_normalized"
    {
        return Err(AdvancedClassificationReviewError::ProviderGeometryMismatch);
    }
    let current = catalog.preferred_cached_artifact(request.representation_id)?;
    let current_revision = current
        .as_ref()
        .map(|current| source_revision(request.photo_id, current))
        .unwrap_or_default();
    let store = ImageUnderstandingStore::open(cache_root)?;
    store.store_classification_review(&StoreClassificationReview {
        photo_id: request.photo_id,
        representation_id: request.representation_id,
        expected_source_revision: &expected_revision,
        current_source_revision: &current_revision,
        taxonomy_revision: request.taxonomy_revision,
        suggestion: &evidence.suggestion,
        provenance: &evidence.provenance,
    })?;
    store
        .classification_review(request.photo_id, request.representation_id)?
        .ok_or(AdvancedClassificationReviewError::ProposalNotFound)
}

/// Loads the latest exact-representation assistant proposal.
///
/// # Errors
///
/// Returns sidecar filesystem, schema, serialization, or database errors.
pub fn advanced_classification_review(
    cache_root: impl AsRef<std::path::Path>,
    photo_id: PhotoId,
    representation_id: RepresentationId,
) -> Result<Option<ClassificationReviewProposal>, ImageUnderstandingStoreError> {
    ImageUnderstandingStore::open(cache_root)?.classification_review(photo_id, representation_id)
}

/// Accepts a model-matched category as an explicit user decision.
///
/// The feedback write marks smart-category adaptation pending. The assistant
/// proposal is then compare-and-swap marked accepted, preserving its exact
/// Qwen provenance without claiming that the model authored the user choice.
///
/// # Errors
///
/// Rejects stale, unresolved, or non-matched proposals and propagates the
/// existing feedback or sidecar persistence errors.
pub fn accept_advanced_classification_review(
    cache_root: impl AsRef<std::path::Path>,
    photo_id: PhotoId,
    representation_id: RepresentationId,
    expected_source_revision: &str,
) -> Result<String, AdvancedClassificationReviewError> {
    let cache_root = cache_root.as_ref();
    let store = ImageUnderstandingStore::open(cache_root)?;
    let proposal = store
        .classification_review(photo_id, representation_id)?
        .ok_or(AdvancedClassificationReviewError::ProposalNotFound)?;
    if proposal.source_revision != expected_source_revision
        || proposal.disposition != ClassificationReviewProposalDisposition::Suggested
    {
        return Err(AdvancedClassificationReviewError::StaleProposal);
    }
    let (ClassificationReviewDisposition::Matched, Some(category_id)) = (
        proposal.suggestion.disposition,
        proposal.suggestion.category_id,
    ) else {
        return Err(AdvancedClassificationReviewError::NoMatchedCategory);
    };
    set_smart_category_feedback(
        cache_root,
        photo_id,
        representation_id,
        &category_id,
        SmartCategoryFeedbackDecision::Belongs,
    )?;
    if !store.set_classification_review_disposition(
        photo_id,
        representation_id,
        expected_source_revision,
        ClassificationReviewProposalDisposition::Accepted,
    )? {
        return Err(AdvancedClassificationReviewError::StaleProposal);
    }
    Ok(category_id)
}

/// Dismisses one exact model proposal without creating feedback.
///
/// # Errors
///
/// Returns stale-proposal or sidecar persistence errors.
pub fn dismiss_advanced_classification_review(
    cache_root: impl AsRef<std::path::Path>,
    photo_id: PhotoId,
    representation_id: RepresentationId,
    expected_source_revision: &str,
) -> Result<(), AdvancedClassificationReviewError> {
    let store = ImageUnderstandingStore::open(cache_root)?;
    if !store.set_classification_review_disposition(
        photo_id,
        representation_id,
        expected_source_revision,
        ClassificationReviewProposalDisposition::Dismissed,
    )? {
        return Err(AdvancedClassificationReviewError::StaleProposal);
    }
    Ok(())
}

#[derive(Debug, Error)]
pub enum AdvancedClassificationReviewError {
    #[error("the current display visual is unavailable for advanced review")]
    VisualUnavailable,
    #[error("advanced classification provider geometry disagrees with the current display visual")]
    ProviderGeometryMismatch,
    #[error("advanced classification proposal was not found")]
    ProposalNotFound,
    #[error("advanced classification proposal is stale or already resolved")]
    StaleProposal,
    #[error("advanced classification proposal did not select a category")]
    NoMatchedCategory,
    #[error(transparent)]
    Catalog(#[from] CatalogError),
    #[error(transparent)]
    Cache(#[from] CachedArtifactLoadError),
    #[error(transparent)]
    Provider(#[from] InferRuntimeClientError),
    #[error(transparent)]
    Store(#[from] ImageUnderstandingStoreError),
    #[error(transparent)]
    SmartClassification(#[from] SmartClassificationError),
}
