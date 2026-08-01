use shadow_domain::{EditRepositoryRefExpectation, EditRepositoryRefKind};

use crate::Catalog;

use super::{
    super::{
        CommitEditRepository, EditRepositoryRefUpdate, MAX_EDIT_REPOSITORY_HISTORY_PAGE_SIZE,
        MAX_EDIT_REPOSITORY_REF_PAGE_SIZE,
    },
    object_graph_fixtures::{commit, initial_pack},
};

#[test]
fn library_history_page_is_keyset_stable_and_projects_refs() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let (pack, root_id) = initial_pack();
    catalog.store_edit_object_pack(&pack).unwrap();
    let root = commit(root_id, Vec::new(), 100);
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: root.clone(),
            update_refs: vec![EditRepositoryRefUpdate {
                name: "versions/original".into(),
                kind: EditRepositoryRefKind::NamedVersion,
                expected: EditRepositoryRefExpectation::Missing,
                updated_at_ms: 100,
            }],
        })
        .unwrap();
    let middle = commit(root_id, vec![root.id()], 200);
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: middle.clone(),
            update_refs: Vec::new(),
        })
        .unwrap();
    let head = commit(root_id, vec![middle.id()], 300);
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: head.clone(),
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::Missing,
                updated_at_ms: 300,
            }],
        })
        .unwrap();

    let first = catalog.edit_repository_history_page(None, 2).unwrap();
    assert_eq!(
        first
            .entries
            .iter()
            .map(|entry| entry.record.commit.id())
            .collect::<Vec<_>>(),
        vec![head.id(), middle.id()]
    );
    assert_eq!(first.entries[0].refs[0].name, "heads/main");
    assert!(first.entries[1].refs.is_empty());

    let newer = commit(root_id, vec![head.id()], 400);
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: newer,
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::At(head.id()),
                updated_at_ms: 400,
            }],
        })
        .unwrap();
    let second = catalog
        .edit_repository_history_page(first.next_cursor.as_ref(), 2)
        .unwrap();
    assert_eq!(second.entries.len(), 1);
    assert_eq!(second.entries[0].record.commit.id(), root.id());
    assert_eq!(second.entries[0].refs[0].name, "versions/original");
    assert!(second.next_cursor.is_none());
}

#[test]
fn library_ref_page_is_stable_and_bounded() {
    let mut catalog = Catalog::open_in_memory().unwrap();
    let (pack, root_id) = initial_pack();
    catalog.store_edit_object_pack(&pack).unwrap();
    let root = commit(root_id, Vec::new(), 100);
    catalog
        .commit_edit_repository(&CommitEditRepository {
            commit: root,
            update_refs: [
                ("versions/z-last", EditRepositoryRefKind::NamedVersion),
                ("heads/main", EditRepositoryRefKind::Branch),
                ("tags/favorite", EditRepositoryRefKind::Tag),
                ("versions/a-first", EditRepositoryRefKind::NamedVersion),
            ]
            .into_iter()
            .map(|(name, kind)| EditRepositoryRefUpdate {
                name: name.into(),
                kind,
                expected: EditRepositoryRefExpectation::Missing,
                updated_at_ms: 100,
            })
            .collect(),
        })
        .unwrap();

    let first = catalog.edit_repository_ref_page(None, 2).unwrap();
    assert_eq!(
        first
            .refs
            .iter()
            .map(|reference| reference.name.as_str())
            .collect::<Vec<_>>(),
        vec!["heads/main", "tags/favorite"]
    );
    let second = catalog
        .edit_repository_ref_page(first.next_cursor.as_deref(), 2)
        .unwrap();
    assert_eq!(
        second
            .refs
            .iter()
            .map(|reference| reference.name.as_str())
            .collect::<Vec<_>>(),
        vec!["versions/a-first", "versions/z-last"]
    );
    assert!(second.next_cursor.is_none());
    assert!(catalog.edit_repository_ref_page(None, 0).is_err());
    assert!(
        catalog
            .edit_repository_ref_page(None, MAX_EDIT_REPOSITORY_REF_PAGE_SIZE + 1)
            .is_err()
    );
    assert!(
        catalog
            .edit_repository_history_page(None, MAX_EDIT_REPOSITORY_HISTORY_PAGE_SIZE + 1)
            .is_err()
    );
}
