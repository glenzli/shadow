use crate::{LibraryPhotoFacts, RegisteredAsset, RepresentationFingerprint};

pub(super) fn facts_for(
    registered: RegisteredAsset,
    captured_at_unix_seconds: Option<i64>,
    make: &str,
    model: &str,
) -> LibraryPhotoFacts {
    LibraryPhotoFacts {
        photo_id: registered.photo_id,
        captured_at_unix_seconds,
        capture_day: captured_at_unix_seconds
            .map(|_| "2023-11-14".to_owned())
            .unwrap_or_default(),
        camera_make: make.into(),
        camera_model: model.into(),
        lens_make: "Nikon".into(),
        lens_model: "NIKKOR Z 24-120mm f/4 S".into(),
        aperture_milli: Some(4_000),
        focal_length_tenth_mm: Some(240),
        iso_speed: Some(800.0),
        latitude_e7: None,
        longitude_e7: None,
        place_name: String::new(),
        indexed_representation_id: Some(registered.representation_id),
        indexed_source: Some(RepresentationFingerprint {
            byte_len: 100,
            modified_at_ms: Some(10),
        }),
        indexed_at_ms: 100,
    }
}
