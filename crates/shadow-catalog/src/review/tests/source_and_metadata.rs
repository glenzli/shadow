use shadow_domain::{AssetLocation, EntityId, Platform, RepresentationKind};

use crate::review::projection::decode_review_metadata;
use crate::{Catalog, RegisterAsset};

#[test]
fn review_metadata_survives_unrelated_decoder_snapshot_schema_changes() {
    let json = r#"{
        "provider":{"id":"legacy-provider"},
        "metadata":{
            "make":"Canon","model":"EOS R",
            "normalized_make":"Canon","normalized_model":"EOS R",
            "dng_version":null,"raw_count":1,
            "raw_dimensions":{"width":6888,"height":4546},
            "image_dimensions":{"width":6742,"height":4498},
            "margins":{"left":146,"top":48,"right":0,"bottom":0},
            "orientation":0,"cfa_pattern":"RGGB","sensor_colors":3,
            "sensor_bits":14,"black_level":0,"white_level":16383,
            "as_shot_neutral":[0.0,0.0,0.0,0.0],
            "baseline_exposure":-999.0,"iso_speed":100.0,
            "exposure_time_seconds":1.6,"aperture_f_number":9.0,
            "focal_length_mm":50.0,"captured_at_unix_seconds":1551392920,
            "lens_make":"","lens_model":"RF50mm F1.2 L USM",
            "focal_length_35mm":0.0
        },
        "capabilities":{"removed_legacy_shape":"must not affect EXIF"},
        "previews":"also deliberately incompatible"
    }"#;

    let metadata = decode_review_metadata(json).expect("decode metadata-only envelope");
    assert_eq!(metadata.make, "Canon");
    assert_eq!(metadata.model, "EOS R");
    assert_eq!(metadata.sensor_bits, 14);
    assert_eq!(metadata.raw_dimensions.width, 6_888);
    assert_eq!(metadata.lens_model, "RF50mm F1.2 L USM");
}

#[test]
#[allow(clippy::too_many_lines)]
fn review_page_includes_original_rasters_while_raw_edit_source_stays_raw_only() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let raw = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/source.nef".to_vec(),
                "/photos/source.nef",
            ),
            byte_len: 4_096,
            modified_at_ms: Some(123),
            now_ms: 100,
        })
        .expect("register RAW review source");
    let raster = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaster,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/source.jpg".to_vec(),
                "/photos/source.jpg",
            ),
            byte_len: 2_048,
            modified_at_ms: Some(124),
            now_ms: 101,
        })
        .expect("register raster review source");

    let page = catalog.review_page(None, 16).expect("read Review page");
    assert_eq!(page.total_items, 2);
    assert!(page.items.iter().any(|item| {
        item.representation_id == raw.representation_id
            && item.location.display_path == "/photos/source.nef"
    }));
    assert!(page.items.iter().any(|item| {
        item.representation_id == raster.representation_id
            && item.location.display_path == "/photos/source.jpg"
    }));

    assert_eq!(
        catalog
            .review_source(raw.photo_id)
            .expect("read RAW edit source")
            .expect("RAW edit source exists")
            .representation_id,
        raw.representation_id
    );
    assert!(
        catalog
            .review_source(raster.photo_id)
            .expect("read raster edit source")
            .is_none(),
        "a raster must not be handed to the current RAW-only editor"
    );
    assert_eq!(
        catalog
            .photo_source(raster.photo_id)
            .expect("read source-neutral raster fallback")
            .expect("original raster is an editable source")
            .representation_id,
        raster.representation_id
    );

    // A photo can own multiple source representations. Production code
    // creates that relationship through import/linking work; the query
    // contract itself is verified directly here.
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
    assert_eq!(
        catalog
            .photo_source(raw.photo_id)
            .expect("read RAW-preferred source")
            .expect("online source")
            .representation_id,
        raw.representation_id,
        "an online RAW remains the first choice when both representations exist"
    );

    catalog
        .connection
        .execute(
            "UPDATE locations SET status = 'offline' WHERE representation_id = ?1",
            [raw.representation_id.as_bytes().as_slice()],
        )
        .expect("mark RAW source offline");
    assert_eq!(
        catalog
            .photo_source(raw.photo_id)
            .expect("read raster fallback after RAW is offline")
            .expect("online raster fallback")
            .representation_id,
        raster.representation_id
    );
    assert!(
        catalog
            .review_source(raw.photo_id)
            .expect("read RAW-only source after RAW is offline")
            .is_none(),
        "the legacy RAW-only query must not silently become source-neutral"
    );
}
