//! Desktop-session CXX delegations for Review presentation, feedback, and decisions.

use anyhow::Result as AnyResult;

use super::{DesktopSession, ffi};

impl DesktopSession {
    pub(crate) fn review_page(
        &self,
        cursor_path: &str,
        cursor_representation_id: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiReviewPage> {
        self.review
            .review_page(cursor_path, cursor_representation_id, limit)
    }

    pub(crate) fn load_review_visual(&self, ticket: &str) -> AnyResult<ffi::FfiVisualPayload> {
        self.review.load_visual(ticket)
    }

    pub(crate) fn prepare_review_comparison(
        &self,
        left_grid_handle: &str,
        right_grid_handle: &str,
    ) -> AnyResult<ffi::FfiReviewComparisonPresentation> {
        self.review
            .prepare_comparison(left_grid_handle, right_grid_handle)
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn record_review_visual_frame(
        &self,
        request_ticket: &str,
        decoder_version: &str,
        requested_width: u32,
        requested_height: u32,
        decoded_width: u32,
        decoded_height: u32,
        pixel_hash_hex: &str,
    ) -> AnyResult<()> {
        self.review.record_visual_frame(
            request_ticket,
            decoder_version,
            requested_width,
            requested_height,
            decoded_width,
            decoded_height,
            pixel_hash_hex,
        )
    }

    pub(crate) fn confirm_review_comparison_ready(
        &self,
        presentation_id: &str,
        left_request_ticket: &str,
        right_request_ticket: &str,
    ) -> AnyResult<()> {
        self.review.confirm_comparison_ready(
            presentation_id,
            left_request_ticket,
            right_request_ticket,
        )
    }

    pub(crate) fn cancel_review_comparison(&self, presentation_id: &str) -> AnyResult<()> {
        self.review.cancel_comparison(presentation_id)
    }

    pub(crate) fn record_review_comparison(
        &self,
        presentation_id: &str,
        outcome: ffi::FfiPairwiseOutcome,
    ) -> AnyResult<ffi::FfiFeedbackReceipt> {
        self.review.record_comparison(presentation_id, outcome)
    }

    pub(crate) fn forget_review_feedback(
        &self,
        event_id: &str,
    ) -> AnyResult<ffi::FfiForgetReceipt> {
        self.review.forget_feedback(event_id)
    }

    pub(crate) fn review_photo_decision_state(
        &self,
        photo_id: &str,
    ) -> AnyResult<ffi::FfiPhotoDecisionState> {
        self.review.photo_decision_state(photo_id)
    }

    pub(crate) fn set_review_photo_decision(
        &self,
        photo_id: &str,
        expected_head_sequence: u64,
        flag: ffi::FfiDecisionFlag,
        rating: u8,
    ) -> AnyResult<ffi::FfiReviewDecisionMutationReceipt> {
        self.review
            .set_photo_decision(photo_id, expected_head_sequence, flag, rating)
    }
}
