//! Actor-side execution for human decision and feedback messages.

use crate::Catalog;

use super::super::protocol::{DecisionMessage, FeedbackMessage};

pub(super) fn run_decision_message(catalog: &mut Catalog, message: DecisionMessage) {
    match message {
        DecisionMessage::State(photo_id, response) => {
            let _ = response.send(catalog.photo_decision_state(photo_id));
        }
        DecisionMessage::Append(request, response) => {
            let _ = response.send(catalog.append_photo_decision_event(request.as_ref()));
        }
        DecisionMessage::EventsAfter(photo_id, after_sequence, limit, response) => {
            let _ =
                response.send(catalog.photo_decision_events_after(photo_id, after_sequence, limit));
        }
    }
}

pub(super) fn run_feedback_message(catalog: &mut Catalog, message: FeedbackMessage) {
    match message {
        FeedbackMessage::AppendEvent(request, response) => {
            let _ = response.send(catalog.append_feedback_event(request.as_ref()));
        }
        FeedbackMessage::EventsAfter(scope, after_sequence, limit, response) => {
            let _ = response.send(catalog.feedback_events_after(&scope, after_sequence, limit));
        }
        FeedbackMessage::AppendForgetFact(request, response) => {
            let _ = response.send(catalog.append_feedback_forget_fact(request.as_ref()));
        }
        FeedbackMessage::ForgottenEventIds(scope, response) => {
            let _ = response.send(catalog.forgotten_feedback_event_ids(&scope));
        }
    }
}
