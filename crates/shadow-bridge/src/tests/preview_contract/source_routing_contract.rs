//! Source-router proxy-request validation and supported raster identity.

use std::path::Path;

use crate::{BridgeError, photo_supported_raster_extensions, render_photo_reference_proxy};

#[test]
fn photo_router_reference_proxy_rejects_invalid_requests_before_source_io() {
    for (max_edge, jpeg_quality) in [(0, 82), (16_385, 82), (1_024, 0), (1_024, 101)] {
        let error = render_photo_reference_proxy(
            Path::new("fixture-that-must-not-be-opened.photo"),
            max_edge,
            jpeg_quality,
        )
        .expect_err("invalid generic proxy request must fail before opening the source");
        assert!(matches!(error, BridgeError::InvalidEditRequest(_)));
    }
}

#[test]
fn photo_router_reports_mandatory_jpeg_raster_support() {
    let extensions = photo_supported_raster_extensions();
    assert!(extensions.contains(&"jpg".to_owned()));
    assert!(extensions.contains(&"jpeg".to_owned()));
}
