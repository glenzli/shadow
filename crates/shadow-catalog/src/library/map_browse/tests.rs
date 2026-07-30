use crate::{
    Catalog, LibraryMapGrid, LibraryMapViewport, LibraryPhotoFacts, LibraryPhotoFilter,
    RegisterAsset, RegisteredAsset, RepresentationFingerprint, SetPhotoLibraryState,
};
use shadow_domain::{AssetLocation, Platform, RepresentationKind};

const GRID: LibraryMapGrid = LibraryMapGrid {
    columns: 16,
    rows: 8,
};

fn world() -> LibraryMapViewport {
    LibraryMapViewport {
        south_latitude_e7: -900_000_000,
        west_longitude_e7: -1_800_000_000,
        north_latitude_e7: 900_000_000,
        east_longitude_e7: 1_800_000_000,
    }
}

fn register(catalog: &mut Catalog, path: &str) -> RegisteredAsset {
    catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 100,
            modified_at_ms: Some(10),
            now_ms: 20,
        })
        .expect("register map source")
}

fn facts_for(
    registered: RegisteredAsset,
    captured_at_unix_seconds: Option<i64>,
    make: &str,
    model: &str,
) -> LibraryPhotoFacts {
    LibraryPhotoFacts {
        photo_id: registered.photo_id,
        captured_at_unix_seconds,
        capture_day: captured_at_unix_seconds
            .map(|_| "2023-11-14".to_owned())
            .unwrap_or_default(),
        camera_make: make.into(),
        camera_model: model.into(),
        lens_make: "Nikon".into(),
        lens_model: "NIKKOR Z 24-120mm f/4 S".into(),
        aperture_milli: Some(4_000),
        focal_length_tenth_mm: Some(240),
        iso_speed: Some(800.0),
        latitude_e7: None,
        longitude_e7: None,
        place_name: String::new(),
        indexed_representation_id: Some(registered.representation_id),
        indexed_source: Some(RepresentationFingerprint {
            byte_len: 100,
            modified_at_ms: Some(10),
        }),
        indexed_at_ms: 100,
    }
}

#[test]
fn map_snapshot_applies_effective_coordinates_and_library_filter() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let liked = register(&mut catalog, "/map/liked.nef");
    let ordinary = register(&mut catalog, "/map/ordinary.nef");
    let untagged = register(&mut catalog, "/map/untagged.nef");

    let mut liked_facts = facts_for(liked, Some(1_700_000_200), "Nikon", "Z 8");
    liked_facts.latitude_e7 = Some(312_304_000);
    liked_facts.longitude_e7 = Some(1_212_473_000);
    catalog
        .upsert_photo_library_facts(&liked_facts)
        .expect("index liked coordinates");

    let mut ordinary_facts = facts_for(ordinary, Some(1_700_000_100), "Nikon", "Z 8");
    ordinary_facts.latitude_e7 = Some(311_904_000);
    ordinary_facts.longitude_e7 = Some(1_215_473_000);
    catalog
        .upsert_photo_library_facts(&ordinary_facts)
        .expect("index ordinary coordinates");
    catalog
        .upsert_photo_library_facts(&facts_for(untagged, Some(1_700_000_000), "Nikon", "Z 8"))
        .expect("index untagged metadata");
    catalog
        .set_photo_library_state(&SetPhotoLibraryState {
            photo_id: liked.photo_id,
            liked: true,
            color_label: "none".into(),
            updated_at_ms: 300,
        })
        .expect("like one photo");

    let all = catalog
        .library_map_snapshot(&LibraryPhotoFilter::default(), world(), GRID)
        .expect("read world map");
    assert_eq!(all.photo_count, 2);
    assert_eq!(
        all.clusters
            .iter()
            .map(|cluster| cluster.photo_count)
            .sum::<u64>(),
        2
    );

    let filtered = catalog
        .library_map_snapshot(
            &LibraryPhotoFilter {
                liked: Some(true),
                ..LibraryPhotoFilter::default()
            },
            world(),
            GRID,
        )
        .expect("read liked map");
    assert_eq!(filtered.photo_count, 1);
    assert_eq!(filtered.clusters.len(), 1);
    assert_eq!(filtered.clusters[0].single_photo_id, Some(liked.photo_id));
    assert_eq!(
        filtered.clusters[0].single_representation_id,
        Some(liked.representation_id)
    );
    assert_eq!(
        filtered.clusters[0].single_source_display_path,
        "/map/liked.nef"
    );
}

#[test]
fn map_snapshot_clusters_across_the_antimeridian() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    for (path, longitude_e7) in [
        ("/map/east.nef", 1_799_000_000),
        ("/map/west.nef", -1_799_000_000),
        ("/map/outside.nef", 0),
    ] {
        let asset = register(&mut catalog, path);
        let mut facts = facts_for(asset, Some(1_700_000_000), "Nikon", "Z 8");
        facts.latitude_e7 = Some(100_000);
        facts.longitude_e7 = Some(longitude_e7);
        catalog
            .upsert_photo_library_facts(&facts)
            .expect("index antimeridian fixture");
    }

    let snapshot = catalog
        .library_map_snapshot(
            &LibraryPhotoFilter::default(),
            LibraryMapViewport {
                south_latitude_e7: -10_000_000,
                west_longitude_e7: 1_700_000_000,
                north_latitude_e7: 10_000_000,
                east_longitude_e7: -1_700_000_000,
            },
            LibraryMapGrid {
                columns: 1,
                rows: 1,
            },
        )
        .expect("read crossing viewport");
    assert_eq!(snapshot.photo_count, 2);
    assert_eq!(snapshot.clusters.len(), 1);
    assert_eq!(snapshot.clusters[0].photo_count, 2);
    assert!(snapshot.clusters[0].longitude_e7.abs() >= 1_790_000_000);
    assert_eq!(snapshot.clusters[0].single_photo_id, None);
    assert!(snapshot.clusters[0].single_source_display_path.is_empty());
}

#[test]
fn map_snapshot_rejects_unbounded_grid_requests() {
    let catalog = Catalog::open_in_memory().expect("open catalog");
    let error = catalog
        .library_map_snapshot(
            &LibraryPhotoFilter::default(),
            world(),
            LibraryMapGrid {
                columns: 128,
                rows: 128,
            },
        )
        .expect_err("oversized map grid must fail");
    assert!(error.to_string().contains("4096"));
}
