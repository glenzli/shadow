use super::*;

#[test]
fn default_layout_keeps_models_outside_the_rebuildable_cache() {
    let paths = RawFoundationRuntimePaths::discover_with(
        Path::new("/Applications/Shadow.app/Contents/MacOS/Shadow"),
        Path::new("/Users/test/Library/Application Support/Shadow/cache"),
        |_| None,
    )
    .expect("default paths");

    assert_eq!(
        paths.provider_executable,
        Path::new(
            "/Applications/Shadow.app/Contents/Helpers/RawNIND/shadow-rawnind-foundation-provider"
        )
    );
    assert_eq!(
        paths.manifest_path,
        Path::new(
            "/Applications/Shadow.app/Contents/Helpers/RawNIND/shadow-rawnind-foundation-model-manifest.json"
        )
    );
    assert_eq!(
        paths.model_package,
        Path::new(
            "/Users/test/Library/Application Support/Shadow/models/rawnind-public-bayer-release-5.6.0/rawdenoise-nind.dtmodel"
        )
    );
    assert_eq!(
        paths.model_graph,
        Path::new(
            "/Users/test/Library/Application Support/Shadow/models/rawnind-public-bayer-release-5.6.0/model_bayer.onnx"
        )
    );
    assert_eq!(
        paths.foundation_store_root,
        Path::new("/Users/test/Library/Application Support/Shadow/cache/ai/raw-foundations")
    );
    assert_eq!(
        paths.raw_frame_staging_root,
        Path::new("/Users/test/Library/Application Support/Shadow/cache/ai/raw-frame-staging")
    );
}

#[test]
fn non_bundle_layout_keeps_the_complete_onedir_beside_the_executable() {
    let paths = RawFoundationRuntimePaths::discover_with(
        Path::new("/opt/shadow/Shadow"),
        Path::new("/var/lib/shadow/cache"),
        |_| None,
    )
    .expect("flat installation paths");

    assert_eq!(
        paths.provider_executable,
        Path::new("/opt/shadow/shadow-rawnind-foundation-provider")
    );
    assert_eq!(
        paths.manifest_path,
        Path::new("/opt/shadow/shadow-rawnind-foundation-model-manifest.json")
    );
}

#[test]
fn explicit_installation_overrides_do_not_relocate_the_cache() {
    let paths = RawFoundationRuntimePaths::discover_with(
        Path::new("/Applications/Shadow.app/Contents/MacOS/Shadow"),
        Path::new("/data/Shadow/cache"),
        |key| {
            Some(match key {
                PROVIDER_OVERRIDE => OsString::from("/provider"),
                PACKAGE_OVERRIDE => OsString::from("/package"),
                GRAPH_OVERRIDE => OsString::from("/graph"),
                MANIFEST_OVERRIDE => OsString::from("/manifest"),
                _ => return None,
            })
        },
    )
    .expect("overridden paths");

    assert_eq!(paths.provider_executable, Path::new("/provider"));
    assert_eq!(paths.model_package, Path::new("/package"));
    assert_eq!(paths.model_graph, Path::new("/graph"));
    assert_eq!(paths.manifest_path, Path::new("/manifest"));
    assert_eq!(
        paths.foundation_store_root,
        Path::new("/data/Shadow/cache/ai/raw-foundations")
    );
    assert_eq!(
        paths.raw_frame_staging_root,
        Path::new("/data/Shadow/cache/ai/raw-frame-staging")
    );
}
