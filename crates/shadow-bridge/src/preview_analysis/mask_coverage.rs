//! Versioned exact-renderer coverage paired with one warm edit preview.

use shadow_domain::ImageDimensions;

use crate::{
    AdjustmentRenderOperation, AdjustmentRenderPlan, BridgeError, decoder::dimensions, ffi,
};

/// Stable native semantic identity for exact pre-adjustment local-mask coverage.
pub const EDIT_PREVIEW_MASK_COVERAGE_VERSION: &str =
    "shadow.edit-preview-mask-coverage.v1:r8-pre-adjustment-input:inverted:paired-geometry";
/// Compact desktop-facing schema number for the current coverage contract.
pub const EDIT_PREVIEW_MASK_COVERAGE_SCHEMA_VERSION: u32 = 1;

/// One optional mask-coverage target in authored Grade Node order.
///
/// `mask_selection_revision` is host transaction metadata. It is deliberately
/// excluded from native mask evaluation and every durable render/cache identity.
#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash)]
pub struct EditPreviewMaskCoverageRequest {
    pub target_layer_index: u32,
    pub mask_selection_revision: u64,
}

/// Exact renderer coverage paired with one preview generation.
///
/// Samples are tightly packed row-major R8. The native semantic version is
/// retained for diagnostics; only the exact current version passes validation.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditPreviewMaskCoverage {
    pub version: String,
    pub target_layer_index: u32,
    pub mask_selection_revision: u64,
    pub dimensions: ImageDimensions,
    pub row_stride_bytes: u32,
    pub samples: Vec<u8>,
}

/// One RGB preview plus optional exact mask coverage from the same native render.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RenderedEditPreview {
    pub proxy: shadow_domain::ProxyPayload,
    pub mask_coverage: Option<EditPreviewMaskCoverage>,
}

pub(crate) fn validate_mask_coverage_request(
    plan: &AdjustmentRenderPlan,
    request: EditPreviewMaskCoverageRequest,
) -> Result<(), BridgeError> {
    let layer_count = plan
        .nodes
        .iter()
        .filter(|node| {
            matches!(
                node.operation,
                AdjustmentRenderOperation::LocalMaskLayerStart { .. }
            )
        })
        .count();
    let target = usize::try_from(request.target_layer_index).map_err(|_| {
        BridgeError::InvalidEditRequest("mask coverage target does not fit the host address space")
    })?;
    if target >= layer_count {
        return Err(BridgeError::InvalidEditRequest(
            "mask coverage target is outside the compiled Grade Node layers",
        ));
    }
    Ok(())
}

pub(crate) fn validate_mask_coverage(
    coverage: ffi::FfiEditPreviewMaskCoverage,
    request: Option<EditPreviewMaskCoverageRequest>,
    expected_dimensions: ImageDimensions,
) -> Result<Option<EditPreviewMaskCoverage>, BridgeError> {
    if !coverage.available {
        if !coverage.version.is_empty()
            || coverage.layer_index != 0
            || coverage.dimensions.width != 0
            || coverage.dimensions.height != 0
            || coverage.row_stride_bytes != 0
            || !coverage.samples.is_empty()
        {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "unavailable mask coverage must use the empty wire sentinel",
            ));
        }
        return Ok(None);
    }

    let Some(request) = request else {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "native mask coverage was returned without a requested target",
        ));
    };
    if coverage.version != EDIT_PREVIEW_MASK_COVERAGE_VERSION {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "mask coverage uses an unsupported semantic version",
        ));
    }
    if coverage.layer_index != request.target_layer_index {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "mask coverage target does not match the requested Grade Node",
        ));
    }

    let coverage_dimensions = dimensions(&coverage.dimensions);
    if coverage_dimensions != expected_dimensions
        || coverage_dimensions.width == 0
        || coverage_dimensions.height == 0
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "mask coverage dimensions must match the paired preview",
        ));
    }
    if coverage.row_stride_bytes != coverage_dimensions.width {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "mask coverage must be tightly packed R8",
        ));
    }
    let expected_len = usize::try_from(
        u64::from(coverage.row_stride_bytes)
            .checked_mul(u64::from(coverage_dimensions.height))
            .ok_or(BridgeError::InvalidEditPreviewOutput(
                "mask coverage byte count overflows u64",
            ))?,
    )
    .map_err(|_| {
        BridgeError::InvalidEditPreviewOutput(
            "mask coverage dimensions exceed the host address space",
        )
    })?;
    if coverage.samples.len() != expected_len {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "mask coverage byte count must equal stride times height",
        ));
    }

    Ok(Some(EditPreviewMaskCoverage {
        version: coverage.version,
        target_layer_index: coverage.layer_index,
        mask_selection_revision: request.mask_selection_revision,
        dimensions: coverage_dimensions,
        row_stride_bytes: coverage.row_stride_bytes,
        samples: coverage.samples,
    }))
}

#[cfg(test)]
mod tests;
