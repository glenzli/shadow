use shadow_domain::{AssetLocation, EntityId, Platform, RepresentationKind};

use crate::{Catalog, RegisterAsset, TechnicalObservationRevision};

#[test]
fn photo_inspection_resolves_only_the_requested_owned_representation() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let raw = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/exact.nef".to_vec(),
                "/photos/exact.nef",
            ),
            byte_len: 4_096,
            modified_at_ms: Some(100),
            now_ms: 100,
        })
        .expect("register RAW");
    let raster = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaster,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/exact.jpg".to_vec(),
                "/photos/exact.jpg",
            ),
            byte_len: 2_048,
            modified_at_ms: Some(101),
            now_ms: 101,
        })
        .expect("register raster");
    let raster_original_photo_id = raster.photo_id;
    catalog
        .connection
        .execute(
            "UPDATE representations SET photo_id = ?1 WHERE id = ?2",
            rusqlite::params![
                raw.photo_id.as_bytes().as_slice(),
                raster.representation_id.as_bytes().as_slice(),
            ],
        )
        .expect("attach raster representation to RAW photo");
    let revision = TechnicalObservationRevision::current("photo-inspection-test-v1");

    let exact_raw = catalog
        .photo_inspection(raw.photo_id, raw.representation_id, &revision)
        .expect("inspect exact RAW")
        .expect("exact RAW is online");
    assert_eq!(exact_raw.photo_id, raw.photo_id);
    assert_eq!(exact_raw.representation_id, raw.representation_id);
    assert_eq!(exact_raw.location.display_path, "/photos/exact.nef");

    let exact_raster = catalog
        .photo_inspection(raw.photo_id, raster.representation_id, &revision)
        .expect("inspect exact raster")
        .expect("exact raster is online");
    assert_eq!(exact_raster.photo_id, raw.photo_id);
    assert_eq!(exact_raster.representation_id, raster.representation_id);
    assert_eq!(exact_raster.location.display_path, "/photos/exact.jpg");

    assert!(
        catalog
            .photo_inspection(
                raster_original_photo_id,
                raster.representation_id,
                &revision,
            )
            .expect("query mismatched pair")
            .is_none(),
        "a representation must never leak through a mismatched photo identity"
    );

    catalog
        .connection
        .execute(
            "UPDATE locations SET status = 'offline' WHERE representation_id = ?1",
            [raster.representation_id.as_bytes().as_slice()],
        )
        .expect("mark requested raster offline");
    assert!(
        catalog
            .photo_inspection(raw.photo_id, raster.representation_id, &revision)
            .expect("query offline exact representation")
            .is_none(),
        "an offline exact selection must not fall back to the online RAW"
    );
}
