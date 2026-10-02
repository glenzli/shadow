//! Simultaneous editor CAS and long-session recovery contracts.

use std::sync::{Arc, Barrier};

use shadow_catalog::CatalogError;
use shadow_domain::{PhotoId, RecipeCommitId};

use crate::{
    open_desktop_session,
    tests::fixtures::{
        edit_session::test_edit_session,
        grade_stack::{assert_close, ffi_parameters},
    },
};

#[test]
fn simultaneous_editors_publish_exactly_one_commit_per_expected_head() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let mut state = session
        .autosave_basic_edit_working_at(
            &photo_id,
            &source_path,
            "",
            "",
            &ffi_parameters(0.0, 1.0, [0.0; 2], 1.0),
            1_000,
        )
        .unwrap();
    let parsed_photo: PhotoId = photo_id.parse().unwrap();
    for round in 0..12 {
        let barrier = Arc::new(Barrier::new(2));
        let handles = [0.25, 0.75].map(|exposure| {
            let root = root.clone();
            let photo_id = photo_id.clone();
            let source_path = source_path.clone();
            let expected_head = state.working_commit_id.clone();
            let expected_variant = state.active_variant_id.clone();
            let barrier = Arc::clone(&barrier);
            std::thread::spawn(move || {
                let editor = open_desktop_session(
                    root.join("catalog.sqlite").to_str().unwrap(),
                    root.join("cache").to_str().unwrap(),
                );
                // Both threads reach the barrier even if opening failed, so an
                // infrastructure failure reports an error instead of deadlocking.
                barrier.wait();
                let editor = editor.unwrap();
                match editor.autosave_basic_edit_working_for_variant_at(
                    &photo_id,
                    &source_path,
                    &expected_head,
                    &expected_head,
                    Some(&expected_variant),
                    &ffi_parameters(exposure, 1.0, [0.0; 2], 1.0),
                    2_000 + round,
                ) {
                    Ok(saved) => Some((saved.working_commit_id, exposure)),
                    Err(error) => {
                        assert!(
                            matches!(
                                error.downcast_ref::<CatalogError>(),
                                Some(CatalogError::RecipeRefExpectationMismatch { .. })
                            ),
                            "unexpected concurrent error: {error:#}"
                        );
                        None
                    }
                }
            })
        });
        let results = handles.map(|handle| handle.join().unwrap());
        let winners: Vec<_> = results.into_iter().flatten().collect();
        assert_eq!(
            winners.len(),
            1,
            "one writer wins each shared expected head"
        );
        let previous_head: RecipeCommitId = state.working_commit_id.parse().unwrap();
        state = session.photo_edit_state(&photo_id, &source_path).unwrap();
        assert_eq!(state.working_commit_id, winners[0].0);
        assert_close(
            state.settings.grade_nodes[0].basic.exposure_stops,
            winners[0].1,
        );
        let commits = session.catalog.recipe_commits(parsed_photo).unwrap();
        assert_eq!(
            commits.len(),
            usize::try_from(round).unwrap() + 2,
            "a rejected writer leaves no orphan immutable commit"
        );
        assert_eq!(commits[0].commit.parents(), &[previous_head]);
    }
    let expected_head = state.working_commit_id;
    drop(session);
    let reopened = open_desktop_session(
        root.join("catalog.sqlite").to_str().unwrap(),
        root.join("cache").to_str().unwrap(),
    )
    .unwrap();
    assert_eq!(
        reopened
            .photo_edit_state(&photo_id, &source_path)
            .unwrap()
            .working_commit_id,
        expected_head
    );
    drop(reopened);
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn long_edit_sequence_preserves_named_checkpoints_checkout_and_reopen() {
    let (root, mut session, photo_id, source_path) = test_edit_session();
    let baseline = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &ffi_parameters(0.0, 1.0, [0.0; 2], 1.0),
            "Baseline",
            1_000,
        )
        .unwrap();
    let baseline_id = baseline.working_commit_id.clone();
    let mut state = baseline;
    let mut named_count = 1;
    for step in 1..=64 {
        let exposure = f64::from(step) / 64.0;
        state = session
            .autosave_basic_edit_working_for_variant_at(
                &photo_id,
                &source_path,
                &state.working_commit_id,
                &state.working_commit_id,
                Some(&state.active_variant_id),
                &ffi_parameters(exposure, 1.0, [0.0; 2], 1.0),
                2_000 + i64::from(step) * 2,
            )
            .unwrap();
        if step % 8 == 0 {
            state = session
                .save_basic_edit_version_at_with_expected(
                    &photo_id,
                    &source_path,
                    &state.working_commit_id,
                    &state.working_commit_id,
                    &state.settings,
                    &format!("Checkpoint {step}"),
                    2_001 + i64::from(step) * 2,
                )
                .unwrap();
            named_count += 1;
        }
        assert_eq!(state.versions.len(), named_count);
        if step % 16 == 0 {
            let expected_head = state.working_commit_id.clone();
            let checkout = session
                .checkout_basic_edit_version(&photo_id, &source_path, &baseline_id)
                .unwrap();
            assert!(checkout.is_version_draft);
            assert_close(checkout.settings.grade_nodes[0].basic.exposure_stops, 0.0);
            drop(session);
            session = open_desktop_session(
                root.join("catalog.sqlite").to_str().unwrap(),
                root.join("cache").to_str().unwrap(),
            )
            .unwrap();
            state = session.photo_edit_state(&photo_id, &source_path).unwrap();
            assert_eq!(state.working_commit_id, expected_head);
            assert_close(state.settings.grade_nodes[0].basic.exposure_stops, exposure);
            assert_eq!(state.versions.len(), named_count);
        }
    }
    assert_eq!(
        session
            .catalog
            .recipe_commits(photo_id.parse().unwrap())
            .unwrap()
            .len(),
        73
    );
    drop(session);
    std::fs::remove_dir_all(root).unwrap();
}
