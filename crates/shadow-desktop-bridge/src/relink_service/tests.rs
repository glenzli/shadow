use shadow_catalog::{
    CatalogActor, ContentIdentity, RecordRepresentationContentIdentity,
    RecordRepresentationContentIdentityStatus, RegisterAsset, RepresentationFingerprint,
};
use shadow_core::native_location;
use shadow_domain::RepresentationKind;
use uuid::Uuid;

use super::{RelinkService, system_time_ms};

#[test]
fn unavailable_library_location_relinks_without_completed_scan_evidence() {
    let root = std::env::temp_dir().join(format!(
        "shadow-library-card-relink-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    std::fs::create_dir_all(&root).expect("create relink fixture");
    let original_path = root.join("original.nef");
    let candidate_path = root.join("moved.nef");
    let bytes = b"stable exact source identity";
    std::fs::write(&original_path, bytes).expect("write original source");
    std::fs::write(&candidate_path, bytes).expect("write moved source");
    let metadata = std::fs::metadata(&original_path).expect("inspect original source");
    let fingerprint = RepresentationFingerprint {
        byte_len: metadata.len(),
        modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
    };

    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&original_path),
            byte_len: fingerprint.byte_len,
            modified_at_ms: fingerprint.modified_at_ms,
            now_ms: 1,
        })
        .expect("register original source");
    let identity = ContentIdentity::whole_file_blake3(*blake3::hash(bytes).as_bytes());
    assert_eq!(
        handle
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: registered.representation_id,
                expected_source: fingerprint,
                identity,
                observed_at_ms: 2,
            })
            .expect("record exact source identity"),
        RecordRepresentationContentIdentityStatus::Recorded
    );
    std::fs::remove_file(&original_path).expect("disconnect original source");

    let receipt = RelinkService::new(handle)
        .relink_library_source_location(
            &registered.location_id.to_string(),
            candidate_path.to_str().expect("candidate path"),
        )
        .expect("relink directly from Library card");
    assert_eq!(receipt.photo_id, registered.photo_id.to_string());
    assert_eq!(
        receipt.representation_id,
        registered.representation_id.to_string()
    );
    assert_eq!(
        receipt.display_path,
        candidate_path
            .canonicalize()
            .expect("canonical candidate")
            .to_string_lossy()
    );

    actor.shutdown().expect("shutdown actor");
    std::fs::remove_dir_all(root).expect("remove relink fixture");
}
