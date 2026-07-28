use std::{fs, path::Path, time::UNIX_EPOCH};

use shadow_catalog::{
    Catalog, ContentIdentity, RecordRepresentationContentIdentity, RegisterAsset,
    RepresentationFingerprint,
};
use shadow_domain::{AssetLocation, EntityId, LocationId, PhotoId, Platform, RepresentationKind};

use super::*;

fn fingerprint(path: &Path) -> RepresentationFingerprint {
    let metadata = fs::metadata(path).expect("read fixture metadata");
    RepresentationFingerprint {
        byte_len: metadata.len(),
        modified_at_ms: metadata.modified().ok().and_then(|time| {
            time.duration_since(UNIX_EPOCH)
                .ok()
                .and_then(|duration| i64::try_from(duration.as_millis()).ok())
        }),
    }
}

fn missing_location(
    photo_id: PhotoId,
    representation_id: shadow_domain::RepresentationId,
    source: RepresentationFingerprint,
) -> MissingSourceLocationRecord {
    MissingSourceLocationRecord {
        photo_id,
        representation_id,
        location_id: LocationId::new_v7(),
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(
            Platform::OtherUnix,
            b"/former-drive/DCIM/DSC_0001.NEF".to_vec(),
            "/former-drive/DCIM/DSC_0001.NEF",
        ),
        source,
        captured_at_unix_seconds: Some(1_700_000_000),
        camera_key: "nikon z9".to_owned(),
        last_seen_at_ms: 10,
    }
}

#[test]
fn missing_location_conversion_keeps_only_weak_presentation_evidence() {
    let source = RepresentationFingerprint {
        byte_len: 42,
        modified_at_ms: Some(7),
    };
    let location = missing_location(
        PhotoId::new_v7(),
        shadow_domain::RepresentationId::new_v7(),
        source,
    );

    let candidate = relink_candidate_from_missing_location(&location);
    assert_eq!(candidate.fingerprint, source);
    assert_eq!(
        candidate.metadata.file_name.as_deref(),
        Some("DSC_0001.NEF")
    );
    assert_eq!(candidate.metadata.camera_key.as_deref(), Some("nikon z9"));
    assert_eq!(candidate.location_label, location.location.display_path);
}

#[test]
fn unique_missing_location_can_produce_a_ready_read_only_plan() {
    let path = std::env::temp_dir().join(format!("shadow-source-health-{}.nef", PhotoId::new_v7()));
    let bytes = b"same original bytes";
    fs::write(&path, bytes).expect("write fixture");
    let source_fingerprint = fingerprint(&path);

    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let original = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::OtherUnix,
                b"/former-drive/DCIM/DSC_0001.NEF".to_vec(),
                "/former-drive/DCIM/DSC_0001.NEF",
            ),
            byte_len: source_fingerprint.byte_len,
            modified_at_ms: source_fingerprint.modified_at_ms,
            now_ms: 1,
        })
        .expect("register old location");
    catalog
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: source_fingerprint,
            identity: ContentIdentity::whole_file_blake3(*blake3::hash(bytes).as_bytes()),
            observed_at_ms: 2,
        })
        .expect("record identity");

    let source = RelinkSource {
        path: path.clone(),
        kind: RepresentationKind::OriginalRaw,
        fingerprint: source_fingerprint,
        metadata: WeakRelinkMetadata {
            file_name: Some("DSC_0001.NEF".to_owned()),
            captured_at_unix_seconds: Some(1_700_000_000),
            camera_key: Some("nikon z9".to_owned()),
        },
    };
    let plan = plan_safe_reattach(
        &catalog,
        source,
        [missing_location(
            original.photo_id,
            original.representation_id,
            source_fingerprint,
        )],
    )
    .expect("plan source reattach");

    let SafeReattachPlan::Catalog(CatalogRelinkConfirmation::Ready(confirmed)) = plan else {
        panic!("exact verified identity should be ready, not attached");
    };
    assert_eq!(
        confirmed.matched.representation_id,
        original.representation_id
    );
    assert_eq!(catalog.stats().expect("stats").locations, 1);
    fs::remove_file(path).expect("remove fixture");
}
