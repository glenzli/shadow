use std::{
    fs,
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
};

use shadow_catalog::{Catalog, CatalogActor, RegisterAsset};
use shadow_core::{fingerprint_source, native_location};
use shadow_domain::{
    DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot, EntityId,
    GpsMetadataSnapshot, ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, PhotoFlag,
    PreviewCodec, RawDevelopmentCapabilitySnapshot, RawMetadataSnapshot, RepresentationKind,
};
use shadow_library_sharing::{
    CachedRemotePreview, MirroredLocalSource, RemoteLibraryMirrorSnapshot, RemotePhotoMirror,
    RemoteReviewFlag, RemoteReviewState,
    protocol::{
        CapabilityAvailability, LIBRARY_PROTOCOL_REVISION, PreviewUnavailableReason,
        RemoteOriginalIdentity, RemotePhotoManifest, RemotePhotoMetadata,
        RemotePreviewAvailability, RemotePreviewManifest, RemotePreviewPixelOrientation,
        RemotePreviewRole, RemoteRepresentationManifest, ServerCapabilities, ServerId, ServerInfo,
    },
};

use super::{
    RemoteLibraryService, cached_original_matches, cached_preview_path, ensure_decode_inspection,
};

const CONNECTION_A: &str = "019fb225-9a01-7301-a64b-c0168f92b834";
const CONNECTION_B: &str = "019fb225-9a01-7301-a64b-c0168f92b835";

#[test]
fn materialized_metadata_inspection_is_current_without_waiting_for_a_proxy() {
    let root = temporary_directory("metadata-only-inspection");
    let catalog_path = root.join("catalog.sqlite");
    let source_path = root.join("materialized.nef");
    fs::write(&source_path, b"materialized metadata source").expect("write source");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let source = fingerprint_source(&source_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&source_path),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 1,
        })
        .expect("register source");
    let inspections = Arc::new(AtomicUsize::new(0));
    let inspect_once = {
        let inspections = inspections.clone();
        move |_path: &std::path::Path| {
            inspections.fetch_add(1, Ordering::Relaxed);
            Ok(metadata_snapshot())
        }
    };
    let first = ensure_decode_inspection(
        &catalog,
        registered.representation_id,
        &source_path,
        source,
        inspect_once,
    )
    .expect("inspect metadata");
    assert_eq!(first.orientation, Some(6));
    assert_eq!(first.focal_length_35mm, Some(52.0));
    assert_eq!(
        first.gps.as_ref().map(|gps| gps.altitude_meters),
        Some(Some(18.5))
    );
    assert!(
        catalog
            .cached_artifacts(registered.representation_id)
            .expect("read artifacts")
            .is_empty(),
        "metadata admission must not wait for or persist a generated proxy"
    );

    let inspect_if_stale = {
        let inspections = inspections.clone();
        move |_path: &std::path::Path| {
            inspections.fetch_add(1, Ordering::Relaxed);
            Ok(metadata_snapshot())
        }
    };
    ensure_decode_inspection(
        &catalog,
        registered.representation_id,
        &source_path,
        source,
        inspect_if_stale,
    )
    .expect("reuse current metadata");
    assert_eq!(inspections.load(Ordering::Relaxed), 1);

    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn cached_original_remains_materializable_when_local_metadata_inspection_is_unsupported() {
    let root = temporary_directory("unsupported-cached-inspection");
    let catalog_path = root.join("catalog.sqlite");
    let cache_root = root.join("cache");
    let source_path = root.join("unsupported.nef");
    fs::write(&source_path, b"verified original without a local decoder")
        .expect("write unsupported source");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let source = fingerprint_source(&source_path).expect("fingerprint source");
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&source_path),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 1,
        })
        .expect("register cached source");
    let remote_photo_id = shadow_domain::PhotoId::new_v7();
    let remote_representation_id = shadow_domain::RepresentationId::new_v7();
    let digest = *blake3::hash(b"verified original without a local decoder").as_bytes();
    let fallback_metadata = RemotePhotoMetadata {
        orientation: Some(8),
        gps: Some(GpsMetadataSnapshot {
            latitude_degrees: 35.0,
            longitude_degrees: 139.0,
            altitude_meters: None,
        }),
        ..RemotePhotoMetadata::default()
    };
    let manifest = RemotePhotoManifest {
        photo_id: remote_photo_id,
        representation_id: remote_representation_id,
        display_name: "unsupported.nef".into(),
        source_byte_len: source.byte_len,
        source_modified_at_ms: source.modified_at_ms,
        metadata: fallback_metadata.clone(),
        preview: RemotePreviewAvailability::Unavailable {
            reason: PreviewUnavailableReason::DecoderCapabilityMissing,
        },
        representations: vec![RemoteRepresentationManifest {
            representation_id: remote_representation_id,
            kind: RepresentationKind::OriginalRaw,
            display_name: "unsupported.nef".into(),
            source_byte_len: source.byte_len,
            source_modified_at_ms: source.modified_at_ms,
            location_count: 1,
            online_location_count: 1,
            original_identity: RemoteOriginalIdentity::Available {
                digest_blake3: digest,
            },
        }],
    };
    let snapshot = RemoteLibraryMirrorSnapshot {
        server: Some(ServerInfo {
            protocol_revision: LIBRARY_PROTOCOL_REVISION,
            server_id: ServerId(uuid::Uuid::now_v7()),
            display_name: "Fixture server".into(),
            capabilities: ServerCapabilities {
                serves_embedded_previews: CapabilityAvailability::Unavailable,
                serves_generated_proxies: CapabilityAvailability::Unavailable,
                serves_originals: CapabilityAvailability::Available,
                private_preview_provider: CapabilityAvailability::Unavailable,
                maximum_page_size: 32,
                maximum_original_chunk_bytes: 1_024,
            },
        }),
        photos: vec![RemotePhotoMirror {
            manifest: manifest.clone(),
            cached_preview: None,
            review_state: RemoteReviewState::default(),
            local_source: Some(MirroredLocalSource::for_remote_manifest(
                registered.photo_id,
                registered.representation_id,
                digest,
                source_path.to_string_lossy().into_owned(),
                &manifest,
            )),
        }],
    };
    let mirror_root = root.join("remote-library/connections").join(CONNECTION_A);
    fs::create_dir_all(&mirror_root).expect("create mirror root");
    fs::write(
        mirror_root.join("remote-library.json"),
        serde_json::to_vec_pretty(&snapshot).expect("serialize mirror"),
    )
    .expect("write mirror");
    let service = RemoteLibraryService::open(catalog.clone(), &catalog_path, &cache_root)
        .expect("open remote Library service");

    let materialized = service
        .materialize(
            CONNECTION_A,
            "",
            "",
            &remote_photo_id.to_string(),
            &remote_representation_id.to_string(),
        )
        .expect("reuse verified original despite inspection failure");
    assert_eq!(materialized.local_photo_id, registered.photo_id);
    assert_eq!(materialized.metadata, fallback_metadata);
    assert!(!materialized.inspection_diagnostic.is_empty());

    drop(service);
    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn same_length_corrupt_cached_original_is_not_reused() {
    let root = temporary_directory("corrupt-cached-original");
    let source_path = root.join("corrupt.nef");
    let expected = b"expected remote bytes";
    let corrupt = vec![b'x'; expected.len()];
    fs::write(&source_path, &corrupt).expect("write corrupt source");

    assert!(
        !cached_original_matches(
            &source_path,
            u64::try_from(expected.len()).expect("fixture length"),
            *blake3::hash(expected).as_bytes(),
        )
        .expect("verify cached source"),
        "byte length alone must not authorize cached remote source reuse"
    );

    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn verified_cached_raster_is_reregistered_after_catalog_identity_loss() {
    let root = temporary_directory("stale-catalog-identity");
    let catalog_path = root.join("catalog.sqlite");
    let cache_root = root.join("cache");
    let source_path = root.join("materialized.jpg");
    let bytes = b"verified cached raster without decodable pixels";
    fs::write(&source_path, bytes).expect("write cached raster");
    let source = fingerprint_source(&source_path).expect("fingerprint cached raster");
    let digest = *blake3::hash(bytes).as_bytes();
    let remote_photo_id = shadow_domain::PhotoId::new_v7();
    let remote_representation_id = shadow_domain::RepresentationId::new_v7();
    let stale_local_photo_id = shadow_domain::PhotoId::new_v7();
    let stale_local_representation_id = shadow_domain::RepresentationId::new_v7();
    let fallback_metadata = RemotePhotoMetadata {
        orientation: Some(3),
        ..RemotePhotoMetadata::default()
    };
    let manifest = RemotePhotoManifest {
        photo_id: remote_photo_id,
        representation_id: remote_representation_id,
        display_name: "materialized.jpg".into(),
        source_byte_len: source.byte_len,
        source_modified_at_ms: source.modified_at_ms,
        metadata: fallback_metadata.clone(),
        preview: RemotePreviewAvailability::Unavailable {
            reason: PreviewUnavailableReason::NotPrepared,
        },
        representations: vec![RemoteRepresentationManifest {
            representation_id: remote_representation_id,
            kind: RepresentationKind::OriginalRaster,
            display_name: "materialized.jpg".into(),
            source_byte_len: source.byte_len,
            source_modified_at_ms: source.modified_at_ms,
            location_count: 1,
            online_location_count: 1,
            original_identity: RemoteOriginalIdentity::Available {
                digest_blake3: digest,
            },
        }],
    };
    let mirror_snapshot = RemoteLibraryMirrorSnapshot {
        server: None,
        photos: vec![RemotePhotoMirror {
            manifest: manifest.clone(),
            cached_preview: None,
            review_state: RemoteReviewState::default(),
            local_source: Some(MirroredLocalSource::for_remote_manifest(
                stale_local_photo_id,
                stale_local_representation_id,
                digest,
                source_path.to_string_lossy().into_owned(),
                &manifest,
            )),
        }],
    };
    let mirror_root = root.join("remote-library/connections").join(CONNECTION_A);
    fs::create_dir_all(&mirror_root).expect("create mirror root");
    fs::write(
        mirror_root.join("remote-library.json"),
        serde_json::to_vec_pretty(&mirror_snapshot).expect("serialize mirror"),
    )
    .expect("write mirror");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let service = RemoteLibraryService::open(catalog.clone(), &catalog_path, &cache_root)
        .expect("open remote Library service");

    let materialized = service
        .materialize(
            CONNECTION_A,
            "",
            "",
            &remote_photo_id.to_string(),
            &remote_representation_id.to_string(),
        )
        .expect("reregister verified cache without contacting server");
    assert_ne!(materialized.local_photo_id, stale_local_photo_id);
    assert_ne!(
        materialized.local_representation_id,
        stale_local_representation_id
    );
    assert_eq!(materialized.metadata, fallback_metadata);
    assert!(materialized.reused_existing);
    assert!(
        !materialized.inspection_diagnostic.is_empty(),
        "an unavailable raster decoder remains diagnostic rather than blocking the verified original"
    );
    let catalog_source = catalog
        .photo_source(materialized.local_photo_id)
        .expect("read reregistered source")
        .expect("reregistered source exists");
    assert_eq!(
        catalog_source.representation_id,
        materialized.local_representation_id
    );
    assert_eq!(catalog_source.source, source);

    drop(service);
    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    let reopened = Catalog::open(&catalog_path).expect("reopen catalog");
    let representation = reopened
        .photo_representation(
            materialized.local_photo_id,
            materialized.local_representation_id,
        )
        .expect("read exact representation")
        .expect("exact representation exists");
    assert_eq!(representation.kind, RepresentationKind::OriginalRaster);
    drop(reopened);
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn first_materialization_migrates_remote_curation_without_overwriting_local_changes() {
    let root = temporary_directory("curation-migration");
    let catalog_path = root.join("catalog.sqlite");
    let cache_root = root.join("cache");
    let source_path = root.join("materialized.nef");
    fs::write(&source_path, b"materialized remote original").expect("write source fixture");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&source_path),
            byte_len: fs::metadata(&source_path).expect("source metadata").len(),
            modified_at_ms: Some(1),
            now_ms: 2,
        })
        .expect("register source fixture");
    let service = RemoteLibraryService::open(catalog.clone(), &catalog_path, &cache_root)
        .expect("open remote Library service");

    service
        .migrate_review_state(
            registered.photo_id,
            &RemoteReviewState {
                flag: RemoteReviewFlag::Picked,
                rating: 4,
                liked: true,
                color_label: "red".to_owned(),
                updated_at_ms: 42,
            },
            100,
        )
        .expect("migrate first remote state");
    let decision = catalog
        .photo_decision_state(registered.photo_id)
        .expect("read decision");
    assert_eq!(decision.flag, PhotoFlag::Picked);
    assert_eq!(decision.rating, 4);
    let library = catalog
        .photo_library_state(registered.photo_id)
        .expect("read Library state");
    assert!(library.liked);
    assert_eq!(library.color_label, "red");
    assert_eq!(library.updated_at_ms, 42);

    service
        .migrate_review_state(
            registered.photo_id,
            &RemoteReviewState {
                flag: RemoteReviewFlag::Rejected,
                rating: 1,
                liked: false,
                color_label: "blue".to_owned(),
                updated_at_ms: 200,
            },
            200,
        )
        .expect("preserve existing local state");
    let retained_decision = catalog
        .photo_decision_state(registered.photo_id)
        .expect("read retained decision");
    assert_eq!(retained_decision.flag, PhotoFlag::Picked);
    assert_eq!(retained_decision.rating, 4);
    let retained_library = catalog
        .photo_library_state(registered.photo_id)
        .expect("read retained Library state");
    assert!(retained_library.liked);
    assert_eq!(retained_library.color_label, "red");

    drop(service);
    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn first_run_snapshot_is_empty_and_offline_safe() {
    let root = temporary_directory("empty-snapshot");
    let catalog_path = root.join("catalog.sqlite");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let service = RemoteLibraryService::open(catalog.clone(), &catalog_path, &root.join("cache"))
        .expect("open remote Library service");

    let snapshot = service.snapshot(CONNECTION_A).expect("read empty snapshot");
    assert!(snapshot.server.is_none());
    assert!(snapshot.photos.is_empty());

    drop(service);
    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn connection_mirrors_have_independent_persistent_roots() {
    let root = temporary_directory("connection-roots");
    let catalog_path = root.join("catalog.sqlite");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let service = RemoteLibraryService::open(catalog.clone(), &catalog_path, &root.join("cache"))
        .expect("open remote Library service");

    service.snapshot(CONNECTION_A).expect("open first mirror");
    service.snapshot(CONNECTION_B).expect("open second mirror");
    assert!(
        root.join("remote-library/connections")
            .join(CONNECTION_A)
            .is_dir()
    );
    assert!(
        root.join("remote-library/connections")
            .join(CONNECTION_B)
            .is_dir()
    );

    drop(service);
    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn stale_mirror_preview_records_do_not_publish_missing_or_truncated_files() {
    let root = temporary_directory("stale-preview-record");
    let catalog_path = root.join("catalog.sqlite");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let service = RemoteLibraryService::open(catalog.clone(), &catalog_path, &root.join("cache"))
        .expect("open remote Library service");
    let bytes = b"complete remote preview";
    let digest = *blake3::hash(bytes).as_bytes();
    let preview = CachedRemotePreview {
        manifest: RemotePreviewManifest {
            role: RemotePreviewRole::GeneratedProxy,
            digest_blake3: digest,
            byte_len: u64::try_from(bytes.len()).expect("fixture length"),
            codec: PreviewCodec::Jpeg,
            dimensions: ImageDimensions {
                width: 2_048,
                height: 1_365,
            },
            pixel_orientation: RemotePreviewPixelOrientation::DisplayOriented,
        },
    };

    assert!(
        cached_preview_path(&service.preview_store, &preview).is_none(),
        "a mirror record alone must not authorize a nonexistent proxy path"
    );
    let stored = service
        .preview_store
        .put(bytes)
        .expect("store complete preview fixture");
    assert_eq!(
        cached_preview_path(&service.preview_store, &preview),
        Some(service.preview_store.resolve(stored.digest)),
        "the exact cached proxy remains available while the server is offline"
    );
    fs::write(service.preview_store.resolve(stored.digest), b"truncated")
        .expect("truncate proxy fixture");
    assert!(
        cached_preview_path(&service.preview_store, &preview).is_none(),
        "a truncated cache object must degrade to an explicit unavailable state"
    );

    drop(service);
    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    fs::remove_dir_all(root).expect("remove fixture");
}

fn temporary_directory(label: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!(
        "shadow-desktop-remote-library-{label}-{}",
        uuid::Uuid::now_v7()
    ));
    fs::create_dir_all(&path).expect("create fixture directory");
    path
}

fn metadata_snapshot() -> DecoderSnapshot {
    DecoderSnapshot {
        provider: DecodeProviderSnapshot {
            id: "anonymous".into(),
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
