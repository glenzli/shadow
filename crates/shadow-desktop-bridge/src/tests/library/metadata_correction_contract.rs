//! Desktop contract for manual metadata correction and previewed GPX application.

use shadow_catalog::{LibraryPhotoFacts, RegisterAsset, RepresentationFingerprint};
use shadow_domain::{AssetLocation, EntityId, Platform, RepresentationId, RepresentationKind};

use crate::{DesktopSession, ffi, open_desktop_session};

#[test]
fn manual_corrections_and_one_time_gpx_preview_share_the_effective_metadata_layer() {
    let fixture = MetadataFixture::open();
    let photo_id = fixture.register_photo();
    let photo_id_text = photo_id.to_string();

    let observed = fixture
        .session
        .library_metadata_state(&photo_id_text)
        .expect("read decoder-observed metadata");
    assert!(observed.has_observed_capture_time);
    assert_eq!(observed.observed_captured_at_unix_seconds, 1_700_000_000);
    assert!(observed.has_observed_coordinates);
    assert_eq!(observed.observed_latitude_e7, 399_000_000);
    assert_eq!(observed.capture_time_override_mode, "inherit");
    assert_eq!(observed.coordinates_override_mode, "inherit");

    let corrected_time = fixture
        .session
        .set_library_capture_time_override(&photo_id_text, "set", 1_700_000_060)
        .expect("set manual capture time");
    assert_eq!(
        corrected_time.effective_captured_at_unix_seconds,
        1_700_000_060
    );
    assert_eq!(corrected_time.capture_time_override_origin, "manual");

    let corrected_coordinates = fixture
        .session
        .set_library_coordinates_override(&photo_id_text, "set", 31.2304, 121.4737, "Shanghai")
        .expect("set manual coordinates");
    assert_eq!(corrected_coordinates.effective_latitude_e7, 312_304_000);
    assert_eq!(corrected_coordinates.effective_longitude_e7, 1_214_737_000);
    assert_eq!(corrected_coordinates.effective_place_name, "Shanghai");
    assert_eq!(corrected_coordinates.coordinates_override_origin, "manual");

    let gpx_path = fixture.root.join("camera-track.gpx");
    std::fs::write(
        &gpx_path,
        concat!(
            "<?xml version=\"1.0\"?><gpx version=\"1.1\"><trk><trkseg>",
            "<trkpt lat=\"30.25\" lon=\"120.50\">",
            "<time>2023-11-14T22:14:20Z</time></trkpt>",
            "</trkseg></trk></gpx>"
        ),
    )
    .expect("write GPX fixture");
    let preview = fixture
        .session
        .preview_library_gpx_import(
            gpx_path.to_str().expect("UTF-8 GPX path"),
            vec![ffi::FfiBatchPhotoTarget {
                photo_id: photo_id_text.clone(),
                source_path: fixture.source_path.to_string_lossy().into_owned(),
            }],
            0,
            1,
        )
        .expect("preview GPX match");
    assert_eq!(preview.requested_photo_count, 1);
    assert_eq!(preview.matched_photo_count, 1);
    assert_eq!(preview.unmatched_photo_count, 0);
    assert_eq!(preview.proposal_sample.len(), 1);
    assert_eq!(preview.proposal_sample[0].latitude_e7, 302_500_000);
    assert_eq!(preview.proposal_sample[0].longitude_e7, 1_205_000_000);

    let receipt = fixture
        .session
        .apply_library_gpx_import(&preview.preview_id)
        .expect("apply previewed GPX coordinates");
    assert_eq!(receipt.requested_photo_count, 1);
    assert_eq!(receipt.applied_photo_count, 1);
    assert!(
        fixture
            .session
            .apply_library_gpx_import(&preview.preview_id)
            .is_err(),
        "one preview must not be applied twice"
    );

    let effective = fixture
        .session
        .library_metadata_state(&photo_id_text)
        .expect("read effective metadata after GPX apply");
    assert_eq!(effective.effective_captured_at_unix_seconds, 1_700_000_060);
    assert_eq!(effective.capture_time_override_origin, "manual");
    assert_eq!(effective.effective_latitude_e7, 302_500_000);
    assert_eq!(effective.effective_longitude_e7, 1_205_000_000);
    assert_eq!(effective.coordinates_override_origin, "gpx");
    assert!(
        effective
            .coordinates_source_label
            .starts_with("camera-track.gpx")
    );
}

#[test]
fn capture_time_batch_previews_effective_shifts_and_restores_camera_values_atomically() {
    let fixture = MetadataFixture::open();
    let first = fixture.register_photo_named("first.nef", Some(1_700_000_000));
    let second = fixture.register_photo_named("second.nef", Some(1_700_001_000));
    let missing = fixture.register_photo_named("missing-time.nef", None);
    let first_id = first.photo_id.to_string();
    fixture
        .session
        .set_library_capture_time_override(&first_id, "set", 1_700_000_060)
        .expect("seed effective capture-time correction");

    let target = |photo_id: shadow_domain::PhotoId| ffi::FfiBatchPhotoTarget {
        photo_id: photo_id.to_string(),
        source_path: String::new(),
    };
    let targets = || {
        vec![
            target(first.photo_id),
            target(second.photo_id),
            target(missing.photo_id),
            target(first.photo_id),
        ]
    };
    let preview = fixture
        .session
        .preview_library_capture_time_batch(targets(), "shift", 3_600)
        .expect("preview effective capture-time shift");
    assert_eq!(preview.requested_photo_count, 3);
    assert_eq!(preview.applicable_photo_count, 2);
    assert_eq!(preview.skipped_photo_count, 1);
    assert_eq!(preview.proposal_sample.len(), 2);
    let first_proposal = preview
        .proposal_sample
        .iter()
        .find(|proposal| proposal.photo_id == first_id)
        .expect("first photo proposal");
    assert_eq!(
        first_proposal.before_captured_at_unix_seconds,
        1_700_000_060
    );
    assert_eq!(first_proposal.after_captured_at_unix_seconds, 1_700_003_660);

    let shifted = fixture
        .session
        .apply_library_capture_time_batch(&preview.preview_id)
        .expect("atomically apply capture-time shift");
    assert_eq!(shifted.requested_photo_count, 3);
    assert_eq!(shifted.applied_photo_count, 2);
    assert_eq!(
        fixture
            .session
            .library_metadata_state(&first_id)
            .expect("read shifted first photo")
            .effective_captured_at_unix_seconds,
        1_700_003_660
    );

    let restore = fixture
        .session
        .preview_library_capture_time_batch(targets(), "inherit", 0)
        .expect("preview camera-time restoration");
    assert_eq!(restore.requested_photo_count, 3);
    assert_eq!(restore.applicable_photo_count, 2);
    assert_eq!(restore.skipped_photo_count, 1);
    let restored = fixture
        .session
        .apply_library_capture_time_batch(&restore.preview_id)
        .expect("restore selected camera times");
    assert_eq!(restored.applied_photo_count, 2);
    assert!(
        fixture
            .session
            .apply_library_capture_time_batch(&restore.preview_id)
            .is_err(),
        "one capture-time preview must not be applied twice"
    );
    assert_eq!(
        fixture
            .session
            .library_metadata_state(&first_id)
            .expect("read restored first photo")
            .effective_captured_at_unix_seconds,
        1_700_000_000
    );
    assert_eq!(
        fixture
            .session
            .library_metadata_state(&second.photo_id.to_string())
            .expect("read restored second photo")
            .effective_captured_at_unix_seconds,
        1_700_001_000
    );
}

#[test]
fn coordinate_batch_preserves_existing_locations_by_default_and_requires_explicit_replacement() {
    let fixture = MetadataFixture::open();
    let existing = fixture.register_photo_named_with_coordinates(
        "existing.nef",
        Some(1_700_000_000),
        Some((39.9, 116.4, "Beijing")),
    );
    let missing =
        fixture.register_photo_named_with_coordinates("missing.nef", Some(1_700_001_000), None);
    let targets = || {
        vec![
            ffi::FfiBatchPhotoTarget {
                photo_id: existing.photo_id.to_string(),
                source_path: String::new(),
            },
            ffi::FfiBatchPhotoTarget {
                photo_id: missing.photo_id.to_string(),
                source_path: String::new(),
            },
            ffi::FfiBatchPhotoTarget {
                photo_id: missing.photo_id.to_string(),
                source_path: String::new(),
            },
        ]
    };

    let safe_preview = fixture
        .session
        .preview_library_coordinate_batch(
            targets(),
            "missing",
            31.2304,
            121.4737,
            "Shanghai",
            "place-search:Shanghai",
        )
        .expect("preview missing-only coordinate assignment");
    assert_eq!(safe_preview.requested_photo_count, 2);
    assert_eq!(safe_preview.missing_photo_count, 1);
    assert_eq!(safe_preview.existing_photo_count, 1);
    assert_eq!(safe_preview.applicable_photo_count, 1);
    assert_eq!(safe_preview.skipped_photo_count, 1);
    assert_eq!(safe_preview.replacement_photo_count, 0);
    let safe_receipt = fixture
        .session
        .apply_library_coordinate_batch(&safe_preview.preview_id)
        .expect("apply missing-only coordinate assignment");
    assert_eq!(safe_receipt.applied_photo_count, 1);
    assert!(
        fixture
            .session
            .apply_library_coordinate_batch(&safe_preview.preview_id)
            .is_err(),
        "coordinate preview must be one-time"
    );
    let unchanged = fixture
        .session
        .library_metadata_state(&existing.photo_id.to_string())
        .expect("read preserved location");
    assert_eq!(unchanged.effective_place_name, "Beijing");
    assert_eq!(unchanged.coordinates_override_mode, "inherit");
    let filled = fixture
        .session
        .library_metadata_state(&missing.photo_id.to_string())
        .expect("read assigned location");
    assert_eq!(filled.effective_latitude_e7, 312_304_000);
    assert_eq!(filled.effective_place_name, "Shanghai");
    assert_eq!(filled.coordinates_source_label, "place-search:Shanghai");

    let replace_preview = fixture
        .session
        .preview_library_coordinate_batch(
            targets(),
            "replace",
            35.6762,
            139.6503,
            "Tokyo",
            "manual-map",
        )
        .expect("preview explicit location replacement");
    assert_eq!(replace_preview.applicable_photo_count, 2);
    assert_eq!(replace_preview.skipped_photo_count, 0);
    assert_eq!(replace_preview.existing_photo_count, 2);
    assert_eq!(replace_preview.replacement_photo_count, 2);
    fixture
        .session
        .apply_library_coordinate_batch(&replace_preview.preview_id)
        .expect("replace all selected coordinates");
    for photo_id in [existing.photo_id, missing.photo_id] {
        let state = fixture
            .session
            .library_metadata_state(&photo_id.to_string())
            .expect("read replaced location");
        assert_eq!(state.effective_latitude_e7, 356_762_000);
        assert_eq!(state.effective_longitude_e7, 1_396_503_000);
        assert_eq!(state.effective_place_name, "Tokyo");
        assert_eq!(state.coordinates_source_label, "manual-map");
    }
}

struct MetadataFixture {
    root: std::path::PathBuf,
    source_path: std::path::PathBuf,
    session: Box<DesktopSession>,
}

impl MetadataFixture {
    fn open() -> Self {
        let root = std::env::temp_dir().join(format!(
            "shadow-desktop-library-metadata-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create Library metadata fixture");
        let session = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("open desktop session");
        Self {
            source_path: root.join("source.nef"),
            root,
            session,
        }
    }

    fn register_photo(&self) -> shadow_domain::PhotoId {
        self.register_photo_named("source.nef", Some(1_700_000_000))
            .photo_id
    }

    fn register_photo_named(
        &self,
        file_name: &str,
        captured_at_unix_seconds: Option<i64>,
    ) -> shadow_catalog::RegisteredAsset {
        self.register_photo_named_with_coordinates(
            file_name,
            captured_at_unix_seconds,
            Some((39.9, 116.4, "Beijing")),
        )
    }

    fn register_photo_named_with_coordinates(
        &self,
        file_name: &str,
        captured_at_unix_seconds: Option<i64>,
        coordinates: Option<(f64, f64, &str)>,
    ) -> shadow_catalog::RegisteredAsset {
        let source_path = self.root.join(file_name);
        let display_path = source_path.to_string_lossy().into_owned();
        let registered = self
            .session
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    display_path.as_bytes().to_vec(),
                    display_path,
                ),
                byte_len: 20_000,
                modified_at_ms: Some(200),
                now_ms: 1_700_000_000,
            })
            .expect("register metadata fixture source");
        self.session
            .catalog
            .upsert_photo_library_facts(&LibraryPhotoFacts {
                photo_id: registered.photo_id,
                captured_at_unix_seconds,
                capture_day: captured_at_unix_seconds
                    .map_or_else(String::new, |_| "2023-11-14".into()),
                camera_make: "Nikon Corporation".into(),
                camera_model: "Nikon Z 8".into(),
                lens_make: "Nikon".into(),
                lens_model: "NIKKOR Z 24-120mm f/4 S".into(),
                aperture_milli: Some(4_000),
                focal_length_tenth_mm: Some(240),
                iso_speed: Some(800.0),
                latitude_e7: coordinates.map(|value| (value.0 * 10_000_000.0).round() as i32),
                longitude_e7: coordinates.map(|value| (value.1 * 10_000_000.0).round() as i32),
                place_name: coordinates.map_or_else(String::new, |value| value.2.into()),
                indexed_representation_id: Some(registered.representation_id),
                indexed_source: Some(RepresentationFingerprint {
                    byte_len: 20_000,
                    modified_at_ms: Some(200),
                }),
                indexed_at_ms: 1_700_000_001,
            })
            .expect("index metadata fixture facts");
        registered
    }
}

impl Drop for MetadataFixture {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.root).expect("remove Library metadata fixture");
    }
}
