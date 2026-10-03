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

fn materialize_tool_source(source: &str) {
    std::fs::write(source, vec![0_u8; 4_096]).unwrap();
    std::fs::OpenOptions::new()
        .write(true)
        .open(source)
        .unwrap()
        .set_times(
            std::fs::FileTimes::new()
                .set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_millis(123)),
        )
        .unwrap();
}

#[test]
fn tool_commit_receipt_stays_exact_across_head_variant_and_restart_changes() {
    let (root, session, photo, source) = test_edit_session();
    materialize_tool_source(&source);
    let initial = session.photo_edit_state(&photo, &source).unwrap();
    let representation = session
        .catalog
        .photo_source(photo.parse().unwrap())
        .unwrap()
        .unwrap()
        .representation_id
        .to_string();
    let receipt = session
        .commit_edit_tool_draft(
            &photo,
            &source,
            &representation,
            "",
            "",
            &initial.active_variant_id,
            &ffi_parameters(0.75, 1.0, [0.0; 2], 1.0),
        )
        .unwrap();
    let other = open_desktop_session(
        root.join("catalog.sqlite").to_str().unwrap(),
        root.join("cache").to_str().unwrap(),
    )
    .unwrap();
    let next = other
        .commit_edit_tool_draft(
            &photo,
            &source,
            &representation,
            &receipt.commit_id,
            &receipt.commit_id,
            &receipt.variant_id,
            &ffi_parameters(1.25, 1.0, [0.0; 2], 1.0),
        )
        .unwrap();
    assert_ne!(receipt.commit_id, next.commit_id);
    assert_close(receipt.settings.grade_nodes[0].basic.exposure_stops, 0.75);
    let record = session
        .catalog
        .recipe_commit(photo.parse().unwrap(), receipt.commit_id.parse().unwrap())
        .unwrap()
        .unwrap();
    assert_eq!(
        receipt.snapshot_digest,
        crate::digest_hex::encode_hex(&record.snapshot_digest)
    );
    assert_eq!(receipt.variant_id, initial.active_variant_id);
    assert!(
        session
            .commit_edit_tool_draft(
                &photo,
                &source,
                &representation,
                &receipt.commit_id,
                &receipt.commit_id,
                &receipt.variant_id,
                &ffi_parameters(2.0, 1.0, [0.0; 2], 1.0)
            )
            .is_err()
    );
    let alternate = other
        .create_photo_variant(&photo, &source, "Tool CAS test")
        .unwrap();
    assert_eq!(alternate.working_commit_id, next.commit_id);
    assert!(
        session
            .commit_edit_tool_draft(
                &photo,
                &source,
                &representation,
                &next.commit_id,
                &next.commit_id,
                &receipt.variant_id,
                &ffi_parameters(2.0, 1.0, [0.0; 2], 1.0)
            )
            .is_err()
    );
    assert!(
        session
            .commit_edit_tool_draft(
                &photo,
                &source,
                "different-source",
                &next.commit_id,
                &next.commit_id,
                &alternate.active_variant_id,
                &ffi_parameters(2.0, 1.0, [0.0; 2], 1.0)
            )
            .is_err()
    );
    assert!(
        session
            .commit_edit_tool_draft(
                &photo,
                &source,
                &representation,
                &next.commit_id,
                &next.commit_id,
                "",
                &ffi_parameters(2.0, 1.0, [0.0; 2], 1.0)
            )
            .is_err()
    );
    drop(other);
    drop(session);
    let reopened = open_desktop_session(
        root.join("catalog.sqlite").to_str().unwrap(),
        root.join("cache").to_str().unwrap(),
    )
    .unwrap();
    let current = reopened.photo_edit_state(&photo, &source).unwrap();
    assert_eq!(current.working_commit_id, next.commit_id);
    assert_eq!(current.active_variant_id, alternate.active_variant_id);
    assert_close(current.settings.grade_nodes[0].basic.exposure_stops, 1.25);
    assert_eq!(
        reopened
            .catalog
            .recipe_commits(photo.parse().unwrap())
            .unwrap()
            .len(),
        2
    );
    drop(reopened);
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn tool_export_pins_requested_commit_instead_of_later_working_head() {
    let (root, session, photo, source) = test_edit_session();
    materialize_tool_source(&source);
    let initial = session.photo_edit_state(&photo, &source).unwrap();
    let representation = session
        .catalog
        .photo_source(photo.parse().unwrap())
        .unwrap()
        .unwrap()
        .representation_id
        .to_string();
    let first = session
        .commit_edit_tool_draft(
            &photo,
            &source,
            &representation,
            "",
            "",
            &initial.active_variant_id,
            &ffi_parameters(0.75, 1.0, [0.0; 2], 1.0),
        )
        .unwrap();
    let next = session
        .commit_edit_tool_draft(
            &photo,
            &source,
            &representation,
            &first.commit_id,
            &first.commit_id,
            &first.variant_id,
            &ffi_parameters(1.25, 1.0, [0.0; 2], 1.0),
        )
        .unwrap();
    let target = |commit: &str, rep: &str| crate::ffi::FfiDurableExportTarget {
        photo_id: photo.clone(),
        source_path: source.clone(),
        output_path: crate::native_path_ffi::location_to_ffi(&shadow_native_path::native_location(
            &root.join("output.png"),
        ))
        .unwrap(),
        recipe_commit_id: commit.into(),
        representation_id: rep.into(),
    };
    session
        .enqueue_durable_export_job(
            vec![target(&first.commit_id, &representation)],
            r#"{"format":"png"}"#,
        )
        .unwrap();
    let item = session
        .catalog
        .claim_next_export_item(i64::MAX - 1)
        .unwrap()
        .unwrap();
    assert_eq!(
        item.recipe_commit_id.to_string(),
        first.commit_id,
        "export must freeze the requested commit, not current working"
    );
    assert_eq!(
        crate::digest_hex::encode_hex(&item.recipe_snapshot_digest),
        first.snapshot_digest
    );
    assert!(
        session
            .enqueue_durable_export_job(
                vec![target(&next.commit_id, "different-source")],
                r#"{"format":"png"}"#
            )
            .is_err()
    );
    assert!(
        session
            .enqueue_durable_export_job(vec![target("", &representation)], r#"{"format":"png"}"#)
            .is_err()
    );
    assert_eq!(
        session
            .catalog
            .recipe_commits(photo.parse().unwrap())
            .unwrap()
            .len(),
        2
    );
    drop(session);
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn tool_commit_rejects_changed_source_without_advancing_the_working_head() {
    let (root, session, photo, source) = test_edit_session();
    materialize_tool_source(&source);
    let initial = session.photo_edit_state(&photo, &source).unwrap();
    let representation = session
        .catalog
        .photo_source(photo.parse().unwrap())
        .unwrap()
        .unwrap()
        .representation_id
        .to_string();
    let receipt = session
        .commit_edit_tool_draft(
            &photo,
            &source,
            &representation,
            "",
            "",
            &initial.active_variant_id,
            &ffi_parameters(0.75, 1.0, [0.0; 2], 1.0),
        )
        .unwrap();
    std::fs::write(&source, b"source replaced after candidate snapshot").unwrap();
    let rejected = session.commit_edit_tool_draft(
        &photo,
        &source,
        &representation,
        &receipt.commit_id,
        &receipt.commit_id,
        &receipt.variant_id,
        &ffi_parameters(1.25, 1.0, [0.0; 2], 1.0),
    );
    assert!(
        rejected.is_err(),
        "a preview of the earlier original cannot authorize a commit after source replacement"
    );
    assert_eq!(
        session
            .photo_edit_state(&photo, &source)
            .unwrap()
            .working_commit_id,
        receipt.commit_id
    );
    assert_eq!(
        session
            .catalog
            .recipe_commits(photo.parse().unwrap())
            .unwrap()
            .len(),
        1
    );
    assert_eq!(
        std::fs::read(&source).unwrap(),
        b"source replaced after candidate snapshot"
    );
    drop(session);
    std::fs::remove_dir_all(root).unwrap();
}
