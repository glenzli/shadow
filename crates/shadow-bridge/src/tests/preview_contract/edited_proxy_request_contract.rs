//! Edited-proxy parameter validation before source I/O.

use std::path::Path;

use crate::{BasicEditParameters, BridgeError, EditedProxyRequest, render_libraw_edited_proxy};

#[test]
fn edited_proxy_parameters_fail_closed_before_raw_io() {
    let invalid_requests = [
        EditedProxyRequest {
            edits: BasicEditParameters {
                exposure_stops: f64::NAN,
                ..BasicEditParameters::default()
            },
            ..EditedProxyRequest::default()
        },
        EditedProxyRequest {
            edits: BasicEditParameters {
                contrast_factor: -0.01,
                ..BasicEditParameters::default()
            },
            ..EditedProxyRequest::default()
        },
        EditedProxyRequest {
            edits: BasicEditParameters {
                white_balance_temperature: 2.0,
                ..BasicEditParameters::default()
            },
            ..EditedProxyRequest::default()
        },
        EditedProxyRequest {
            edits: BasicEditParameters {
                saturation_factor: f64::INFINITY,
                ..BasicEditParameters::default()
            },
            ..EditedProxyRequest::default()
        },
        EditedProxyRequest {
            max_edge: 0,
            ..EditedProxyRequest::default()
        },
        EditedProxyRequest {
            jpeg_quality: 0,
            ..EditedProxyRequest::default()
        },
    ];

    for request in invalid_requests {
        let error =
            render_libraw_edited_proxy(Path::new("fixture-that-must-not-be-opened.raw"), request)
                .expect_err("invalid request must fail");
        assert!(matches!(error, BridgeError::InvalidEditRequest(_)));
    }
}
