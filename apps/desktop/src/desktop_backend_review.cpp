#include "backend/desktop_backend_private.hpp"
#include "backend/review_projection.hpp"
#include "backend/rust_qt_projection.hpp"

namespace {

using desktop_backend_projection::decision_flag;
using desktop_backend_projection::ffi_decision_flag;
using desktop_backend_projection::ffi_outcome;
using desktop_backend_projection::qbytes;
using desktop_backend_projection::qstring;

} // namespace

BackendReviewVisual DesktopBackend::loadReviewVisual(const QString& ticket) const {
    const auto payload = impl_->session->load_review_visual(ticket.toStdString());
    return {
        .bytes = qbytes(payload.bytes),
        .requires_frame_receipt = payload.requires_frame_receipt,
    };
}

BackendReviewComparisonPresentation DesktopBackend::prepareReviewComparison(
    const QString& left_visual_handle,
    const QString& right_visual_handle
) const {
    const auto presentation = impl_->session->prepare_review_comparison(
        left_visual_handle.toStdString(),
        right_visual_handle.toStdString()
    );
    return {
        .presentation_id = qstring(presentation.presentation_id),
        .left_request_ticket = qstring(presentation.left_request_ticket),
        .right_request_ticket = qstring(presentation.right_request_ticket),
    };
}

void DesktopBackend::reportReviewVisualFrame(
    const QString& ticket,
    const QString& decoder_version,
    const std::uint32_t requested_width,
    const std::uint32_t requested_height,
    const std::uint32_t decoded_width,
    const std::uint32_t decoded_height,
    const QString& pixel_hash_hex
) const {
    impl_->session->record_review_visual_frame(
        ticket.toStdString(),
        decoder_version.toStdString(),
        requested_width,
        requested_height,
        decoded_width,
        decoded_height,
        pixel_hash_hex.toStdString()
    );
}

void DesktopBackend::confirmReviewComparisonReady(
    const QString& presentation_id,
    const QString& left_request_ticket,
    const QString& right_request_ticket
) const {
    impl_->session->confirm_review_comparison_ready(
        presentation_id.toStdString(),
        left_request_ticket.toStdString(),
        right_request_ticket.toStdString()
    );
}

void DesktopBackend::cancelReviewComparison(const QString& presentation_id) const {
    impl_->session->cancel_review_comparison(presentation_id.toStdString());
}

BackendFeedbackReceipt DesktopBackend::recordReviewComparison(
    const QString& presentation_id,
    const BackendPairwiseOutcome outcome
) const {
    const auto receipt = impl_->session->record_review_comparison(
        presentation_id.toStdString(),
        ffi_outcome(outcome)
    );
    return {
        .event_id = qstring(receipt.event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
    };
}

BackendForgetReceipt DesktopBackend::forgetReviewFeedback(
    const QString& event_id
) const {
    const auto receipt = impl_->session->forget_review_feedback(event_id.toStdString());
    return {
        .fact_id = qstring(receipt.fact_id),
        .target_event_id = qstring(receipt.target_event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
    };
}

BackendReviewDecisionState DesktopBackend::reviewPhotoDecisionState(
    const QString& photo_id
) const {
    const auto state = impl_->session->review_photo_decision_state(photo_id.toStdString());
    return {
        .photo_id = qstring(state.photo_id),
        .head_sequence = state.head_sequence,
        .flag = decision_flag(state.flag),
        .rating = state.rating,
    };
}

BackendReviewDecisionMutationReceipt DesktopBackend::setReviewPhotoDecision(
    const QString& photo_id,
    const std::uint64_t expected_head_sequence,
    const BackendReviewDecisionFlag desired_flag,
    const std::uint8_t desired_rating
) const {
    const auto receipt = impl_->session->set_review_photo_decision(
        photo_id.toStdString(),
        expected_head_sequence,
        ffi_decision_flag(desired_flag),
        desired_rating
    );
    const QString returned_photo_id = qstring(receipt.photo_id);
    return {
        .event_id = qstring(receipt.event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
        .before = {
            .photo_id = returned_photo_id,
            .head_sequence = receipt.before_head_sequence,
            .flag = decision_flag(receipt.before_flag),
            .rating = receipt.before_rating,
        },
        .after = {
            .photo_id = returned_photo_id,
            .head_sequence = receipt.sequence,
            .flag = decision_flag(receipt.after_flag),
            .rating = receipt.after_rating,
        },
    };
}
