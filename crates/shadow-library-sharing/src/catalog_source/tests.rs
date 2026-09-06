use std::{fs, path::PathBuf};

use shadow_catalog::{
    CachedArtifact, CachedArtifactRecord, CachedArtifactRole, Catalog, ImportPhotoGrouping,
    RecordDecodeSnapshot, RecordDecodeSnapshotStatus, RegisterAsset,
};
use shadow_domain::{
    AssetLocation, DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport,
    DecoderSnapshot, EntityId, GpsMetadataSnapshot, ImageDimensions, ImageMargins,
    PendingCorrectionsSnapshot, Platform, PreviewByteOrder, PreviewCodec,
    RawDevelopmentCapabilitySnapshot, RawMetadataSnapshot, RepresentationKind,
};
use shadow_native_path::{current_platform, native_location};

use super::{CatalogSharePolicy, CatalogShareSource, native_path, remote_preview_manifest};
use crate::{
    LibraryShareSource,
    protocol::{
        CapabilityAvailability, REMOTE_PHOTO_METADATA_SCHEMA_VERSION, RemoteOriginalIdentity,
        RemotePreviewPixelOrientation,
    },
};

#[test]
fn manifest_projects_bounded_orientation_location_and_display_metadata() {
    let root = temporary_directory("metadata-projection");
    let shared = root.join("shared");
    fs::create_dir_all(&shared).expect("create shared root");
    let path = shared.join("metadata.nef");
    fs::write(&path, b"remote metadata fixture").expect("write fixture");
    let catalog_path = root.join("catalog.sqlite");
    let mut catalog = Catalog::open(&catalog_path).expect("open catalog");
    let registered = catalog
        .register_asset(&registration(
            &path,
            fs::metadata(&path).expect("metadata").len(),
        ))
        .expect("register fixture");
    let source = catalog
        .representation_fingerprint(registered.representation_id)
        .expect("source fingerprint");
    let snapshot = metadata_snapshot();
    assert_eq!(
        catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id: registered.representation_id,
                expected_source: source,
                snapshot,
                inspected_at_ms: 2,
            })
            .expect("record metadata"),
        RecordDecodeSnapshotStatus::Recorded
    );
    drop(catalog);

    let source = CatalogShareSource::open_with_policy(
        &catalog_path,
        root.join("cache"),
        root.join("state"),
        "Studio",
        false,
        CatalogSharePolicy::for_roots(vec![shared], false),
    )
    .expect("open source");
    let page = source.list_photos(None, 96).expect("list photos");
    let metadata = &page.items[0].metadata;
    assert_eq!(
        metadata.schema_version,
        REMOTE_PHOTO_METADATA_SCHEMA_VERSION
    );
    assert_eq!(metadata.orientation, Some(6));
    assert_eq!(
        metadata.image_dimensions,
        Some(ImageDimensions {
            width: 3_000,
            height: 4_000
        })
    );
    assert_eq!(metadata.focal_length_35mm, Some(52.0));
    assert_eq!(
        metadata.gps,
        Some(GpsMetadataSnapshot {
            latitude_degrees: 31.2304,
            longitude_degrees: 121.4737,
            altitude_meters: Some(18.5),
        })
    );
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn shared_preview_declares_whether_pixels_are_already_display_oriented() {
    let representation_id = shadow_domain::RepresentationId::new_v7();
    let source = shadow_catalog::RepresentationFingerprint {
        byte_len: 10,
        modified_at_ms: Some(2),
    };
    let record = |role| CachedArtifactRecord {
        representation_id,
        source,
        artifact: CachedArtifact {
            role,
            variant_key: "fixture".into(),
            generator_id: "fixture".into(),
            generator_version: "1".into(),
            recipe_snapshot_digest: None,
            provider_preview_id: None,
            blob_algorithm: "blake3-256".into(),
            blob_digest: [1; 32],
            blob_byte_len: 10,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 40,
                height: 30,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 3,
        },
    };
    assert_eq!(
        remote_preview_manifest(&record(CachedArtifactRole::EmbeddedPreview)).pixel_orientation,
        RemotePreviewPixelOrientation::EncodedMetadata
    );
    assert_eq!(
        remote_preview_manifest(&record(CachedArtifactRole::GeneratedProxy)).pixel_orientation,
        RemotePreviewPixelOrientation::DisplayOriented
    );
}

#[test]
fn server_identity_is_stable_across_source_reopen() {
    let root = temporary_directory("server-identity");
    let catalog_path = root.join("catalog.sqlite");
    Catalog::open(&catalog_path).expect("create catalog");
    let cache = root.join("cache");
    let state = root.join("state");
    let first = CatalogShareSource::open(&catalog_path, &cache, &state, "Studio", false)
        .expect("open first source");
    let first_id = first.server_info().server_id;
    drop(first);
    let second = CatalogShareSource::open(&catalog_path, &cache, &state, "Studio", false)
        .expect("reopen source");
    assert_eq!(second.server_info().server_id, first_id);
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn explicit_roots_and_original_permission_bound_the_manifest() {
    let root = temporary_directory("root-policy");
    let shared = root.join("shared");
    let private = root.join("private");
    fs::create_dir_all(&shared).expect("create shared root");
    fs::create_dir_all(&private).expect("create private root");
    let shared_photo = shared.join("shared.nef");
    let private_photo = private.join("private.nef");
    fs::write(&shared_photo, b"shared").expect("write shared photo");
    fs::write(&private_photo, b"private").expect("write private photo");
    let catalog_path = root.join("catalog.sqlite");
    let mut catalog = Catalog::open(&catalog_path).expect("create catalog");
    let shared_asset = catalog
        .register_asset(&registration(&shared_photo, 6))
        .expect("register shared photo");
    catalog
        .register_asset(&registration(&private_photo, 7))
        .expect("register private photo");
    let source = CatalogShareSource::open_with_policy(
        &catalog_path,
        root.join("cache"),
        root.join("state"),
        "Studio",
        false,
        CatalogSharePolicy::for_roots(vec![shared.clone()], false),
    )
    .expect("open constrained source");
    assert_eq!(
        source.server_info().capabilities.serves_originals,
        CapabilityAvailability::Unavailable
    );
    let page = source.list_photos(None, 96).expect("list shared photos");
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.items[0].photo_id, shared_asset.photo_id);
    assert!(
        source
            .prepare_original(shared_asset.photo_id, shared_asset.representation_id)
            .is_err()
    );
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn a_foreign_platform_location_is_never_reopened_as_host_bytes() {
    let foreign = match current_platform() {
        Platform::Windows => Platform::MacOs,
        Platform::MacOs | Platform::OtherUnix => Platform::Windows,
    };
    let location = AssetLocation::new(foreign, b"foreign-native-path".to_vec(), "foreign");

    let error = native_path(&location).expect_err("reject foreign native path");
    assert_eq!(error.code, crate::protocol::RemoteErrorCode::Unavailable);
}

#[test]
fn manifest_lists_companion_representations_and_publishes_a_prepared_raw_identity() {
    let root = temporary_directory("logical-photo-manifest");
    let shared = root.join("shared");
    fs::create_dir_all(&shared).expect("create shared root");
    let raw_path = shared.join("IMG_0001.NEF");
    let jpeg_path = shared.join("IMG_0001.JPG");
    fs::write(&raw_path, b"raw original").expect("write RAW");
    fs::write(&jpeg_path, b"camera jpeg").expect("write JPEG");

    let catalog_path = root.join("catalog.sqlite");
    let mut catalog = Catalog::open(&catalog_path).expect("create catalog");
    let source_root = location(&shared);
    let session = catalog
        .begin_import_session(&source_root, 1)
        .expect("begin import");
    let group = ImportPhotoGrouping::same_directory_stem("img_0001").expect("group key");
    let raw_request = registration_from_file(&raw_path, RepresentationKind::OriginalRaw);
    let jpeg_request = registration_from_file(&jpeg_path, RepresentationKind::OriginalRaster);
    for request in [&jpeg_request, &raw_request] {
        catalog
            .record_import_discovered(session, request)
            .expect("record discovery");
    }
    let jpeg = catalog
        .register_import_asset_grouped(session, &jpeg_request, &group)
        .expect("register JPEG");
    let raw = catalog
        .register_import_asset_grouped(session, &raw_request, &group)
        .expect("register RAW");
    assert_eq!(jpeg.photo_id, raw.photo_id);
    drop(catalog);

    let source = CatalogShareSource::open_with_policy(
        &catalog_path,
        root.join("cache"),
        root.join("state"),
        "Studio",
        false,
        CatalogSharePolicy::for_roots(vec![shared.clone()], true),
    )
    .expect("open source");
    let initial = source.list_photos(None, 96).expect("list logical photo");
    assert_eq!(initial.items.len(), 1);
    assert_eq!(initial.items[0].representation_id, raw.representation_id);
    assert_eq!(initial.items[0].representations.len(), 2);
    assert!(
        initial.items[0]
            .representations
            .iter()
            .all(|representation| {
                representation.original_identity == RemoteOriginalIdentity::NotPrepared
            })
    );

    let prepared = source
        .prepare_original(raw.photo_id, raw.representation_id)
        .expect("prepare RAW");
    let refreshed = source.list_photos(None, 96).expect("refresh manifest");
    assert_eq!(
        refreshed.items[0].preferred_original_digest(),
        Some(prepared.digest_blake3)
    );
    fs::remove_dir_all(root).expect("remove fixture");
}

#[cfg(unix)]
#[test]
fn shared_root_symlink_cannot_admit_an_outside_photo() {
    use std::os::unix::fs::symlink;

    let root = temporary_directory("root-symlink-policy");
    let shared = root.join("shared");
    let private = root.join("private");
    fs::create_dir_all(&shared).expect("create shared root");
    fs::create_dir_all(&private).expect("create private root");
    let private_photo = private.join("private.nef");
    fs::write(&private_photo, b"private").expect("write private photo");
    let shared_link = shared.join("linked-private.nef");
    symlink(&private_photo, &shared_link).expect("link outside photo into shared root");
    let catalog_path = root.join("catalog.sqlite");
    let mut catalog = Catalog::open(&catalog_path).expect("create catalog");
    catalog
        .register_asset(&registration(&shared_link, 7))
        .expect("register linked photo");
    let source = CatalogShareSource::open_with_policy(
        &catalog_path,
        root.join("cache"),
        root.join("state"),
        "Studio",
        false,
        CatalogSharePolicy::for_roots(vec![shared.canonicalize().expect("canonical shared")], true),
    )
    .expect("open constrained source");
    assert!(
        source
            .list_photos(None, 96)
            .expect("list shared photos")
            .items
            .is_empty()
    );
    fs::remove_dir_all(root).expect("remove fixture");
}

fn registration(path: &std::path::Path, byte_len: u64) -> RegisterAsset {
    RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: native_location(path),
        byte_len,
        modified_at_ms: Some(1),
        now_ms: 1,
    }
}

fn registration_from_file(path: &std::path::Path, kind: RepresentationKind) -> RegisterAsset {
    let metadata = path.metadata().expect("read fixture metadata");
    RegisterAsset {
        kind,
        location: location(path),
        byte_len: metadata.len(),
        modified_at_ms: metadata.modified().ok().and_then(|modified| {
            modified
                .duration_since(std::time::UNIX_EPOCH)
                .ok()
                .and_then(|duration| i64::try_from(duration.as_millis()).ok())
        }),
        now_ms: 1,
    }
}

fn metadata_snapshot() -> DecoderSnapshot {
    DecoderSnapshot {
        provider: DecodeProviderSnapshot {
            id: "fixture".into(),
            version: "1".into(),
            dng_sdk: false,
            rawspeed: false,
            jpeg: true,
        },
        metadata: RawMetadataSnapshot {
            make: "Fixture".into(),
            model: "Camera".into(),
            normalized_make: "Fixture".into(),
            normalized_model: "Camera".into(),
            dng_version: None,
            raw_count: 1,
            raw_dimensions: ImageDimensions {
                width: 4_000,
                height: 3_000,
            },
            image_dimensions: ImageDimensions {
                width: 3_000,
                height: 4_000,
            },
            margins: ImageMargins::default(),
            orientation: 6,
            cfa_pattern: "RGGB".into(),
            sensor_colors: 3,
            sensor_bits: 14,
            black_level: 0,
            white_level: 16_383,
            as_shot_neutral: [1.0, 1.0, 1.0, 0.0],
            baseline_exposure: 0.0,
            iso_speed: 100.0,
            exposure_time_seconds: 1.0 / 125.0,
            aperture_f_number: 4.0,
            focal_length_mm: 35.0,
            focus_observation: None,
            captured_at_unix_seconds: 1_700_000_000,
            gps: Some(GpsMetadataSnapshot {
                latitude_degrees: 31.2304,
                longitude_degrees: 121.4737,
                altitude_meters: Some(18.5),
            }),
            lens_make: "Fixture".into(),
            lens_model: "35mm".into(),
            focal_length_35mm: 52.0,
        },
        capabilities: DecodeCapabilitySnapshot {
            metadata: DecodeSupport::Available,
            embedded_previews: DecodeSupport::Unavailable,
            raw_frame: DecodeSupport::Available,
            reference_rgb: DecodeSupport::Unavailable,
            pending_corrections: PendingCorrectionsSnapshot::default(),
            raw_development: RawDevelopmentCapabilitySnapshot::default(),
        },
        previews: Vec::new(),
    }
}

fn location(path: &std::path::Path) -> AssetLocation {
    native_location(path)
}

fn temporary_directory(label: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!(
        "shadow-library-sharing-{label}-{}",
        uuid::Uuid::now_v7()
    ));
    fs::create_dir_all(&path).expect("create fixture directory");
    path
}
