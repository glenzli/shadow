use shadow_bridge::{
    EDIT_PREVIEW_HDR_HEADROOM_BIN_COUNT, EDIT_PREVIEW_HISTOGRAM_BIN_COUNT, EditPreviewAnalysis,
    OpticsReceipt, SensorClippingMask,
};
use shadow_domain::{ImageDimensions, PreviewCodec, ProxyPayload};

use super::*;
use crate::ffi;

#[test]
fn cancelled_response_is_an_empty_terminal_sentinel() {
    let response = cancelled_edited_preview();

    assert_eq!(response.terminal, ffi::FfiEditPreviewTerminal::Cancelled);
    assert_eq!((response.width, response.height), (0, 0));
    assert!(response.bytes.is_empty());
    assert!(!response.analysis_available);
    assert!(!response.sensor_clipping_available);
    assert!(response.optics_status.is_empty());
}

#[test]
fn completed_response_projects_settled_diagnostics_and_hides_them_interactively() {
    let dimensions = ImageDimensions {
        width: 2,
        height: 1,
    };
    let mut red = [0; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT];
    red[7] = 2;
    let mut headroom = [0; EDIT_PREVIEW_HDR_HEADROOM_BIN_COUNT];
    headroom[3] = 1;
    let analysis = EditPreviewAnalysis {
        version: "analysis-v1".to_owned(),
        sample_dimensions: dimensions,
        red,
        green: [0; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
        blue: [0; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
        luma: [0; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
        below_zero_samples: [1, 2, 3],
        above_one_samples: [4, 5, 6],
        hdr_headroom_bins: headroom,
        hdr_headroom_pixels: 1,
        hdr_peak_headroom_ev: 1.5,
        pixel_count: 2,
        shadow_clipped_pixels: 1,
        highlight_clipped_pixels: 1,
    };
    let sensor = SensorClippingMask {
        available: true,
        dimensions,
        samples: vec![1, 2],
        highlight_pixel_count: 1,
        shadow_pixel_count: 1,
    };
    let optics = OpticsReceipt {
        status: "applied".to_owned(),
        provider_id: "lensfun".to_owned(),
        provider_version: "1".to_owned(),
        camera_profile: "camera".to_owned(),
        lens_profile: "lens".to_owned(),
        distortion_available: true,
        tca_available: true,
        vignetting_available: true,
        applied_distortion: true,
        applied_tca: false,
        applied_vignetting: true,
        vignetting_used_distance_fallback: false,
        applied_scaling: true,
    };

    let settled = completed_edited_preview(
        proxy(dimensions),
        Some(&analysis),
        &optics,
        &sensor,
        EditPreviewPolicy::Settled,
    );
    assert_eq!(settled.terminal, ffi::FfiEditPreviewTerminal::Completed);
    assert_eq!((settled.width, settled.height), (2, 1));
    assert_eq!(settled.red_histogram[7], 2);
    assert_eq!(settled.below_zero_samples, [1, 2, 3]);
    assert_eq!(settled.hdr_headroom_bins[3], 1);
    assert!(settled.sensor_clipping_available);
    assert_eq!(settled.sensor_clipping_mask, [1, 2]);
    assert_eq!(settled.optics_provider_id, "lensfun");
    assert!(settled.optics_applied_scaling);

    let interactive = completed_edited_preview(
        proxy(dimensions),
        None,
        &optics,
        &sensor,
        EditPreviewPolicy::Interactive,
    );
    assert!(!interactive.analysis_available);
    assert!(interactive.red_histogram.is_empty());
    assert!(!interactive.sensor_clipping_available);
    assert!(interactive.sensor_clipping_mask.is_empty());
    assert_eq!(interactive.optics_status, "applied");
}

fn proxy(dimensions: ImageDimensions) -> ProxyPayload {
    ProxyPayload {
        dimensions,
        codec: PreviewCodec::Jpeg,
        bits_per_channel: 8,
        channels: 3,
        bytes: vec![0xff, 0xd8, 0xff, 0xd9],
    }
}
