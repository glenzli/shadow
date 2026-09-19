use shadow_domain::{AssetLocation, EntityId, Platform, RepresentationKind};

use crate::{Catalog, RegisterAsset};

#[test]
fn review_query_pages_with_a_stable_path_and_id_cursor() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    for (path, state) in [
        ("/photos/aa.dng", "archived"),
        ("/photos/bb.dng", "trashed"),
    ] {
        let asset = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
                byte_len: 4096,
                modified_at_ms: Some(123),
                now_ms: 100,
            })
            .expect("register inactive photo");
        catalog
            .connection
            .execute(
                "UPDATE photos SET lifecycle_state = ?1 WHERE id = ?2",
                rusqlite::params![state, asset.photo_id.as_bytes().as_slice()],
            )
            .expect("set lifecycle fixture");
    }
    for path in ["/photos/c.dng", "/photos/a.dng", "/photos/b.dng"] {
        catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
                byte_len: 4_096,
                modified_at_ms: Some(123),
                now_ms: 100,
            })
            .expect("register RAW");
    }

    let first = catalog.review_page(None, 2).expect("first Review page");
    assert_eq!(first.total_items, 3);
    assert_eq!(
        first
            .items
            .iter()
            .map(|item| item.location.display_path.as_str())
            .collect::<Vec<_>>(),
        ["/photos/a.dng", "/photos/b.dng"]
    );
    let second = catalog
        .review_page(first.next_cursor.as_ref(), 2)
        .expect("second Review page");
    assert_eq!(second.total_items, 3);
    assert_eq!(second.items.len(), 1);
    assert_eq!(second.items[0].location.display_path, "/photos/c.dng");
    assert!(second.next_cursor.is_none());
}
