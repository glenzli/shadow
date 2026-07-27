use super::evidence::{FeedbackAction, FeedbackEvent, LearningScope, PresentationContext};

pub(super) fn event(action: FeedbackAction) -> FeedbackEvent {
    FeedbackEvent {
        event_id: "event-1".into(),
        sequence: 1,
        occurred_at_unix_ms: 123,
        scope: LearningScope::Global,
        presentation: PresentationContext {
            session_id: "review-1".into(),
            group_id: None,
            candidates: vec![],
            active_model: None,
        },
        action,
    }
}
