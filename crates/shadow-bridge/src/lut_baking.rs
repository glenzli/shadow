//! Bounded CPU sampling of explicitly portable Grade Node transforms.

use crate::{
    AdjustmentGeometry, AdjustmentRenderPlan, BridgeError, ffi, render_wire::ffi_render_request,
};

/// One independent, thread-safe cancellation source for an export snapshot.
#[derive(Clone)]
pub struct LutBakeCancellation {
    handle: cxx::SharedPtr<ffi::EditPreviewCancellationHandle>,
}

impl std::fmt::Debug for LutBakeCancellation {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LutBakeCancellation")
            .finish_non_exhaustive()
    }
}

impl LutBakeCancellation {
    /// Allocates an independent cancellation source.
    ///
    /// # Errors
    /// Returns the native allocation error or a null-handle error.
    pub fn new() -> Result<Self, BridgeError> {
        let handle = ffi::new_edit_preview_cancellation()?;
        if handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        Ok(Self { handle })
    }

    pub fn cancel(&self) -> bool {
        self.handle
            .as_ref()
            .is_some_and(ffi::EditPreviewCancellationHandle::cancel)
    }
}

/// Standard linear-sRGB .cube text and measured interpolation error.
#[derive(Debug, Clone)]
pub struct BakedCubeLut {
    pub document: Vec<u8>,
    pub maximum_absolute_error: f64,
    pub root_mean_square_error: f64,
    pub probe_count: u32,
}

/// Validates the same bounded grammar that the native renderer consumes.
///
/// # Errors
/// Returns an error for malformed, unsupported, or oversized .cube documents.
pub fn validate_cube_lut_document(document: &[u8]) -> Result<(), BridgeError> {
    ffi::validate_cube_lut_document(document)?;
    Ok(())
}

/// Samples a projected color-only plan through the production CPU executor.
///
/// # Errors
/// Rejects spatial operations, masks, geometry, unsupported sizes, invalid
/// parameters, non-finite output, and cancellation.
pub fn bake_adjustment_lut(
    plan: &AdjustmentRenderPlan,
    size: u16,
    cancellation: &LutBakeCancellation,
) -> Result<BakedCubeLut, BridgeError> {
    plan.validate()?;
    if plan.liquify.is_some() || plan.geometry != AdjustmentGeometry::default() {
        return Err(BridgeError::InvalidEditRequest(
            "LUT baking cannot represent geometry",
        ));
    }
    let handle = cancellation
        .handle
        .as_ref()
        .ok_or(BridgeError::NullHandle)?;
    let baked = ffi::bake_adjustment_cube_lut(&ffi_render_request(plan, 1, 95), size, handle)?;
    Ok(BakedCubeLut {
        document: baked.document,
        maximum_absolute_error: baked.maximum_absolute_error,
        root_mean_square_error: baked.root_mean_square_error,
        probe_count: baked.probe_count,
    })
}

#[cfg(test)]
mod tests;
