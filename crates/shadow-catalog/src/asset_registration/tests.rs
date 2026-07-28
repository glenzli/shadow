use super::*;
use shadow_domain::Platform;

fn request(byte_len: u64, modified_at_ms: Option<i64>) -> RegisterAsset {
    RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(
            Platform::MacOs,
            b"/photos/DSC_0001.NEF".to_vec(),
            "/photos/DSC_0001.NEF",
        ),
        byte_len,
        modified_at_ms,
        now_ms: 1_700_000_000_000,
    }
}

#[test]
fn registering_the_same_location_is_idempotent() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let first = catalog
        .register_asset(&request(42, Some(100)))
        .expect("first registration");
    let second = catalog
        .register_asset(&request(42, Some(100)))
        .expect("second registration");

    assert_eq!(first.status, RegistrationStatus::Inserted);
    assert_eq!(second.status, RegistrationStatus::Unchanged);
    assert_eq!(first.photo_id, second.photo_id);
    assert_eq!(catalog.stats().expect("stats").photos, 1);
}

#[test]
fn changed_file_is_marked_for_revalidation_without_silent_replacement() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    catalog
        .register_asset(&request(42, Some(100)))
        .expect("initial registration");
    let changed = catalog
        .register_asset(&request(84, Some(200)))
        .expect("changed registration");

    assert_eq!(changed.status, RegistrationStatus::NeedsRevalidation);
    assert_eq!(
        catalog
            .stats()
            .expect("stats")
            .locations_needing_revalidation,
        1
    );
}
