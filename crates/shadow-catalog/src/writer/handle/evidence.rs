//! Client adapters for human decision and feedback evidence.

use std::collections::BTreeSet;

use shadow_ai::{
    FeedbackEvent, FeedbackForgetFact, LearningScope, NewFeedbackEvent, NewFeedbackForgetFact,
};
use shadow_domain::{NewPhotoDecisionEvent, PhotoDecisionEvent, PhotoDecisionState, PhotoId};

use crate::{CatalogError, FeedbackPage, PhotoDecisionPage};

use super::super::{
    CatalogHandle,
    protocol::{DecisionMessage, FeedbackMessage, Message},
};

impl CatalogHandle {
    /// Returns one photo's current authoritative culling/rating decision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable, the photo is
    /// absent, or its current pointer is invalid.
    pub fn photo_decision_state(
        &self,
        photo_id: PhotoId,
    ) -> Result<PhotoDecisionState, CatalogError> {
        self.request(|response| Message::Decision(DecisionMessage::State(photo_id, response)))
    }

    /// Appends one decision event and advances the current pointer atomically.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for invalid input, a stale expected head,
    /// unavailable actor, or durable persistence failure.
    pub fn append_photo_decision_event(
        &self,
        request: &NewPhotoDecisionEvent,
    ) -> Result<PhotoDecisionEvent, CatalogError> {
        self.request(|response| {
            Message::Decision(DecisionMessage::Append(Box::new(request.clone()), response))
        })
    }

    /// Reads one bounded ascending page of immutable decisions for a photo.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable, the photo is
    /// absent, the bound is invalid, or history integrity checks fail.
    pub fn photo_decision_events_after(
        &self,
        photo_id: PhotoId,
        after_sequence_exclusive: u64,
        limit: usize,
    ) -> Result<PhotoDecisionPage, CatalogError> {
        self.request(|response| {
            Message::Decision(DecisionMessage::EventsAfter(
                photo_id,
                after_sequence_exclusive,
                limit,
                response,
            ))
        })
    }

    /// Appends one validated human-feedback event and returns its assigned sequence.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when validation, referenced-photo checks, or
    /// durable persistence fails.
    pub fn append_feedback_event(
        &self,
        request: &NewFeedbackEvent,
    ) -> Result<FeedbackEvent, CatalogError> {
        self.request(|response| {
            Message::Feedback(FeedbackMessage::AppendEvent(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Reads one bounded, ascending page after an exclusive sequence cursor.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid page bound, unavailable actor,
    /// or persisted integrity failure.
    pub fn feedback_events_after(
        &self,
        scope: &LearningScope,
        after_sequence_exclusive: u64,
        limit: usize,
    ) -> Result<FeedbackPage, CatalogError> {
        self.request(|response| {
            Message::Feedback(FeedbackMessage::EventsAfter(
                scope.clone(),
                after_sequence_exclusive,
                limit,
                response,
            ))
        })
    }

    /// Appends a non-destructive forget fact for one existing feedback event.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when validation or persistence fails.
    pub fn append_feedback_forget_fact(
        &self,
        request: &NewFeedbackForgetFact,
    ) -> Result<FeedbackForgetFact, CatalogError> {
        self.request(|response| {
            Message::Feedback(FeedbackMessage::AppendForgetFact(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Returns every forgotten source event id in exactly one learning scope.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the query fails.
    pub fn forgotten_feedback_event_ids(
        &self,
        scope: &LearningScope,
    ) -> Result<BTreeSet<String>, CatalogError> {
        self.request(|response| {
            Message::Feedback(FeedbackMessage::ForgottenEventIds(scope.clone(), response))
        })
    }
}
