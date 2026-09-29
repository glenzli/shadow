//! Move-only interactive frame ownership across the first CXX boundary.
//!
//! Native rendering moves either packed RGB8 or one completed Metal presentation surface plus
//! optional paired R8 mask coverage into one immutable C++ owner. Descriptor validation never
//! materializes the native texture. Explicit fallback publishes packed RGB8 exactly once.

use shadow_domain::ImageDimensions;

use crate::{BridgeError, EDIT_PREVIEW_MASK_COVERAGE_VERSION, EditPreviewMaskCoverageRequest, ffi};

// SAFETY: native construction finishes and validates every allocation before publication. The
// handle owns (rather than borrows) its host bytes/native surface and coverage vector. Lazy RGB8
// materialization is guarded by native call_once plus release/acquire publication; after
// publication the vector is never mutated or reallocated. The owner has no session or decoder
// reference, and destruction is thread-independent.
unsafe impl Send for ffi::InteractiveEditPreviewFrameHandle {}
// SAFETY: see Send above. All shared access is immutable and returns slices tied to the handle.
unsafe impl Sync for ffi::InteractiveEditPreviewFrameHandle {}

#[derive(Debug)]
struct MaskCoverageDescriptor {
    version: String,
    target_layer_index: u32,
    target_component_index: Option<u32>,
    mask_selection_revision: u64,
    dimensions: ImageDimensions,
    row_stride_bytes: u32,
}

const HOST_RGB8_STORAGE_KIND: u8 = 0;
const APPLE_METAL_RGBA8_SRGB_STORAGE_KIND: u8 = 1;
const APPLE_METAL_RGBA8_SRGB_PIXEL_FORMAT: u8 = 1;
const METAL_LINEAR_TEXTURE_ROW_ALIGNMENT: u32 = 256;

/// Borrowed native Metal texture identity retained by one interactive frame.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub struct AppleMetalEditPreviewTexture {
    pub resource_id: u64,
    pub texture_handle: usize,
    pub device_handle: usize,
    pub row_stride_bytes: u32,
}

/// Storage selected by the completed native renderer.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum InteractiveEditPreviewStorage {
    HostRgb8,
    AppleMetalRgba8Srgb(AppleMetalEditPreviewTexture),
}

/// Borrowed exact-renderer coverage from the same native owner as the paired RGB frame.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub struct EditPreviewMaskCoverageView<'frame> {
    pub version: &'frame str,
    pub target_layer_index: u32,
    pub target_component_index: Option<u32>,
    pub mask_selection_revision: u64,
    pub dimensions: ImageDimensions,
    pub row_stride_bytes: u32,
    pub samples: &'frame [u8],
}

/// Move-only owner for one completed interactive RGB8 frame and optional R8 mask coverage.
///
/// Pixel addresses remain stable for this object's lifetime, including after moving the Rust
/// wrapper or dropping the preview session that produced it.
pub struct OwnedInteractivePreviewFrame {
    handle: cxx::UniquePtr<ffi::InteractiveEditPreviewFrameHandle>,
    dimensions: ImageDimensions,
    row_stride_bytes: u32,
    storage: InteractiveEditPreviewStorage,
    presentation_fallback_diagnostic: String,
    mask_coverage: Option<MaskCoverageDescriptor>,
}

impl std::fmt::Debug for OwnedInteractivePreviewFrame {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("OwnedInteractivePreviewFrame")
            .field("dimensions", &self.dimensions)
            .field("row_stride_bytes", &self.row_stride_bytes)
            .field("storage", &self.storage)
            .field("materialized_pixel_bytes", &self.materialized_pixel_bytes())
            .field("retained_bytes", &self.retained_bytes())
            .field("mask_coverage", &self.mask_coverage)
            .finish_non_exhaustive()
    }
}

impl OwnedInteractivePreviewFrame {
    pub(crate) fn from_nullable_native(
        handle: cxx::UniquePtr<ffi::InteractiveEditPreviewFrameHandle>,
        expected_dimensions: ImageDimensions,
        request: Option<EditPreviewMaskCoverageRequest>,
    ) -> Result<Option<Self>, BridgeError> {
        if handle.is_null() {
            return Ok(None);
        }
        Self::from_native(handle, expected_dimensions, request).map(Some)
    }

    fn from_native(
        handle: cxx::UniquePtr<ffi::InteractiveEditPreviewFrameHandle>,
        expected_dimensions: ImageDimensions,
        request: Option<EditPreviewMaskCoverageRequest>,
    ) -> Result<Self, BridgeError> {
        let native = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let dimensions = ImageDimensions {
            width: native.width(),
            height: native.height(),
        };
        if dimensions != expected_dimensions || dimensions.width == 0 || dimensions.height == 0 {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "owned interactive frame dimensions do not match the rendered canvas",
            ));
        }
        let expected_stride =
            dimensions
                .width
                .checked_mul(3)
                .ok_or(BridgeError::InvalidEditPreviewOutput(
                    "owned interactive RGB8 row stride overflows u32",
                ))?;
        let row_stride_bytes = native.row_stride_bytes();
        if row_stride_bytes != expected_stride {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "owned interactive frame must be tightly packed RGB8",
            ));
        }
        let expected_pixel_bytes = checked_buffer_len(row_stride_bytes, dimensions.height)?;
        let presentation_fallback_diagnostic = native.presentation_fallback_diagnostic();
        let storage = validate_storage_descriptor(
            native,
            dimensions,
            expected_pixel_bytes,
            &presentation_fallback_diagnostic,
        )?;
        let mask_coverage = validate_mask_coverage_descriptor(native, request, dimensions)?;
        let mask_bytes = mask_coverage
            .as_ref()
            .map_or(0, |_| native.mask_coverage_samples().len());
        let expected_retained_bytes = match storage {
            InteractiveEditPreviewStorage::HostRgb8 => expected_pixel_bytes,
            InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(texture) => {
                checked_buffer_len(texture.row_stride_bytes, dimensions.height)?
            }
        }
        .checked_add(mask_bytes)
        .ok_or(BridgeError::InvalidEditPreviewOutput(
            "owned interactive retained byte count overflows usize",
        ))?;
        if native.retained_bytes() != expected_retained_bytes {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "owned interactive retained bytes do not match its published storage",
            ));
        }
        Ok(Self {
            handle,
            dimensions,
            row_stride_bytes,
            storage,
            presentation_fallback_diagnostic,
            mask_coverage,
        })
    }

    /// Pixel dimensions shared by the RGB8 frame and optional mask coverage.
    #[must_use]
    pub const fn dimensions(&self) -> ImageDimensions {
        self.dimensions
    }

    /// Tightly packed RGB8 row stride in bytes.
    #[must_use]
    pub const fn row_stride_bytes(&self) -> u32 {
        self.row_stride_bytes
    }

    /// Native storage published for this frame. Inspecting it performs no host readback.
    #[must_use]
    pub const fn storage(&self) -> InteractiveEditPreviewStorage {
        self.storage
    }

    /// Named Metal-presentation failure when this frame completed on host RGB8 fallback.
    #[must_use]
    pub fn presentation_fallback_diagnostic(&self) -> &str {
        &self.presentation_fallback_diagnostic
    }

    /// Host RGB8 bytes currently materialized by the native owner.
    #[must_use]
    pub fn materialized_pixel_bytes(&self) -> usize {
        self.native().materialized_pixel_bytes()
    }

    /// Explicitly materializes immutable display-sRGB RGB8 pixels.
    ///
    /// The first native-texture caller performs the shared-buffer RGB extraction; every later or
    /// concurrent caller receives the same stable allocation.
    ///
    /// # Errors
    ///
    /// Returns an error when native materialization fails or publishes bytes that do not match
    /// the frame's declared dimensions and row stride.
    pub fn materialize_pixels(&self) -> Result<&[u8], BridgeError> {
        let pixels = self.native().materialize_pixels()?;
        if pixels.len() != checked_buffer_len(self.row_stride_bytes, self.dimensions.height)? {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "materialized interactive RGB8 byte count must equal stride times height",
            ));
        }
        Ok(pixels)
    }

    /// Optional tightly packed R8 mask coverage borrowed from the same native owner.
    #[must_use]
    pub fn mask_coverage(&self) -> Option<EditPreviewMaskCoverageView<'_>> {
        let descriptor = self.mask_coverage.as_ref()?;
        Some(EditPreviewMaskCoverageView {
            version: &descriptor.version,
            target_layer_index: descriptor.target_layer_index,
            target_component_index: descriptor.target_component_index,
            mask_selection_revision: descriptor.mask_selection_revision,
            dimensions: descriptor.dimensions,
            row_stride_bytes: descriptor.row_stride_bytes,
            samples: self.native().mask_coverage_samples(),
        })
    }

    /// Native bytes retained by this frame owner, excluding small descriptors.
    #[must_use]
    pub fn retained_bytes(&self) -> usize {
        self.native().retained_bytes()
    }

    fn native(&self) -> &ffi::InteractiveEditPreviewFrameHandle {
        self.handle
            .as_ref()
            .expect("validated interactive frame owner cannot become null")
    }
}

fn validate_storage_descriptor(
    native: &ffi::InteractiveEditPreviewFrameHandle,
    dimensions: ImageDimensions,
    expected_pixel_bytes: usize,
    fallback_diagnostic: &str,
) -> Result<InteractiveEditPreviewStorage, BridgeError> {
    let kind = native.storage_kind();
    let texture_row_stride = native.native_texture_row_stride_bytes();
    let texture_pixel_format = native.native_texture_pixel_format();
    let resource_id = native.native_resource_id();
    let texture_handle = native.native_texture_handle();
    let device_handle = native.native_device_handle();
    let materialized_pixel_bytes = native.materialized_pixel_bytes();

    match kind {
        HOST_RGB8_STORAGE_KIND => {
            if texture_row_stride != 0
                || texture_pixel_format != 0
                || resource_id != 0
                || texture_handle != 0
                || device_handle != 0
                || materialized_pixel_bytes != expected_pixel_bytes
            {
                return Err(BridgeError::InvalidEditPreviewOutput(
                    "host interactive frame must use empty native-texture sentinels and complete RGB8",
                ));
            }
            Ok(InteractiveEditPreviewStorage::HostRgb8)
        }
        APPLE_METAL_RGBA8_SRGB_STORAGE_KIND => {
            let minimum_row_stride =
                dimensions
                    .width
                    .checked_mul(4)
                    .ok_or(BridgeError::InvalidEditPreviewOutput(
                        "native RGBA8 texture row stride overflows u32",
                    ))?;
            if texture_row_stride < minimum_row_stride
                || !texture_row_stride.is_multiple_of(METAL_LINEAR_TEXTURE_ROW_ALIGNMENT)
                || texture_pixel_format != APPLE_METAL_RGBA8_SRGB_PIXEL_FORMAT
                || resource_id == 0
                || texture_handle == 0
                || device_handle == 0
                || materialized_pixel_bytes != 0
                || !fallback_diagnostic.is_empty()
            {
                return Err(BridgeError::InvalidEditPreviewOutput(
                    "Metal interactive frame has an invalid native-texture descriptor",
                ));
            }
            Ok(InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(
                AppleMetalEditPreviewTexture {
                    resource_id,
                    texture_handle,
                    device_handle,
                    row_stride_bytes: texture_row_stride,
                },
            ))
        }
        _ => Err(BridgeError::InvalidEditPreviewOutput(
            "owned interactive frame uses an unknown storage kind",
        )),
    }
}

fn validate_mask_coverage_descriptor(
    native: &ffi::InteractiveEditPreviewFrameHandle,
    request: Option<EditPreviewMaskCoverageRequest>,
    expected_dimensions: ImageDimensions,
) -> Result<Option<MaskCoverageDescriptor>, BridgeError> {
    let available = native.mask_coverage_available();
    let version = native.mask_coverage_version();
    let target_layer_index = native.mask_coverage_layer_index();
    let component_selected = native.mask_coverage_component_selected();
    let component_index = native.mask_coverage_component_index();
    let dimensions = ImageDimensions {
        width: native.mask_coverage_width(),
        height: native.mask_coverage_height(),
    };
    let row_stride_bytes = native.mask_coverage_row_stride_bytes();
    let samples = native.mask_coverage_samples();

    if !available {
        if !version.is_empty()
            || target_layer_index != 0
            || component_selected
            || component_index != 0
            || dimensions.width != 0
            || dimensions.height != 0
            || row_stride_bytes != 0
            || !samples.is_empty()
        {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "unavailable owned mask coverage must use the empty sentinel",
            ));
        }
        if request.is_some() {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "requested owned mask coverage is missing from the completed frame",
            ));
        }
        return Ok(None);
    }

    let request = request.ok_or(BridgeError::InvalidEditPreviewOutput(
        "owned mask coverage was returned without a requested target",
    ))?;
    if version != EDIT_PREVIEW_MASK_COVERAGE_VERSION {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "owned mask coverage uses an unsupported semantic version",
        ));
    }
    if target_layer_index != request.target_layer_index {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "owned mask coverage target does not match the requested Grade Node",
        ));
    }
    if component_selected != request.target_component_index.is_some()
        || (component_selected && Some(component_index) != request.target_component_index)
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "owned mask coverage component does not match the requested target",
        ));
    }
    if dimensions != expected_dimensions {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "owned mask coverage dimensions must match the paired RGB frame",
        ));
    }
    if row_stride_bytes != dimensions.width {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "owned mask coverage must be tightly packed R8",
        ));
    }
    if samples.len() != checked_buffer_len(row_stride_bytes, dimensions.height)? {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "owned mask coverage byte count must equal stride times height",
        ));
    }

    Ok(Some(MaskCoverageDescriptor {
        version,
        target_layer_index,
        target_component_index: request.target_component_index,
        mask_selection_revision: request.mask_selection_revision,
        dimensions,
        row_stride_bytes,
    }))
}

fn checked_buffer_len(row_stride_bytes: u32, height: u32) -> Result<usize, BridgeError> {
    usize::try_from(
        u64::from(row_stride_bytes)
            .checked_mul(u64::from(height))
            .ok_or(BridgeError::InvalidEditPreviewOutput(
                "owned interactive frame byte count overflows u64",
            ))?,
    )
    .map_err(|_| {
        BridgeError::InvalidEditPreviewOutput(
            "owned interactive frame dimensions exceed the host address space",
        )
    })
}

#[cfg(test)]
mod tests;
