use shadow_ai::LearningScope;
use shadow_domain::{EntityId, PhotoId};

use super::{
    super::*,
    event_fixtures::{exported_event, register_photo},
};

#[test]
fn catalog_assigns_global_sequence_and_pages_each_scope_after_reopen() {
    let root = std::env::temp_dir().join(format!("shadow-feedback-{}", PhotoId::new_v7()));
    std::fs::create_dir_all(&root).expect("create feedback fixture");
    let path = root.join("catalog.sqlite");
    let (first_photo, second_photo);
    {
        let mut catalog = Catalog::open(&path).expect("open catalog");
        first_photo = register_photo(&mut catalog, 1);
        second_photo = register_photo(&mut catalog, 2);
        let first = catalog
            .append_feedback_event(&exported_event(
                "event-global-1",
                LearningScope::Global,
                first_photo,
            ))
            .expect("append first event");
        let project = catalog
            .append_feedback_event(&exported_event(
                "event-project-1",
                LearningScope::Project {
                    project_id: "wedding".into(),
                },
                second_photo,
            ))
            .expect("append project event");
        let second = catalog
            .append_feedback_event(&exported_event(
                "event-global-2",
                LearningScope::Global,
                second_photo,
            ))
            .expect("append second event");
        assert_eq!(
            (first.sequence, project.sequence, second.sequence),
            (1, 2, 3)
        );
    }

    {
        let catalog = Catalog::open(&path).expect("reopen catalog");
        let first_page = catalog
            .feedback_events_after(&LearningScope::Global, 0, 1)
            .expect("read first global page");
        assert!(first_page.has_more);
        assert_eq!(first_page.events[0].event_id, "event-global-1");
        let second_page = catalog
            .feedback_events_after(&LearningScope::Global, first_page.events[0].sequence, 1)
            .expect("read second global page");
        assert!(!second_page.has_more);
        assert_eq!(second_page.events[0].event_id, "event-global-2");

        let project_page = catalog
            .feedback_events_after(
                &LearningScope::Project {
                    project_id: "wedding".into(),
                },
                0,
                10,
            )
            .expect("read project page");
        assert_eq!(project_page.events.len(), 1);
        assert_eq!(project_page.events[0].event_id, "event-project-1");
    }
    std::fs::remove_dir_all(root).expect("remove feedback fixture");
}

#[test]
fn duplicate_event_id_is_rejected_without_overwriting_source_fact() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let photo = register_photo(&mut catalog, 1);
    let original = exported_event("event-1", LearningScope::Global, photo);
    catalog
        .append_feedback_event(&original)
        .expect("append original");
    let mut replacement = original;
    replacement.occurred_at_unix_ms += 1;
    let error = catalog
        .append_feedback_event(&replacement)
        .expect_err("duplicate event must fail");
    assert!(matches!(error, CatalogError::FeedbackEventAlreadyExists(id) if id == "event-1"));
    let stored = catalog
        .feedback_events_after(&LearningScope::Global, 0, 10)
        .expect("read original");
    assert_eq!(stored.events.len(), 1);
    assert_eq!(stored.events[0].occurred_at_unix_ms, 1_700_000_001_000);
}

#[test]
fn page_limit_is_strictly_bounded() {
    let catalog = Catalog::open_in_memory().expect("open catalog");
    assert!(matches!(
        catalog.feedback_events_after(&LearningScope::Global, 0, 0),
        Err(CatalogError::InvalidFeedbackPageLimit { .. })
    ));
    assert!(matches!(
        catalog.feedback_events_after(&LearningScope::Global, 0, MAX_FEEDBACK_PAGE_SIZE + 1,),
        Err(CatalogError::InvalidFeedbackPageLimit { .. })
    ));
}
