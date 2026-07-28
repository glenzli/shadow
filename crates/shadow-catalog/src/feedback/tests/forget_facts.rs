use std::collections::BTreeSet;

use shadow_ai::{LearningScope, NewFeedbackForgetFact};

use super::{
    super::*,
    event_fixtures::{exported_event, register_photo},
};

#[test]
fn forgetting_is_append_only_and_never_deletes_the_source_event() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let photo = register_photo(&mut catalog, 1);
    catalog
        .append_feedback_event(&exported_event(
            "event-to-forget",
            LearningScope::Global,
            photo,
        ))
        .expect("append source event");
    let forget = NewFeedbackForgetFact {
        fact_id: "forget-1".into(),
        target_event_id: "event-to-forget".into(),
        occurred_at_unix_ms: 1_700_000_002_000,
        reason: Some("user requested retraining exclusion".into()),
    };
    let stored_forget = catalog
        .append_feedback_forget_fact(&forget)
        .expect("append forget fact");
    assert_eq!(stored_forget.sequence, 1);
    assert!(matches!(
        catalog.append_feedback_forget_fact(&forget),
        Err(CatalogError::FeedbackForgetFactAlreadyExists(id)) if id == "forget-1"
    ));

    let forgotten = catalog
        .forgotten_feedback_event_ids(&LearningScope::Global)
        .expect("read forgotten ids");
    assert_eq!(forgotten, BTreeSet::from(["event-to-forget".into()]));
    assert_eq!(
        catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read preserved source")
            .events
            .len(),
        1
    );
    let delete_error = catalog.connection.execute(
        "DELETE FROM ai_feedback_events WHERE event_id = ?1",
        ["event-to-forget"],
    );
    let update_error = catalog.connection.execute(
        "UPDATE ai_feedback_events SET occurred_at_ms = 0 WHERE event_id = ?1",
        ["event-to-forget"],
    );
    let delete_forget_error = catalog.connection.execute(
        "DELETE FROM ai_feedback_forget_facts WHERE fact_id = ?1",
        ["forget-1"],
    );
    let update_forget_error = catalog.connection.execute(
        "UPDATE ai_feedback_forget_facts SET occurred_at_ms = 0 WHERE fact_id = ?1",
        ["forget-1"],
    );
    assert!(delete_error.is_err());
    assert!(update_error.is_err());
    assert!(delete_forget_error.is_err());
    assert!(update_forget_error.is_err());
}
