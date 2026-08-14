use super::*;

#[test]
fn default_layout_keeps_shadow_cache_and_infer_credentials_separate() {
    let paths = RawFoundationRuntimePaths::discover_with(
        Path::new("/Users/test/Library/Application Support/Shadow/cache"),
        |_| None,
    )
    .expect("default paths");

    assert_eq!(
        paths.foundation_store_root,
        Path::new("/Users/test/Library/Application Support/Shadow/cache/ai/raw-foundations")
    );
    assert_eq!(
        paths.raw_frame_staging_root,
        Path::new("/Users/test/Library/Application Support/Shadow/cache/ai/raw-frame-staging")
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
fn infer_consumer_reuses_the_shared_overrides() {
    let paths = RawFoundationRuntimePaths::discover_with(
        Path::new("/Users/test/Library/Application Support/Shadow/cache"),
        |key| match key {
            INFER_BASE_URL_OVERRIDE => Some(OsString::from("http://127.0.0.1:9876")),
            INFER_CREDENTIAL_OVERRIDE => Some(OsString::from("/private/credential")),
            _ => None,
        },
    )
    .expect("infer paths");

    assert_eq!(
        paths.infer_base_url_override.as_deref(),
        Some("http://127.0.0.1:9876")
    );
    assert_eq!(
        paths.infer_credential_file,
        Path::new("/private/credential")
    );
}
