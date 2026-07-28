use std::thread;

use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{RegisterAsset, writer::CatalogActor};

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
