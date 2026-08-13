use std::{collections::BTreeMap, ffi::OsString, path::Path};

use super::*;

#[test]
fn default_layout_uses_discovery_and_keeps_durable_rasters_outside_preview_cache() {
    let paths = SubjectMaskRuntimePaths::discover_with(
        Path::new("/Applications/Shadow.app/Contents/MacOS/Shadow"),
        Path::new("/Users/test/Library/Application Support/Shadow/Shadow/cache"),
        |_| None,
    )
    .unwrap();

    assert_eq!(paths.infer_base_url_override, None);
    assert_eq!(
        paths.infer_credential_file,
        Path::new(
            "/Users/test/Library/Application Support/Shadow/Shadow/credentials/infer-runtime-shadow.token"
        )
    );
    assert_eq!(
        paths.scratch_root,
        Path::new("/Users/test/Library/Application Support/Shadow/Shadow/cache/ai/subject-mask")
    );
    assert_eq!(
        paths.derived_raster_store_root,
        Path::new("/Users/test/Library/Application Support/Shadow/Shadow/derived-rasters")
    );
}

#[test]
fn explicit_infer_overrides_remain_diagnostic_only_inputs() {
    let environment = BTreeMap::from([
        (
            INFER_BASE_URL_OVERRIDE,
            OsString::from("http://127.0.0.1:8787"),
        ),
        (
            INFER_CREDENTIAL_OVERRIDE,
            OsString::from("/private/shadow.token"),
        ),
    ]);
    let paths = SubjectMaskRuntimePaths::discover_with(
        Path::new("/Applications/Shadow.app/Contents/MacOS/Shadow"),
        Path::new("/Users/test/Shadow/cache"),
        |key| environment.get(key).cloned(),
    )
    .unwrap();

    assert_eq!(
        paths.infer_base_url_override.as_deref(),
        Some("http://127.0.0.1:8787")
    );
    assert_eq!(
        paths.infer_credential_file,
        Path::new("/private/shadow.token")
    );
}
