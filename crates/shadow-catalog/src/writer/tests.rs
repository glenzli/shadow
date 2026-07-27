use std::thread;

use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::RegisterAsset;

use super::*;

#[test]
fn cloned_handles_serialize_writes_through_one_actor() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handles = (0_u8..4)
        .map(|index| {
            let handle = actor.handle();
            thread::spawn(move || {
                let request = RegisterAsset {
                    kind: RepresentationKind::OriginalRaw,
                    location: AssetLocation::new(
                        Platform::MacOs,
                        format!("/photos/{index}.nef").into_bytes(),
                        format!("/photos/{index}.nef"),
                    ),
                    byte_len: 42,
                    modified_at_ms: Some(100),
                    now_ms: 1_700_000_000_000,
                };
                handle.register_asset(&request).expect("register asset");
            })
        })
        .collect::<Vec<_>>();

    for handle in handles {
        handle.join().expect("join client thread");
    }
    assert_eq!(actor.handle().stats().expect("stats").photos, 4);
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_resolves_an_original_raster_through_the_source_neutral_query() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaster,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/editable.jpg".to_vec(),
                "/photos/editable.jpg",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register raster photo");

    assert_eq!(
        handle
            .photo_source(registered.photo_id)
            .expect("resolve source-neutral photo source")
            .expect("online raster source")
            .representation_id,
        registered.representation_id
    );
    assert!(
        handle
            .review_source(registered.photo_id)
            .expect("resolve legacy RAW-only source")
            .is_none()
    );
    actor.shutdown().expect("shutdown actor");
}
