use std::{collections::BTreeMap, ffi::OsString};

use super::*;

#[test]
fn default_layout_keeps_models_and_durable_rasters_outside_preview_cache() {
    let paths = SubjectMaskRuntimePaths::discover_with(
        Path::new("/Applications/Shadow.app/Contents/MacOS/Shadow"),
        Path::new("/Users/test/Library/Application Support/Shadow/Shadow/cache"),
        |_| None,
    )
    .unwrap();

    assert_eq!(
        paths.provider_executable,
        Path::new("/Applications/Shadow.app/Contents/MacOS/shadow-sam2-coreml-provider")
    );
    assert_eq!(
        paths.manifest_path,
        Path::new("/Applications/Shadow.app/Contents/MacOS/shadow-sam2-coreml-model-manifest.json")
    );
    assert_eq!(
        paths.model_directory,
        Path::new(
            "/Users/test/Library/Application Support/Shadow/Shadow/models/apple/coreml-sam2.1-small"
        )
    );
    assert_eq!(
        paths.scratch_root,
        Path::new("/Users/test/Library/Application Support/Shadow/Shadow/cache/ai/sam2-coreml")
    );
    assert_eq!(
        paths.derived_raster_store_root,
        Path::new("/Users/test/Library/Application Support/Shadow/Shadow/derived-rasters")
    );
    assert!(
        !paths
            .derived_raster_store_root
            .starts_with("/Users/test/Library/Application Support/Shadow/Shadow/cache")
    );
}

#[test]
fn explicit_local_provider_paths_override_the_packaged_defaults() {
    let environment = BTreeMap::from([
        (
            PROVIDER_OVERRIDE,
            OsString::from("/private/provider/shadow-sam"),
        ),
        (MODEL_OVERRIDE, OsString::from("/private/models/sam2.1")),
        (
            MANIFEST_OVERRIDE,
            OsString::from("/private/manifests/sam2.1.json"),
        ),
    ]);
    let paths = SubjectMaskRuntimePaths::discover_with(
        Path::new("/Applications/Shadow.app/Contents/MacOS/Shadow"),
        Path::new("/Users/test/Shadow/cache"),
        |key| environment.get(key).cloned(),
    )
    .unwrap();

    assert_eq!(
        paths.provider_executable,
        Path::new("/private/provider/shadow-sam")
    );
    assert_eq!(paths.model_directory, Path::new("/private/models/sam2.1"));
    assert_eq!(
        paths.manifest_path,
        Path::new("/private/manifests/sam2.1.json")
    );
}
