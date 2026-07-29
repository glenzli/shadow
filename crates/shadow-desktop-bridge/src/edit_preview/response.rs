//! Complete projection from a validated native preview into the desktop FFI.

use shadow_bridge::{
    EDIT_PREVIEW_MASK_COVERAGE_SCHEMA_VERSION, EditPreviewAnalysis, EditPreviewMaskCoverage,
    OpticsReceipt, SensorClippingMask,
};
use shadow_domain::{PreviewCodec, ProxyPayload};

use super::EditPreviewPolicy;
use crate::ffi;

pub(crate) fn cancelled_edited_preview() -> ffi::FfiEditedPreview {
    ffi::FfiEditedPreview {
        terminal: ffi::FfiEditPreviewTerminal::Cancelled,
        width: 0,
        height: 0,
        row_stride_bytes: 0,
        bytes: Vec::new(),
        mask_coverage_available: false,
        mask_coverage_version: 0,
        mask_coverage_target_layer_index: 0,
        mask_selection_revision: 0,
        mask_coverage_width: 0,
        mask_coverage_height: 0,
        mask_coverage_row_stride_bytes: 0,
        mask_coverage_samples: Vec::new(),
        sensor_clipping_available: false,
        sensor_clipping_width: 0,
        sensor_clipping_height: 0,
        sensor_clipping_mask: Vec::new(),
        sensor_highlight_clipped_pixels: 0,
        sensor_shadow_clipped_pixels: 0,
        analysis_available: false,
        analysis_version: String::new(),
        analysis_width: 0,
        analysis_height: 0,
        red_histogram: Vec::new(),
        green_histogram: Vec::new(),
        blue_histogram: Vec::new(),
        luma_histogram: Vec::new(),
        below_zero_samples: Vec::new(),
        above_one_samples: Vec::new(),
        hdr_headroom_bins: Vec::new(),
        hdr_headroom_pixels: 0,
        hdr_peak_headroom_ev: 0.0,
        pixel_count: 0,
        shadow_clipped_pixels: 0,
        highlight_clipped_pixels: 0,
        optics_status: String::new(),
        optics_provider_id: String::new(),
        optics_provider_version: String::new(),
        optics_camera_profile: String::new(),
        optics_lens_profile: String::new(),
        optics_distortion_available: false,
        optics_tca_available: false,
        optics_vignetting_available: false,
        optics_applied_distortion: false,
        optics_applied_tca: false,
        optics_applied_vignetting: false,
        optics_vignetting_used_distance_fallback: false,
        optics_applied_scaling: false,
    }
}

// The explicit CXX field mapping is one auditable wire contract; fragmenting
// it into field-family mutators would hide omissions behind construction order.
#[allow(clippy::too_many_lines)]
pub(crate) fn completed_edited_preview(
    proxy: ProxyPayload,
    analysis: Option<&EditPreviewAnalysis>,
    mask_coverage: Option<EditPreviewMaskCoverage>,
    optics: &OpticsReceipt,
    sensor_clipping: &SensorClippingMask,
    policy: EditPreviewPolicy,
) -> ffi::FfiEditedPreview {
    let return_sensor_diagnostics = policy.returns_sensor_diagnostics();
    let analysis_available = analysis.is_some();
    debug_assert_eq!(analysis_available, policy.requires_analysis());
    let row_stride_bytes = match proxy.codec {
        PreviewCodec::Bitmap => proxy
            .dimensions
            .width
            .checked_mul(3)
            .expect("validated RGB8 preview row stride must fit u32"),
        PreviewCodec::Jpeg => 0,
        _ => unreachable!("validated edit previews are JPEG or display-sRGB RGB8"),
    };
    if let Some(mask_coverage) = mask_coverage.as_ref() {
        debug_assert_eq!(mask_coverage.dimensions, proxy.dimensions);
        debug_assert_eq!(
            mask_coverage.row_stride_bytes,
            mask_coverage.dimensions.width
        );
    }
    let (
        mask_coverage_available,
        mask_coverage_version,
        mask_coverage_target_layer_index,
        mask_selection_revision,
        mask_coverage_width,
        mask_coverage_height,
        mask_coverage_row_stride_bytes,
        mask_coverage_samples,
    ) = mask_coverage.map_or_else(
        || (false, 0, 0, 0, 0, 0, 0, Vec::new()),
        |coverage| {
            (
                true,
                EDIT_PREVIEW_MASK_COVERAGE_SCHEMA_VERSION,
                coverage.target_layer_index,
                coverage.mask_selection_revision,
                coverage.dimensions.width,
                coverage.dimensions.height,
                coverage.row_stride_bytes,
                coverage.samples,
            )
        },
    );
    ffi::FfiEditedPreview {
        terminal: ffi::FfiEditPreviewTerminal::Completed,
        width: proxy.dimensions.width,
        height: proxy.dimensions.height,
        row_stride_bytes,
        bytes: proxy.bytes,
        mask_coverage_available,
        mask_coverage_version,
        mask_coverage_target_layer_index,
        mask_selection_revision,
        mask_coverage_width,
        mask_coverage_height,
        mask_coverage_row_stride_bytes,
        mask_coverage_samples,
        sensor_clipping_available: return_sensor_diagnostics && sensor_clipping.available,
        sensor_clipping_width: if return_sensor_diagnostics {
            sensor_clipping.dimensions.width
        } else {
            0
        },
        sensor_clipping_height: if return_sensor_diagnostics {
            sensor_clipping.dimensions.height
        } else {
            0
        },
        sensor_clipping_mask: if return_sensor_diagnostics {
            sensor_clipping.samples.clone()
        } else {
            Vec::new()
        },
        sensor_highlight_clipped_pixels: if return_sensor_diagnostics {
            sensor_clipping.highlight_pixel_count
        } else {
            0
        },
        sensor_shadow_clipped_pixels: if return_sensor_diagnostics {
            sensor_clipping.shadow_pixel_count
        } else {
            0
        },
        analysis_available,
        analysis_version: analysis.map_or_else(String::new, |value| value.version.clone()),
        analysis_width: analysis.map_or(0, |value| value.sample_dimensions.width),
        analysis_height: analysis.map_or(0, |value| value.sample_dimensions.height),
        red_histogram: analysis.map_or_else(Vec::new, |value| value.red.to_vec()),
        green_histogram: analysis.map_or_else(Vec::new, |value| value.green.to_vec()),
        blue_histogram: analysis.map_or_else(Vec::new, |value| value.blue.to_vec()),
        luma_histogram: analysis.map_or_else(Vec::new, |value| value.luma.to_vec()),
        below_zero_samples: analysis
            .map_or_else(Vec::new, |value| value.below_zero_samples.to_vec()),
        above_one_samples: analysis.map_or_else(Vec::new, |value| value.above_one_samples.to_vec()),
        hdr_headroom_bins: analysis.map_or_else(Vec::new, |value| value.hdr_headroom_bins.to_vec()),
        hdr_headroom_pixels: analysis.map_or(0, |value| value.hdr_headroom_pixels),
        hdr_peak_headroom_ev: analysis.map_or(0.0, |value| value.hdr_peak_headroom_ev),
        pixel_count: analysis.map_or(0, |value| value.pixel_count),
        shadow_clipped_pixels: analysis.map_or(0, |value| value.shadow_clipped_pixels),
        highlight_clipped_pixels: analysis.map_or(0, |value| value.highlight_clipped_pixels),
        optics_status: optics.status.clone(),
        optics_provider_id: optics.provider_id.clone(),
        optics_provider_version: optics.provider_version.clone(),
        optics_camera_profile: optics.camera_profile.clone(),
        optics_lens_profile: optics.lens_profile.clone(),
        optics_distortion_available: optics.distortion_available,
        optics_tca_available: optics.tca_available,
        optics_vignetting_available: optics.vignetting_available,
        optics_applied_distortion: optics.applied_distortion,
        optics_applied_tca: optics.applied_tca,
        optics_applied_vignetting: optics.applied_vignetting,
        optics_vignetting_used_distance_fallback: optics.vignetting_used_distance_fallback,
        optics_applied_scaling: optics.applied_scaling,
    }
}
