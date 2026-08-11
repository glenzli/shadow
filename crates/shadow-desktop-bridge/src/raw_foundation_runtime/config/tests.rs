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
    assert_eq!(
        paths.execution_route,
        RawFoundationExecutionRoute::LegacySidecar
    );
    assert_eq!(paths.infer_base_url_override, None);
    assert_eq!(
        paths.infer_credential_file,
        Path::new(
            "/Users/test/Library/Application Support/Shadow/credentials/infer-runtime-shadow.token"
        )
    );
}

#[test]
fn infer_route_is_explicit_and_reuses_the_shared_consumer_overrides() {
    let paths = RawFoundationRuntimePaths::discover_with(
        Path::new("/Applications/Shadow.app/Contents/MacOS/Shadow"),
        Path::new("/Users/test/Library/Application Support/Shadow/cache"),
        |key| match key {
            EXECUTION_ROUTE_OVERRIDE => Some(OsString::from("infer-runtime")),
            INFER_BASE_URL_OVERRIDE => Some(OsString::from("http://127.0.0.1:9876")),
            INFER_CREDENTIAL_OVERRIDE => Some(OsString::from("/private/credential")),
            _ => None,
        },
    )
    .expect("infer paths");

    assert_eq!(
        paths.execution_route,
        RawFoundationExecutionRoute::InferRuntime
    );
    assert_eq!(
        paths.infer_base_url_override.as_deref(),
        Some("http://127.0.0.1:9876")
    );
    assert_eq!(
        paths.infer_credential_file,
        Path::new("/private/credential")
    );
}

#[test]
fn unknown_execution_route_fails_closed() {
    let error = RawFoundationRuntimePaths::discover_with(
        Path::new("/Applications/Shadow.app/Contents/MacOS/Shadow"),
        Path::new("/Users/test/Library/Application Support/Shadow/cache"),
        |key| (key == EXECUTION_ROUTE_OVERRIDE).then(|| OsString::from("automatic")),
    )
    .unwrap_err();

    assert!(matches!(
        error,
        RawFoundationRuntimePathError::InvalidExecutionRoute
    ));
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
