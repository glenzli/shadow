//! Lensfun discovery from a live RAW and from a detached metadata snapshot.

use std::path::Path;

use shadow_domain::DecodeSupport;

use crate::{inspect_photo, query_libraw_optics_profiles, query_optics_profiles_from_metadata};

#[test]
#[ignore = "requires SHADOW_TEST_DNG to identify a camera present in Lensfun"]
fn real_dng_enumerates_compatible_lensfun_profiles() {
    let path = std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG");
    let candidates =
        query_libraw_optics_profiles(Path::new(&path)).expect("query Lensfun candidates");
    assert!(!candidates.is_empty());
    assert!(candidates.iter().all(|candidate| {
        !candidate.camera_model.is_empty() && !candidate.lens_model.is_empty()
    }));
}

#[test]
#[ignore = "requires SHADOW_TEST_METADATA_RAW to identify a camera present in Lensfun"]
fn raw_metadata_snapshot_enumerates_lensfun_profiles_without_pixel_decode() {
    let path = std::env::var_os("SHADOW_TEST_METADATA_RAW").expect("SHADOW_TEST_METADATA_RAW");
    let snapshot = inspect_photo(Path::new(&path)).expect("inspect metadata-only RAW");
    assert_eq!(snapshot.capabilities.metadata, DecodeSupport::Available);

    // Candidate enumeration receives only the detached snapshot. The source path and decode
    // handle cannot cross this boundary, regardless of the provider's pixel capabilities.
    let candidates = query_optics_profiles_from_metadata(&snapshot.metadata);
    assert!(!candidates.is_empty());
    assert!(candidates.iter().all(|candidate| {
        !candidate.camera_model.is_empty() && !candidate.lens_model.is_empty()
    }));
}
