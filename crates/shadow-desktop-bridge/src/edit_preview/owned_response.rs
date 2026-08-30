//! Stable ownership for a completed edit preview crossing the desktop CXX boundary.
//!
//! Interactive RGB8 pixels and their optional R8 mask coverage stay in the
//! native frame owner. Settled JPEG/analysis responses retain the existing
//! materialized FFI projection. Callers that cannot retain this owner may use
//! the explicit materializing compatibility path.

use shadow_bridge::{
    EDIT_PREVIEW_MASK_COVERAGE_SCHEMA_VERSION, InteractiveEditPreviewStorage, OpticsReceipt,
    OwnedInteractivePreviewFrame,
};
use shadow_domain::ImageDimensions;

use crate::{AnyResult, ffi};

const HOST_RGB8_STORAGE_KIND: u8 = 0;
const APPLE_METAL_RGBA8_SRGB_STORAGE_KIND: u8 = 1;
const APPLE_METAL_RGBA8_SRGB_PIXEL_FORMAT: u8 = 1;

/// One terminal edit-preview result with an optional zero-copy interactive frame.
pub struct OwnedEditedPreview {
    projection: ffi::FfiEditedPreview,
    interactive_frame: Option<OwnedInteractivePreviewFrame>,
}

impl std::fmt::Debug for OwnedEditedPreview {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("OwnedEditedPreview")
            .field("projection", &self.projection)
            .field(
                "interactive_retained_bytes",
                &self
                    .interactive_frame
                    .as_ref()
                    .map_or(0, OwnedInteractivePreviewFrame::retained_bytes),
            )
            .finish_non_exhaustive()
    }
}

impl OwnedEditedPreview {
    pub(crate) fn materialized(projection: ffi::FfiEditedPreview) -> Box<Self> {
        Box::new(Self {
            projection,
            interactive_frame: None,
        })
    }

    pub(crate) fn interactive(
        frame: OwnedInteractivePreviewFrame,
        level_zero_dimensions: ImageDimensions,
        optics: &OpticsReceipt,
    ) -> Box<Self> {
        let projection = interactive_projection(&frame, level_zero_dimensions, optics);
        Box::new(Self {
            projection,
            interactive_frame: Some(frame),
        })
    }

    /// Small descriptors and settled diagnostics shared with the existing desktop projection.
    ///
    /// For an interactive result, the two large payload vectors are deliberately
    /// empty. Borrow them from [`Self::interactive_pixels`] and
    /// [`Self::interactive_mask_coverage_samples`] while retaining this owner.
    pub fn projection(&self) -> &ffi::FfiEditedPreview {
        &self.projection
    }

    /// True only for the zero-copy interactive RGB8 route.
    pub fn interactive_frame_available(&self) -> bool {
        self.interactive_frame.is_some()
    }

    /// Native storage kind; zero is also the sentinel for a materialized/non-interactive result.
    pub fn interactive_storage_kind(&self) -> u8 {
        self.interactive_frame
            .as_ref()
            .map_or(HOST_RGB8_STORAGE_KIND, |frame| match frame.storage() {
                InteractiveEditPreviewStorage::HostRgb8 => HOST_RGB8_STORAGE_KIND,
                InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(_) => {
                    APPLE_METAL_RGBA8_SRGB_STORAGE_KIND
                }
            })
    }

    pub fn interactive_native_texture_row_stride_bytes(&self) -> u32 {
        self.metal_texture()
            .map_or(0, |texture| texture.row_stride_bytes)
    }

    pub fn interactive_native_texture_pixel_format(&self) -> u8 {
        self.metal_texture()
            .map_or(0, |_| APPLE_METAL_RGBA8_SRGB_PIXEL_FORMAT)
    }

    pub fn interactive_native_resource_id(&self) -> u64 {
        self.metal_texture()
            .map_or(0, |texture| texture.resource_id)
    }

    pub fn interactive_native_texture_handle(&self) -> usize {
        self.metal_texture()
            .map_or(0, |texture| texture.texture_handle)
    }

    pub fn interactive_native_device_handle(&self) -> usize {
        self.metal_texture()
            .map_or(0, |texture| texture.device_handle)
    }

    pub fn interactive_materialized_pixel_bytes(&self) -> usize {
        self.interactive_frame
            .as_ref()
            .map_or(0, OwnedInteractivePreviewFrame::materialized_pixel_bytes)
    }

    pub fn interactive_presentation_fallback_diagnostic(&self) -> &str {
        self.interactive_frame
            .as_ref()
            .map_or("", |frame| frame.presentation_fallback_diagnostic())
    }

    /// Explicitly materializes display-sRGB RGB8 pixels from the retained frame.
    pub fn interactive_pixels(&self) -> AnyResult<&[u8]> {
        self.interactive_frame
            .as_ref()
            .map_or(
                Ok(&[][..]),
                OwnedInteractivePreviewFrame::materialize_pixels,
            )
            .map_err(Into::into)
    }

    /// Optional R8 mask coverage borrowed from the same retained native frame.
    pub fn interactive_mask_coverage_samples(&self) -> &[u8] {
        self.interactive_frame
            .as_ref()
            .and_then(OwnedInteractivePreviewFrame::mask_coverage)
            .map_or(&[], |coverage| coverage.samples)
    }

    /// Native payload bytes retained by this owner, excluding small descriptors.
    pub fn interactive_retained_bytes(&self) -> usize {
        self.interactive_frame
            .as_ref()
            .map_or(0, OwnedInteractivePreviewFrame::retained_bytes)
    }

    /// Explicit compatibility fallback for callers that cannot retain an opaque owner.
    pub(crate) fn into_materialized_projection(
        mut self: Box<Self>,
    ) -> AnyResult<ffi::FfiEditedPreview> {
        if let Some(frame) = self.interactive_frame.take() {
            self.projection.bytes = frame.materialize_pixels()?.to_vec();
            self.projection.mask_coverage_samples = frame
                .mask_coverage()
                .map_or_else(Vec::new, |coverage| coverage.samples.to_vec());
        }
        Ok(self.projection)
    }

    fn metal_texture(&self) -> Option<shadow_bridge::AppleMetalEditPreviewTexture> {
        match self.interactive_frame.as_ref()?.storage() {
            InteractiveEditPreviewStorage::HostRgb8 => None,
            InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(texture) => Some(texture),
        }
    }
}

fn interactive_projection(
    frame: &OwnedInteractivePreviewFrame,
    level_zero_dimensions: ImageDimensions,
    optics: &OpticsReceipt,
) -> ffi::FfiEditedPreview {
    let dimensions = frame.dimensions();
    let mask_coverage = frame.mask_coverage();
    ffi::FfiEditedPreview {
        terminal: ffi::FfiEditPreviewTerminal::Completed,
        width: dimensions.width,
        height: dimensions.height,
        level_zero_width: level_zero_dimensions.width,
        level_zero_height: level_zero_dimensions.height,
        row_stride_bytes: frame.row_stride_bytes(),
        // The opaque owner supplies these two large payloads as borrowed
        // slices. Empty vectors make an accidental materializing path visible.
        bytes: Vec::new(),
        mask_coverage_available: mask_coverage.is_some(),
        mask_coverage_version: mask_coverage
            .map_or(0, |_| EDIT_PREVIEW_MASK_COVERAGE_SCHEMA_VERSION),
        mask_coverage_target_layer_index: mask_coverage
            .map_or(0, |coverage| coverage.target_layer_index),
        mask_selection_revision: mask_coverage
            .map_or(0, |coverage| coverage.mask_selection_revision),
        mask_coverage_width: mask_coverage.map_or(0, |coverage| coverage.dimensions.width),
        mask_coverage_height: mask_coverage.map_or(0, |coverage| coverage.dimensions.height),
        mask_coverage_row_stride_bytes: mask_coverage
            .map_or(0, |coverage| coverage.row_stride_bytes),
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

#[cfg(test)]
mod tests;
