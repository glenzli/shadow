//! Shared Grade Node publication and batch-application contracts.

use shadow_domain::{EntityId, LayerId};

use crate::{
    ffi, open_desktop_session,
    recipe_v1::new_basic_grade_node,
    tests::fixtures::{edit_session::test_edit_session, grade_stack::assert_close},
};

#[test]
fn shared_grade_node_heads_are_named_versioned_and_renderable() {
    let root = std::env::temp_dir().join(format!(
        "shadow-shared-grade-node-{}-{}",
        std::process::id(),
        LayerId::new_v7()
    ));
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open desktop session");
    let mut local = new_basic_grade_node("Portrait").expect("local Grade Node");
    local.basic.exposure_stops = 0.35;

    let first = session
        .publish_shared_grade_node("Portrait foundation", &local)
        .expect("publish shared Grade Node");
    assert_eq!(first.revision_number, 1);
    assert_eq!(first.label, "Portrait foundation");
    assert_eq!(first.grade_node.shared_layer_id, first.layer_id);
    assert_eq!(first.grade_node.shared_revision_id, first.revision_id);
    assert_close(first.grade_node.basic.exposure_stops, 0.35);

    let mut update = first.grade_node.clone();
    update.basic.exposure_stops = 0.8;
    let second = session
        .publish_shared_grade_node("Portrait foundation", &update)
        .expect("publish second shared revision");
    assert_eq!(second.layer_id, first.layer_id);
    assert_ne!(second.revision_id, first.revision_id);
    assert_eq!(second.revision_number, 2);
    assert_close(second.grade_node.basic.exposure_stops, 0.8);

    let listed = session.shared_grade_nodes().expect("list shared heads");
    assert_eq!(listed.len(), 1);
    assert_eq!(listed[0].revision_id, second.revision_id);
    assert_eq!(listed[0].grade_node.shared_layer_id, first.layer_id);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove shared Grade Node fixture");
}

#[test]
fn batch_application_links_latest_shared_revision_and_is_idempotent() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let mut local = new_basic_grade_node("Shared contrast").expect("local Grade Node");
    local.basic.contrast_factor = 1.12;
    let shared = session
        .publish_shared_grade_node("Shared contrast", &local)
        .expect("publish shared Grade Node");
    let target = || ffi::FfiBatchPhotoTarget {
        photo_id: photo_id.clone(),
        source_path: source_path.clone(),
    };

    let first = session
        .apply_shared_grade_node_to_photos(&shared.layer_id, vec![target()])
        .expect("apply shared Grade Node");
    assert_eq!(first.requested, 1);
    assert_eq!(first.updated, 1);
    assert_eq!(first.unchanged, 0);
    assert_eq!(first.failed, 0);
    let state = session
        .photo_edit_state(&photo_id, &source_path)
        .expect("read linked edit");
    assert_eq!(state.settings.grade_nodes.len(), 2);
    assert_eq!(
        state.settings.grade_nodes[1].shared_layer_id,
        shared.layer_id
    );
    assert_eq!(
        state.settings.grade_nodes[1].shared_revision_id,
        shared.revision_id
    );

    let second = session
        .apply_shared_grade_node_to_photos(&shared.layer_id, vec![target()])
        .expect("reapply shared Grade Node");
    assert_eq!(second.updated, 0);
    assert_eq!(second.unchanged, 1);
    assert_eq!(second.failed, 0);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove batch Grade Node fixture");
}
