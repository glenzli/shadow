use shadow_catalog::{CatalogActor, RegisterAsset};
use shadow_domain::{
    AssetLocation, EntityId, PhotoId, Platform, RepresentationId, RepresentationKind,
};

use super::PhotoInspectionService;

#[test]
fn service_preserves_exact_identity_and_reports_normal_absence() {
    let root = fixture_root("exact");
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let registered = actor
        .handle()
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaster,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/selected.jpg".to_vec(),
                "/photos/selected.jpg",
            ),
            byte_len: 1_024,
            modified_at_ms: Some(55),
            now_ms: 100,
        })
        .expect("register exact photo");
    let service = PhotoInspectionService::new(actor.handle());

    let available = service
        .inspect(
            &registered.photo_id.to_string(),
            &registered.representation_id.to_string(),
        )
        .expect("inspect exact pair");
    assert!(available.available);
    assert_eq!(available.photo_id, registered.photo_id.to_string());
    assert_eq!(
        available.representation_id,
        registered.representation_id.to_string()
    );
    assert_eq!(available.source_path, "/photos/selected.jpg");
    assert_eq!(available.source_byte_len, 1_024);
    assert!(available.has_source_modified_at);
    assert_eq!(available.source_modified_at_ms, 55);

    let absent = service
        .inspect(
            &PhotoId::new_v7().to_string(),
            &registered.representation_id.to_string(),
        )
        .expect("mismatched pair is normal absence");
    assert!(!absent.available);
    assert_eq!(
        absent.representation_id,
        registered.representation_id.to_string()
    );
    actor.shutdown().expect("shutdown catalog actor");
    std::fs::remove_dir_all(root).expect("remove exact inspection fixture");
}

#[test]
fn service_rejects_malformed_identity_before_querying_catalog() {
    let root = fixture_root("invalid");
    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let service = PhotoInspectionService::new(actor.handle());

    assert!(
        service
            .inspect("not-a-photo-id", &RepresentationId::new_v7().to_string())
            .expect_err("malformed photo id must fail")
            .to_string()
            .contains("parse selected photo id")
    );
    assert!(
        service
            .inspect(&PhotoId::new_v7().to_string(), "not-a-representation-id")
            .expect_err("malformed representation id must fail")
            .to_string()
            .contains("parse selected representation id")
    );
    actor.shutdown().expect("shutdown catalog actor");
    std::fs::remove_dir_all(root).expect("remove invalid inspection fixture");
}

fn fixture_root(label: &str) -> std::path::PathBuf {
    let root = std::env::temp_dir().join(format!(
        "shadow-photo-inspection-{label}-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create photo-inspection fixture");
    root
}
