use shadow_domain::{AssetLocation, EntityId, Platform, RepresentationKind};

use crate::{Catalog, ImportPhotoGrouping, RegisterAsset};

#[test]
fn representation_inventory_keeps_raw_first_and_counts_its_locations() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let root = AssetLocation::new(Platform::MacOs, b"/photos".to_vec(), "/photos");
    let session = catalog
        .begin_import_session(&root, 1)
        .expect("begin import");
    let group = ImportPhotoGrouping::same_directory_stem("img_0001").expect("group key");
    let jpeg = request("/photos/IMG_0001.JPG", RepresentationKind::OriginalRaster);
    let raw = request("/photos/IMG_0001.NEF", RepresentationKind::OriginalRaw);
    for request in [&jpeg, &raw] {
        catalog
            .record_import_discovered(session, request)
            .expect("record discovery");
    }
    let jpeg = catalog
        .register_import_asset_grouped(session, &jpeg, &group)
        .expect("register JPEG");
    let raw = catalog
        .register_import_asset_grouped(session, &raw, &group)
        .expect("register RAW");
    catalog
        .connection
        .execute(
            "INSERT INTO locations(
                 id, representation_id, platform, native_path, display_path, sort_name_key,
                 status, created_at_ms
             ) VALUES (?1, ?2, 'macos', ?3, ?4, ?5, 'offline', 3)",
            rusqlite::params![
                shadow_domain::LocationId::new_v7().as_bytes().as_slice(),
                raw.representation_id.as_bytes().as_slice(),
                b"/backup/IMG_0001.NEF".as_slice(),
                "/backup/IMG_0001.NEF",
                "img_0001.nef",
            ],
        )
        .expect("add offline RAW location");

    let inventory = catalog
        .photo_representations(raw.photo_id)
        .expect("representation inventory");
    assert_eq!(inventory.len(), 2);
    assert_eq!(inventory[0].representation_id, raw.representation_id);
    assert_eq!(inventory[0].kind, RepresentationKind::OriginalRaw);
    assert_eq!(inventory[0].location_count, 2);
    assert_eq!(inventory[0].online_location_count, 1);
    assert_eq!(inventory[1].representation_id, jpeg.representation_id);
}

fn request(path: &str, kind: RepresentationKind) -> RegisterAsset {
    RegisterAsset {
        kind,
        location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
        byte_len: 42,
        modified_at_ms: Some(1),
        now_ms: 2,
    }
}
