use super::asset_registration_fixture::register;
use crate::{Catalog, LibraryKeywordAssignmentOrigin, LibraryPhotoFilter, SmartAlbumQueryV1};

#[test]
fn hierarchy_moves_as_a_subtree_and_delete_reports_its_exact_scope() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let wildlife = catalog
        .create_library_keyword(None, "Wildlife", 10)
        .expect("create root keyword");
    let birds = catalog
        .create_library_keyword(Some(wildlife.id), "Birds", 11)
        .expect("create child keyword");
    let raptors = catalog
        .create_library_keyword(Some(birds.id), "Raptors", 12)
        .expect("create grandchild keyword");
    let travel = catalog
        .create_library_keyword(None, "Travel", 13)
        .expect("create second root");

    let renamed = catalog
        .rename_library_keyword(birds.id, "Avian", 20)
        .expect("rename keyword");
    assert_eq!(renamed.name, "Avian");
    let moved = catalog
        .move_library_keyword(birds.id, Some(travel.id), 21)
        .expect("move subtree");
    assert_eq!(moved.parent_id, Some(travel.id));

    let tree = catalog.library_keyword_tree().expect("read tree");
    assert_eq!(
        tree.iter()
            .find(|record| record.id == raptors.id)
            .expect("raptors keyword")
            .depth,
        2
    );
    assert!(
        catalog
            .move_library_keyword(travel.id, Some(raptors.id), 22)
            .is_err(),
        "a parent must not move beneath its own descendant"
    );
    assert!(
        catalog
            .create_library_keyword(Some(travel.id), "Avian", 23)
            .is_err(),
        "siblings must have unique normalized names"
    );

    let photo = register(&mut catalog, "/archive/raptor.nef").photo_id;
    catalog
        .assign_library_keyword_to_photos(
            raptors.id,
            &[photo],
            LibraryKeywordAssignmentOrigin::Manual,
            "",
            None,
            30,
        )
        .expect("assign descendant keyword");
    let tree = catalog.library_keyword_tree().expect("read counted tree");
    assert_eq!(
        tree.iter()
            .find(|record| record.id == travel.id)
            .expect("travel root")
            .subtree_photo_count,
        1
    );
    let receipt = catalog
        .delete_library_keyword_subtree(birds.id)
        .expect("delete moved subtree");
    assert_eq!(receipt.deleted_keyword_count, 2);
    assert_eq!(receipt.deleted_assignment_count, 1);
    assert_eq!(
        catalog
            .library_keyword_tree()
            .expect("read remaining tree")
            .len(),
        2
    );
}

#[test]
fn subtree_filters_compose_with_exclusion_and_smart_album_queries() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let animal = catalog
        .create_library_keyword(None, "Animal", 10)
        .expect("create animal");
    let bird = catalog
        .create_library_keyword(Some(animal.id), "Bird", 11)
        .expect("create bird");
    let blurry = catalog
        .create_library_keyword(None, "Needs review", 12)
        .expect("create exclusion");
    let eagle = register(&mut catalog, "/archive/eagle.nef").photo_id;
    let uncertain = register(&mut catalog, "/archive/uncertain-bird.nef").photo_id;
    let landscape = register(&mut catalog, "/archive/landscape.nef").photo_id;

    catalog
        .assign_library_keyword_to_photos(
            bird.id,
            &[eagle, uncertain, eagle],
            LibraryKeywordAssignmentOrigin::Manual,
            "",
            None,
            20,
        )
        .expect("assign bird batch");
    catalog
        .assign_library_keyword_to_photos(
            blurry.id,
            &[uncertain],
            LibraryKeywordAssignmentOrigin::Manual,
            "",
            None,
            21,
        )
        .expect("assign exclusion");

    let filter = LibraryPhotoFilter {
        keyword_ids_all: vec![animal.id],
        excluded_keyword_ids_any: vec![blurry.id],
        ..LibraryPhotoFilter::default()
    };
    let page = catalog
        .library_photo_page(&filter, crate::LibraryPhotoOrder::default(), None, 16)
        .expect("filter by keyword subtrees");
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.items[0].photo_id, eagle);
    assert_ne!(page.items[0].photo_id, landscape);

    let query = SmartAlbumQueryV1::new(filter).expect("create keyword smart query");
    let round_trip =
        SmartAlbumQueryV1::from_json(&query.to_json().expect("serialize keyword smart query"))
            .expect("parse keyword smart query")
            .library_filter()
            .expect("read executable keyword filter");
    assert_eq!(round_trip.keyword_ids_all, vec![animal.id]);
    assert_eq!(round_trip.excluded_keyword_ids_any, vec![blurry.id]);
}

#[test]
fn accepted_ai_assignment_requires_provenance_and_manual_writes_are_idempotent() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let keyword = catalog
        .create_library_keyword(None, "Portrait", 10)
        .expect("create keyword");
    let photo = register(&mut catalog, "/archive/portrait.nef").photo_id;

    assert!(
        catalog
            .assign_library_keyword_to_photos(
                keyword.id,
                &[photo],
                LibraryKeywordAssignmentOrigin::AiAccepted,
                "",
                Some(920),
                20,
            )
            .is_err(),
        "accepted model output must retain a source identity"
    );
    let first = catalog
        .assign_library_keyword_to_photos(
            keyword.id,
            &[photo],
            LibraryKeywordAssignmentOrigin::Manual,
            "",
            None,
            21,
        )
        .expect("assign manually");
    let repeated = catalog
        .assign_library_keyword_to_photos(
            keyword.id,
            &[photo],
            LibraryKeywordAssignmentOrigin::Manual,
            "",
            None,
            22,
        )
        .expect("repeat manual assignment");
    assert_eq!(first.changed_photo_count, 1);
    assert_eq!(repeated.changed_photo_count, 0);
    assert_eq!(
        catalog
            .library_keywords_for_photo(photo)
            .expect("read assignments")[0]
            .origin,
        LibraryKeywordAssignmentOrigin::Manual
    );
}
