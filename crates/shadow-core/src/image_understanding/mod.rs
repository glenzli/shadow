//! Resumable structured image-understanding policy and workflow.
//!
//! This owner is deliberately separate from `SigLIP` search and smart-category
//! classification. It selects the bounded Library population eligible for a
//! heavier structured model, while explicit single-photo review remains able
//! to bypass the background selection policy. Provider execution and durable
//! proposal storage are added behind this contract rather than leaking model
//! names or desktop settings into Catalog queries.

use shadow_catalog::LibraryPhotoFilter;
use thiserror::Error;

mod classification_review;
mod store;
mod workflow;

pub use classification_review::{
    AdvancedClassificationReviewError, AdvancedClassificationReviewRequest,
    accept_advanced_classification_review, advanced_classification_review,
    dismiss_advanced_classification_review, review_smart_classification_with_model,
};
pub use store::{
    ClassificationReviewProposal, ClassificationReviewProposalDisposition,
    ImageUnderstandingProposal, ImageUnderstandingProposalDisposition,
    ImageUnderstandingRunSnapshot, ImageUnderstandingRunStatus, ImageUnderstandingStoreError,
};
pub use workflow::{
    ImageUnderstandingBatch, ImageUnderstandingKeywordAcceptance,
    ImageUnderstandingKeywordApplication, ImageUnderstandingRequest,
    ImageUnderstandingWorkflowError, apply_image_understanding_keywords,
    image_understanding_proposal, image_understanding_snapshot, pause_image_understanding,
    process_image_understanding_batch,
};

pub const IMAGE_UNDERSTANDING_SCAN_POLICY_VERSION: u32 = 1;
pub const DEFAULT_IMAGE_UNDERSTANDING_BATCH_SIZE: usize = 4;
pub const MAX_IMAGE_UNDERSTANDING_BATCH_SIZE: usize = 16;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ImageUnderstandingScanScope {
    All,
    Liked,
    MinimumRating,
    LikedOrMinimumRating,
}

impl ImageUnderstandingScanScope {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::All => "all",
            Self::Liked => "liked",
            Self::MinimumRating => "minimum_rating",
            Self::LikedOrMinimumRating => "liked_or_minimum_rating",
        }
    }
}

/// Stable selection and checkpoint policy for heavyweight background image
/// understanding.
///
/// `LikedOrMinimumRating` intentionally expands into two ordinary Catalog
/// filters. The queue owner must de-duplicate logical photos across those
/// pages before execution. Other scopes require only one bounded keyset scan.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct ImageUnderstandingScanPolicy {
    scope: ImageUnderstandingScanScope,
    minimum_rating: Option<u8>,
    batch_size: usize,
}

impl Default for ImageUnderstandingScanPolicy {
    fn default() -> Self {
        Self {
            scope: ImageUnderstandingScanScope::Liked,
            minimum_rating: None,
            batch_size: DEFAULT_IMAGE_UNDERSTANDING_BATCH_SIZE,
        }
    }
}

impl ImageUnderstandingScanPolicy {
    /// Creates one normalized background selection policy.
    ///
    /// # Errors
    ///
    /// Rating scopes require a one-through-five threshold. Non-rating scopes
    /// reject a threshold so ignored UI state cannot create a false checkpoint
    /// revision. Batches remain deliberately small for heavyweight local VLMs.
    pub fn new(
        scope: ImageUnderstandingScanScope,
        minimum_rating: Option<u8>,
        batch_size: usize,
    ) -> Result<Self, ImageUnderstandingPolicyError> {
        if batch_size == 0 || batch_size > MAX_IMAGE_UNDERSTANDING_BATCH_SIZE {
            return Err(ImageUnderstandingPolicyError::InvalidBatchSize(batch_size));
        }
        match scope {
            ImageUnderstandingScanScope::All | ImageUnderstandingScanScope::Liked => {
                if minimum_rating.is_some() {
                    return Err(ImageUnderstandingPolicyError::UnexpectedRatingThreshold);
                }
            }
            ImageUnderstandingScanScope::MinimumRating
            | ImageUnderstandingScanScope::LikedOrMinimumRating => {
                if !matches!(minimum_rating, Some(1..=5)) {
                    return Err(ImageUnderstandingPolicyError::InvalidRatingThreshold);
                }
            }
        }
        Ok(Self {
            scope,
            minimum_rating,
            batch_size,
        })
    }

    pub const fn scope(self) -> ImageUnderstandingScanScope {
        self.scope
    }

    pub const fn minimum_rating(self) -> Option<u8> {
        self.minimum_rating
    }

    pub const fn batch_size(self) -> usize {
        self.batch_size
    }

    /// Evaluates one already projected photo without performing a Catalog
    /// query. This is also the canonical de-duplication predicate for resumed
    /// queues whose review state changed after the previous checkpoint.
    pub fn includes(self, liked: bool, rating: u8) -> bool {
        match self.scope {
            ImageUnderstandingScanScope::All => true,
            ImageUnderstandingScanScope::Liked => liked,
            ImageUnderstandingScanScope::MinimumRating => {
                rating >= self.minimum_rating.unwrap_or(5)
            }
            ImageUnderstandingScanScope::LikedOrMinimumRating => {
                liked || rating >= self.minimum_rating.unwrap_or(5)
            }
        }
    }

    /// Returns the ordinary bounded Library filters that cover this policy.
    /// The caller retains keyset cursors per returned filter and de-duplicates
    /// photos when two branches are present.
    pub fn catalog_filters(self) -> Vec<LibraryPhotoFilter> {
        match self.scope {
            ImageUnderstandingScanScope::All => vec![LibraryPhotoFilter::default()],
            ImageUnderstandingScanScope::Liked => vec![LibraryPhotoFilter {
                liked: Some(true),
                ..LibraryPhotoFilter::default()
            }],
            ImageUnderstandingScanScope::MinimumRating => vec![LibraryPhotoFilter {
                minimum_rating: self.minimum_rating,
                ..LibraryPhotoFilter::default()
            }],
            ImageUnderstandingScanScope::LikedOrMinimumRating => vec![
                LibraryPhotoFilter {
                    liked: Some(true),
                    ..LibraryPhotoFilter::default()
                },
                LibraryPhotoFilter {
                    minimum_rating: self.minimum_rating,
                    ..LibraryPhotoFilter::default()
                },
            ],
        }
    }

    /// Stable path-free identity used by resumable queue checkpoints.
    pub fn revision(self) -> String {
        format!(
            "shadow.image-understanding-scan:v{}:{}:rating-{}:batch-{}",
            IMAGE_UNDERSTANDING_SCAN_POLICY_VERSION,
            self.scope.as_str(),
            self.minimum_rating
                .map_or_else(|| "none".to_owned(), |value| value.to_string()),
            self.batch_size
        )
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Error)]
pub enum ImageUnderstandingPolicyError {
    #[error("image-understanding batch size {0} is outside the supported bound")]
    InvalidBatchSize(usize),
    #[error("image-understanding rating scope requires a threshold from one through five")]
    InvalidRatingThreshold,
    #[error("image-understanding non-rating scope must not carry a rating threshold")]
    UnexpectedRatingThreshold,
}

#[cfg(test)]
mod tests;
