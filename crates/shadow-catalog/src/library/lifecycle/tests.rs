use shadow_domain::{AssetLocation, EntityId, Platform, RepresentationKind};

use crate::{Catalog, LibraryPhotoFilter, LibraryPhotoOrder, RegisterAsset};

#[test]
fn archiving_a_library_photo_is_non_destructive_and_idempotent() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let registered = catalog
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
        catalog
            .archive_library_photo(registered.photo_id)
            .expect("archive active photo")
    );
    assert!(
        !catalog
            .archive_library_photo(registered.photo_id)
            .expect("repeat archive")
    );
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count active Library photos"),
        0
    );

    let stored_state: String = catalog
        .connection
        .query_row(
            "SELECT lifecycle_state FROM photos WHERE id = ?1",
            [registered.photo_id.as_bytes().as_slice()],
            |row| row.get(0),
        )
        .expect("read archived photo");
    assert_eq!(stored_state, "archived");
    assert!(
        catalog
            .library_photo_page(
                &LibraryPhotoFilter::default(),
                LibraryPhotoOrder::default(),
                None,
                16,
            )
            .expect("read active Library page")
            .items
            .is_empty()
    );
}
