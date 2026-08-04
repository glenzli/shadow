use super::*;

fn inspector_with_route_cache(runtime_cache_root: Option<PathBuf>) -> PhotoInspector {
    let isolated_helper_path = runtime_cache_root
        .as_ref()
        .map(|_| PathBuf::from("/tmp/shadow-image-decode-helper"));
    PhotoInspector {
        version: "test-router-version".to_owned(),
        original_raster_extensions: vec!["jpg".to_owned(), "heif".to_owned()],
        proxy_variant_key: "test-proxy".to_owned(),
        isolated_proxy_runtime_cache: runtime_cache_root,
        isolated_helper_path,
    }
}

#[test]
fn helper_enabled_raw_inspection_never_chooses_the_direct_route() {
    let inspector = inspector_with_route_cache(Some(PathBuf::from("/tmp/shadow-test-cache")));
    assert_eq!(
        inspector.inspection_route(Path::new("source.CR3")),
        InspectionRoute::IsolatedRaw
    );
    assert_eq!(
        inspector.inspection_route(Path::new("source.nef")),
        InspectionRoute::IsolatedRaw
    );
}

#[test]
fn original_rasters_and_no_helper_cache_keep_the_direct_route() {
    let helper_enabled = inspector_with_route_cache(Some(PathBuf::from("/tmp/shadow-test-cache")));
    assert_eq!(
        helper_enabled.inspection_route(Path::new("source.JPG")),
        InspectionRoute::Direct
    );
    let direct = inspector_with_route_cache(None);
    assert_eq!(
        direct.inspection_route(Path::new("source.cr3")),
        InspectionRoute::Direct
    );
}
