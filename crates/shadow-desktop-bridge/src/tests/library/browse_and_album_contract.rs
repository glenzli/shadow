//! Photo-first Library paging, filters, state, decisions, and album contracts.

use shadow_catalog::{
    LibraryPhotoFacts, RecordLibraryPlaceResolution, RecordLibraryPlaceResolutionStatus,
    RegisterAsset, RepresentationFingerprint,
};
use shadow_domain::{AssetLocation, EntityId, Platform, RepresentationId, RepresentationKind};

use crate::{DesktopSession, ffi, open_desktop_session};

#[test]
#[allow(clippy::too_many_lines)]
fn library_page_is_photo_first_keyset_paginated_and_filterable() {
    let root = std::env::temp_dir().join(format!(
        "shadow-desktop-library-page-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create Library fixture");
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open desktop session");

    let newest_path = root.join("renamed/newest.nef");
    let older_path = root.join("older.nef");
    let newest = register_library_fixture(&session, &newest_path, 20_000, Some(200), 1_700_000_200);
    let older = register_library_fixture(&session, &older_path, 10_000, Some(100), 1_700_000_100);
    assert_eq!(
        session
            .catalog
            .record_library_place_resolution(&RecordLibraryPlaceResolution {
                latitude_e7: 399_000_000,
                longitude_e7: 1_164_000_000,
                country_code: "CN".into(),
                country_name: "China".into(),
                administrative_area: "Beijing".into(),
                locality: "Beijing".into(),
                display_name: "Beijing, China".into(),
                provider_id: "fixture".into(),
                provider_version: "v1".into(),
                locale: "en".into(),
                resolved_at_ms: 1_700_000_300_000,
            })
            .expect("record resolved Library place"),
        RecordLibraryPlaceResolutionStatus::Recorded
    );
    let library_state = session
        .set_photo_library_state(&newest.photo_id.to_string(), true, "blue")
        .expect("persist newest Library state through the bridge");
    assert_eq!(library_state.photo_id, newest.photo_id.to_string());
    assert!(library_state.liked);
    assert_eq!(library_state.color_label, "blue");
    assert!(library_state.updated_at_ms > 0);
    let decision = session
        .set_review_photo_decision(
            &newest.photo_id.to_string(),
            0,
            ffi::FfiDecisionFlag::Picked,
            4,
        )
        .expect("pick and rate newest photo");

    let filtered = ffi_library_filter();
    let filtered_page = session
        .library_photo_page(
            &filtered,
            ffi::FfiLibraryPhotoOrder::CaptureTimeDescending,
            &ffi_library_start_cursor(),
            16,
        )
        .expect("query filtered Library page");
    assert_eq!(
        session
            .library_photo_count(&filtered)
            .expect("count filter"),
        1
    );
    assert!(!filtered_page.has_more);
    assert_eq!(filtered_page.items.len(), 1);
    let item = &filtered_page.items[0];
    assert_eq!(item.photo_id, newest.photo_id.to_string());
    assert_eq!(item.representation_id, newest.representation_id.to_string());
    assert_eq!(item.location_id, newest.location_id.to_string());
    assert_eq!(item.title, "newest.nef");
    assert_eq!(item.source_path, newest_path.to_string_lossy());
    assert!(item.source_available);
    assert_eq!(item.source_byte_len, 20_000);
    assert!(item.has_source_modified_at);
    assert_eq!(item.source_modified_at_ms, 200);
    assert!(!item.has_visual);
    assert!(item.visual_handle.is_empty());
    assert!(item.has_metadata);
    assert!(item.has_captured_at);
    assert_eq!(item.captured_at_unix_seconds, 1_700_000_200);
    assert_eq!(item.camera_model, "Nikon Z 8");
    assert_eq!(item.resolved_country_name, "China");
    assert_eq!(item.resolved_locality_label, "Beijing · China");
    assert_eq!(item.resolved_place_name, "Beijing, China");
    assert!(item.liked);
    assert_eq!(item.color_label, "blue");
    assert_eq!(item.decision_head_sequence, decision.sequence);
    assert_eq!(item.decision_flag, ffi::FfiDecisionFlag::Picked);
    assert_eq!(item.decision_rating, 4);

    std::fs::remove_file(&newest_path).expect("disconnect newest source");
    let unavailable = session
        .library_photo_page(
            &filtered,
            ffi::FfiLibraryPhotoOrder::CaptureTimeDescending,
            &ffi_library_start_cursor(),
            16,
        )
        .expect("refresh unavailable Library source");
    assert_eq!(unavailable.items.len(), 1);
    assert!(!unavailable.items[0].source_available);

    let first = session
        .library_photo_page(
            &ffi_library_neutral_filter(),
            ffi::FfiLibraryPhotoOrder::CaptureTimeDescending,
            &ffi_library_start_cursor(),
            1,
        )
        .expect("read first Library page");
    assert!(first.has_more);
    assert_eq!(first.items.len(), 1);
    assert_eq!(first.items[0].photo_id, newest.photo_id.to_string());
    let second = session
        .library_photo_page(
            &ffi_library_neutral_filter(),
            ffi::FfiLibraryPhotoOrder::CaptureTimeDescending,
            &first.next_cursor,
            1,
        )
        .expect("read second Library page");
    assert!(!second.has_more);
    assert_eq!(second.items.len(), 1);
    assert_eq!(second.items[0].photo_id, older.photo_id.to_string());

    // Hierarchical keywords are durable user organization. A parent predicate
    // matches a direct assignment anywhere in its subtree, while exclusions
    // reject any matching subtree.
    let people = session
        .create_library_keyword("", "People")
        .expect("create root keyword");
    let family = session
        .create_library_keyword(&people.id, "Family")
        .expect("create child keyword");
    let work = session
        .create_library_keyword("", "Work")
        .expect("create exclusion keyword");
    let assigned = session
        .assign_library_keyword(&family.id, vec![newest.photo_id.to_string()])
        .expect("assign child keyword through the public bridge");
    assert_eq!(assigned.requested_photo_count, 1);
    assert_eq!(assigned.changed_photo_count, 1);
    session
        .assign_library_keyword(&work.id, vec![older.photo_id.to_string()])
        .expect("assign excluded keyword");

    let tree = session
        .library_keywords()
        .expect("read flattened keyword taxonomy");
    assert_eq!(tree.len(), 3);
    assert_eq!(tree[0].id, people.id);
    assert_eq!(tree[0].subtree_photo_count, 1);
    assert_eq!(tree[1].parent_id, people.id);
    assert_eq!(tree[1].depth, 1);
    let photo_keywords = session
        .library_keywords_for_photo(&newest.photo_id.to_string())
        .expect("read selected-photo keyword assignments");
    assert_eq!(photo_keywords.len(), 1);
    assert_eq!(photo_keywords[0].keyword.id, family.id);
    assert_eq!(
        photo_keywords[0].origin,
        ffi::FfiLibraryKeywordOrigin::Manual
    );

    let mut keyword_filter = ffi_library_neutral_filter();
    keyword_filter.keyword_ids_all.push(people.id.clone());
    keyword_filter
        .excluded_keyword_ids_any
        .push(work.id.clone());
    let keyword_page = session
        .library_photo_page(
            &keyword_filter,
            ffi::FfiLibraryPhotoOrder::CaptureTimeDescending,
            &ffi_library_start_cursor(),
            16,
        )
        .expect("query hierarchical include/exclude keyword predicates");
    assert_eq!(keyword_page.items.len(), 1);
    assert_eq!(keyword_page.items[0].photo_id, newest.photo_id.to_string());

    // Manual albums retain explicit membership while smart albums execute
    // their frozen Library filter through the same photo-first page path.
    let manual = session
        .create_manual_library_album("Trip selects")
        .expect("create manual Library album");
    assert_eq!(manual.kind, ffi::FfiLibraryAlbumKind::Manual);
    assert!(manual.query_filter.album_id.is_empty());
    session
        .add_photo_to_manual_library_album(&manual.id, &newest.photo_id.to_string())
        .expect("add selected photo to manual album");
    let memberships = session
        .library_albums_for_photo(&newest.photo_id.to_string())
        .expect("list manual album memberships");
    assert_eq!(memberships.len(), 1);
    assert_eq!(memberships[0].id, manual.id);

    let smart = session
        .create_smart_library_album("Nikon picks", &filtered)
        .expect("create smart Library album");
    assert_eq!(smart.kind, ffi::FfiLibraryAlbumKind::Smart);
    assert_eq!(smart.query_filter.camera_key, filtered.camera_key);
    assert_eq!(
        session
            .smart_library_photo_count(&smart.id)
            .expect("count filtered smart album"),
        1
    );
    let smart_page = session
        .smart_library_photo_page(&smart.id, &ffi_library_start_cursor(), 16)
        .expect("page filtered smart album");
    assert_eq!(smart_page.items.len(), 1);
    assert_eq!(smart_page.items[0].photo_id, newest.photo_id.to_string());
    let keyword_smart = session
        .replace_smart_library_album_filter(&smart.id, &keyword_filter)
        .expect("persist hierarchical keyword predicates in a smart album");
    assert_eq!(
        keyword_smart.query_filter.keyword_ids_all,
        keyword_filter.keyword_ids_all
    );
    assert_eq!(
        keyword_smart.query_filter.excluded_keyword_ids_any,
        keyword_filter.excluded_keyword_ids_any
    );
    assert_eq!(
        session
            .smart_library_photo_count(&smart.id)
            .expect("count keyword-backed smart album"),
        1
    );

    let renamed = session
        .rename_library_album(&manual.id, "Trip picks 2026")
        .expect("rename manual Library album");
    assert_eq!(renamed.name, "Trip picks 2026");
    let broadened = session
        .replace_smart_library_album_filter(&smart.id, &ffi_library_neutral_filter())
        .expect("replace smart Library filter");
    assert_eq!(broadened.kind, ffi::FfiLibraryAlbumKind::Smart);
    assert_eq!(
        session
            .smart_library_photo_count(&smart.id)
            .expect("count broadened smart album"),
        2
    );
    assert!(
        session
            .remove_photo_from_manual_library_album(&manual.id, &newest.photo_id.to_string())
            .expect("remove manual album membership")
    );
    assert!(
        session
            .delete_library_album(&manual.id)
            .expect("delete manual Library album")
    );
    let albums = session
        .library_albums()
        .expect("list remaining Library albums");
    assert_eq!(albums.len(), 1);
    assert_eq!(albums[0].id, smart.id);

    let unassigned = session
        .remove_library_keyword(&family.id, vec![newest.photo_id.to_string()])
        .expect("remove a batch keyword assignment");
    assert_eq!(unassigned.changed_photo_count, 1);
    let deleted = session
        .delete_library_keyword_subtree(&people.id)
        .expect("delete keyword subtree");
    assert_eq!(deleted.deleted_keyword_count, 2);
    assert_eq!(deleted.deleted_assignment_count, 0);

    assert!(
        session
            .archive_library_photo(&older.photo_id.to_string())
            .expect("archive Library photo through bridge")
    );
    assert!(
        !session
            .archive_library_photo(&older.photo_id.to_string())
            .expect("repeat bridge archive")
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove Library fixture");
}

fn ffi_library_neutral_filter() -> ffi::FfiLibraryPhotoFilter {
    ffi::FfiLibraryPhotoFilter {
        has_capture_start: false,
        capture_start_unix_seconds: 0,
        has_capture_end: false,
        capture_end_unix_seconds: 0,
        capture_month: String::new(),
        has_chinese_lunar_month: false,
        chinese_lunar_month: 0,
        has_chinese_lunar_day: false,
        chinese_lunar_day: 0,
        has_chinese_lunar_is_leap_month: false,
        chinese_lunar_is_leap_month: false,
        camera_key: String::new(),
        lens_key: String::new(),
        country_key: String::new(),
        locality_key: String::new(),
        living_place_rules: Vec::new(),
        include_living_place_rules: false,
        has_aperture_minimum: false,
        aperture_minimum_milli: 0,
        has_aperture_maximum: false,
        aperture_maximum_milli: 0,
        has_liked: false,
        liked: false,
        color_label: String::new(),
        flag: ffi::FfiLibraryFlagFilter::Any,
        has_minimum_rating: false,
        minimum_rating: 0,
        has_development_edits: false,
        development_edits: false,
        album_id: String::new(),
        keyword_ids_all: Vec::new(),
        excluded_keyword_ids_any: Vec::new(),
    }
}

fn ffi_library_filter() -> ffi::FfiLibraryPhotoFilter {
    ffi::FfiLibraryPhotoFilter {
        camera_key: shadow_catalog::library_equipment_key("Nikon Corporation", "Nikon Z 8"),
        lens_key: shadow_catalog::library_equipment_key("Nikon", "NIKKOR Z 24-120mm f/4 S"),
        has_aperture_minimum: true,
        aperture_minimum_milli: 4_000,
        has_aperture_maximum: true,
        aperture_maximum_milli: 4_000,
        has_liked: true,
        liked: true,
        color_label: "blue".into(),
        flag: ffi::FfiLibraryFlagFilter::Picked,
        has_minimum_rating: true,
        minimum_rating: 4,
        ..ffi_library_neutral_filter()
    }
}

fn ffi_library_start_cursor() -> ffi::FfiLibraryPhotoCursor {
    ffi::FfiLibraryPhotoCursor {
        photo_id: String::new(),
        has_capture_time: false,
        captured_at_unix_seconds: 0,
        file_name: String::new(),
    }
}

fn register_library_fixture(
    session: &DesktopSession,
    path: &std::path::Path,
    byte_len: u64,
    modified_at_ms: Option<i64>,
    captured_at_unix_seconds: i64,
) -> shadow_catalog::RegisteredAsset {
    std::fs::create_dir_all(path.parent().expect("Library fixture parent"))
        .expect("create Library source folder");
    std::fs::write(
        path,
        vec![0_u8; usize::try_from(byte_len).expect("bounded fixture size")],
    )
    .expect("write Library source fixture");
    let display_path = path.to_string_lossy().into_owned();
    let registered = session
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                display_path.as_bytes().to_vec(),
                display_path,
            ),
            byte_len,
            modified_at_ms,
            now_ms: captured_at_unix_seconds,
        })
        .expect("register Library fixture source");
    session
        .catalog
        .upsert_photo_library_facts(&LibraryPhotoFacts {
            photo_id: registered.photo_id,
            captured_at_unix_seconds: Some(captured_at_unix_seconds),
            capture_day: "2023-11-14".into(),
            camera_make: "Nikon Corporation".into(),
            camera_model: "Nikon Z 8".into(),
            lens_make: "Nikon".into(),
            lens_model: "NIKKOR Z 24-120mm f/4 S".into(),
            aperture_milli: Some(4_000),
            focal_length_tenth_mm: Some(240),
            iso_speed: Some(800.0),
            latitude_e7: Some(399_000_000),
            longitude_e7: Some(1_164_000_000),
            place_name: "Beijing".into(),
            indexed_representation_id: Some(registered.representation_id),
            indexed_source: Some(RepresentationFingerprint {
                byte_len,
                modified_at_ms,
            }),
            indexed_at_ms: captured_at_unix_seconds + 1,
        })
        .expect("index Library fixture metadata");
    registered
}
