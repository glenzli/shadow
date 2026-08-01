//! Cross-facade contract for stable photo and Library History projection.

use crate::{ffi, tests::fixtures::edit_session::test_edit_session};

use super::fixtures::grade_stack::ffi_parameters;

#[test]
fn durable_history_pages_keep_refs_diffs_and_keyset_continuation() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let first = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            "",
            &ffi_parameters(0.25, 1.1, [0.0; 2], 0.9),
            "First look",
            1_000,
        )
        .expect("save first version");
    let second = session
        .save_basic_edit_version_at(
            &photo_id,
            &source_path,
            &first.working_commit_id,
            &ffi_parameters(0.75, 1.2, [0.0; 2], 0.8),
            "Second look",
            2_000,
        )
        .expect("save second version");

    let first_page = session
        .photo_edit_history_page(&photo_id, &ffi::FfiHistoryCursor::default(), 1)
        .expect("first photo page");
    assert_eq!(first_page.entries.len(), 1);
    assert!(first_page.has_more);
    let newest = &first_page.entries[0];
    assert_eq!(newest.commit_id, second.working_commit_id);
    assert_eq!(newest.name, "Second look");
    assert!(newest.is_working);
    assert!(newest.is_named);
    assert!(
        newest
            .changed_parameter_keys
            .contains(&"exposure_stops".to_owned())
    );
    assert!(newest.refs.iter().any(|reference| {
        reference.kind == ffi::FfiHistoryRefKind::Working && reference.commit_id == newest.commit_id
    }));
    assert!(newest.refs.iter().any(|reference| {
        reference.kind == ffi::FfiHistoryRefKind::NamedVersion
            && reference.commit_id == newest.commit_id
    }));

    let next_page = session
        .photo_edit_history_page(&photo_id, &first_page.next_cursor, 1)
        .expect("continued photo page");
    assert_eq!(next_page.entries.len(), 1);
    assert_eq!(next_page.entries[0].commit_id, first.working_commit_id);
    assert!(!next_page.has_more);

    let library_page = session
        .library_edit_history_page(&ffi::FfiHistoryCursor::default(), 1)
        .expect("first Library page");
    assert_eq!(library_page.entries.len(), 1);
    assert!(library_page.has_more);
    assert_eq!(library_page.entries[0].message, "Second look");
    assert!(library_page.entries[0].is_head);
    assert_eq!(library_page.entries[0].photo_changes, 1);

    let mut ref_names = Vec::new();
    let mut cursor = String::new();
    loop {
        let page = session
            .library_edit_history_ref_page(&cursor, 1)
            .expect("Library ref page");
        ref_names.extend(page.refs.iter().map(|reference| reference.name.clone()));
        if !page.has_more {
            break;
        }
        cursor = page.next_cursor;
    }
    assert!(ref_names.windows(2).all(|names| names[0] < names[1]));
    assert!(ref_names.iter().any(|name| name == "heads/main"));
    assert!(ref_names.iter().any(|name| name.starts_with("versions/")));

    drop(session);
    std::fs::remove_dir_all(root).expect("remove history fixture");
}
