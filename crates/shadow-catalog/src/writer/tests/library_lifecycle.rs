use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{CatalogActor, RegisterAsset};

#[test]
fn actor_archives_a_library_photo_once() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/archive/missing.nef".to_vec(),
                "/archive/missing.nef",
            ),
            byte_len: 42,
            modified_at_ms: Some(7),
            now_ms: 9,
        })
        .expect("register source");

    assert!(
        handle
            .archive_library_photo(registered.photo_id)
            .expect("archive photo")
    );
    assert!(
        !handle
            .archive_library_photo(registered.photo_id)
            .expect("repeat archive")
    );
    actor.shutdown().expect("shutdown actor");
}
