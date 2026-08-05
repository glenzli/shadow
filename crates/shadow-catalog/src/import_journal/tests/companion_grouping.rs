use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{Catalog, ImportPhotoGrouping, LibraryPhotoFilter, LibraryPhotoOrder, RegisterAsset};

#[test]
fn source_group_registers_raw_and_camera_jpeg_as_two_representations_of_one_photo() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let session = catalog
        .begin_import_session(
            &AssetLocation::new(Platform::MacOs, b"/photos".to_vec(), "/photos"),
            1,
        )
        .expect("begin import");
    let group =
        ImportPhotoGrouping::same_directory_stem("trip/img_0001").expect("valid companion group");
    let jpeg = request(
        "/photos/trip/IMG_0001.JPG",
        RepresentationKind::OriginalRaster,
        2,
    );
    let raw = request(
        "/photos/trip/IMG_0001.NEF",
        RepresentationKind::OriginalRaw,
        3,
    );

    for asset in [&jpeg, &raw] {
        catalog
            .record_import_discovered(session, asset)
            .expect("record discovery");
    }
    let jpeg = catalog
        .register_import_asset_grouped(session, &jpeg, &group)
        .expect("register JPEG first");
    let raw = catalog
        .register_import_asset_grouped(session, &raw, &group)
        .expect("attach RAW representation");

    assert_eq!(jpeg.photo_id, raw.photo_id);
    assert_ne!(jpeg.representation_id, raw.representation_id);
    let stats = catalog.stats().expect("catalog stats");
    assert_eq!(stats.photos, 1);
    assert_eq!(stats.representations, 2);
    assert_eq!(stats.locations, 2);
    let page = catalog
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            LibraryPhotoOrder::FileNameAscending,
            None,
            10,
        )
        .expect("logical photo page");
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.items[0].representation_count, 2);
    assert_eq!(page.items[0].source_location_count, 2);
    assert!(page.items[0].has_raw_representation);
    assert!(page.items[0].has_raster_representation);
    assert_eq!(
        catalog
            .photo_source(raw.photo_id)
            .expect("photo source")
            .expect("online source")
            .representation_id,
        raw.representation_id,
        "RAW must become the preferred representation even when JPEG arrived first"
    );
}

fn request(path: &str, kind: RepresentationKind, now_ms: i64) -> RegisterAsset {
    RegisterAsset {
        kind,
        location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
        byte_len: 100 + u64::try_from(now_ms).expect("positive fixture time"),
        modified_at_ms: Some(now_ms),
        now_ms,
    }
}
