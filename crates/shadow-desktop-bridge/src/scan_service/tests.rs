use super::*;
use crate::open_desktop_session;
use shadow_domain::{EntityId, RepresentationId};

#[test]
fn viewport_hints_are_validated_bounded_and_fenced_to_active_scan() {
    let root = std::env::temp_dir().join(format!(
        "shadow-scan-priority-{}",
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).unwrap();
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().unwrap(),
        root.join("cache").to_str().unwrap(),
    )
    .unwrap();
    let id = RepresentationId::new_v7().to_string();
    assert!(
        !session
            .prioritize_visible_refinements(1, &id, std::slice::from_ref(&id))
            .unwrap()
    );
    session.begin_folder_scan(1).unwrap();
    assert!(
        session
            .prioritize_visible_refinements(1, &id, std::slice::from_ref(&id))
            .unwrap()
    );
    assert!(
        !session
            .prioritize_visible_refinements(2, &id, std::slice::from_ref(&id))
            .unwrap()
    );
    assert!(
        session
            .prioritize_visible_refinements(1, "invalid", &[])
            .is_err()
    );
    assert!(
        session
            .prioritize_visible_refinements(1, "", &["invalid".into()])
            .is_err()
    );
    assert!(
        session
            .prioritize_visible_refinements(1, "", &vec![id; MAX_VISIBLE_REFINEMENTS + 1])
            .is_err()
    );
    assert!(session.prioritize_visible_refinements(1, "", &[]).unwrap());
    session.cancel_folder_scan(1).unwrap();
    assert!(!session.prioritize_visible_refinements(1, "", &[]).unwrap());
    drop(session);
    std::fs::remove_dir_all(root).unwrap();
}
