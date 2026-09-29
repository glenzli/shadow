//! Stateless reference-proxy and adjustment-plan rendering entry points.

use std::path::Path;

use super::{
    BridgeError,
    adjustment::{
        AdjustmentRenderPlan, EditedProxyRequest, basic_adjustment_render_plan,
        validate_jpeg_quality, validate_proxy_max_edge,
    },
    decoder::{dimensions, open_libraw, preview_codec},
    ffi,
    render_wire::{ffi_render_request, proxy_payload},
};

/// Renders a bounded, display-referred JPEG proxy through the `LibRaw`
/// reference path. This is a fallback for RAW files without an embedded
/// preview, not Shadow's eventual camera-domain renderer.
///
/// # Errors
///
/// Returns [`BridgeError`] when the provider cannot render or encode the RAW.
pub fn render_libraw_reference_proxy(
    path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    let handle = open_libraw(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    let proxy = handle.render_reference_proxy(max_edge, jpeg_quality)?;
    Ok(shadow_domain::ProxyPayload {
        dimensions: dimensions(&proxy.dimensions),
        codec: preview_codec(proxy.format),
        bits_per_channel: proxy.bits_per_channel,
        channels: proxy.channels,
        bytes: proxy.bytes,
    })
}

/// Renders a bounded display-referred JPEG proxy through Shadow's source-neutral photo router.
///
/// The router decides whether the source is developed through a RAW provider or decoded from a
/// supported raster. The returned raster is always the same display-proxy contract; inspect the
/// prepared edit session's [`RawDevelopmentReceipt`] to distinguish provider-side RAW
/// development from a raster source.
///
/// # Errors
///
/// Returns an invalid-request, path, source-router, decode, or encoding error.
pub fn render_photo_reference_proxy(
    path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    validate_proxy_max_edge(max_edge)?;
    validate_jpeg_quality(jpeg_quality)?;
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    Ok(proxy_payload(ffi::render_photo_reference_proxy(
        utf8_path,
        max_edge,
        jpeg_quality,
    )?))
}

/// Renders the fixed-order basic edit recipe as a bounded standard JPEG.
///
/// The C++ kernel configures `LibRaw` to emit 16-bit processed linear-light RGB
/// in sRGB/Rec.709-D65 primaries, normalizes it into the float working proxy,
/// and executes adjustment nodes without intermediate clipping. The versioned
/// display boundary then performs hue-preserving Oklab gamut mapping and the
/// sRGB transfer only while encoding RGB8/JPEG. The `LibRaw` result has already
/// undergone black subtraction, white balance, demosaic, camera-to-output color
/// conversion, and fixed integer scaling (with frame-adaptive maximum disabled);
/// it is not untouched sensor-linear mosaic/radiance data or
/// the eventual full RAW color pipeline.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidEditRequest`] before RAW I/O for non-finite,
/// out-of-range, or unbounded parameters. Decoder and encoder failures are
/// returned as [`BridgeError::Decoder`].
pub fn render_libraw_edited_proxy(
    path: &Path,
    request: EditedProxyRequest,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    request.validate()?;
    let plan = basic_adjustment_render_plan(request.edits)?;
    render_libraw_adjustment_plan(path, &plan, request.max_edge, request.jpeg_quality)
}

/// Renders a typed, dependency-ordered adjustment plan as a bounded JPEG.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidEditRequest`] before RAW I/O for an invalid
/// plan, edge bound, or JPEG quality. Decoder, executor, and encoder failures
/// are returned as [`BridgeError::Decoder`].
pub fn render_libraw_adjustment_plan(
    path: &Path,
    plan: &AdjustmentRenderPlan,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    plan.validate()?;
    validate_proxy_max_edge(max_edge)?;
    validate_jpeg_quality(jpeg_quality)?;
    let handle = open_libraw(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    let prepared = handle.prepare_edit_preview(max_edge)?;
    let prepared = prepared.as_ref().ok_or(BridgeError::NullHandle)?;
    let receipt = crate::raw_development::raw_pipeline_receipt(prepared.raw_pipeline_receipt()?)?;
    let mut ffi_request = ffi_render_request(plan, max_edge, jpeg_quality);
    crate::completion_color_adaptation::bind(&mut ffi_request.nodes, plan, &receipt);
    let proxy = prepared.render_adjustment_plan(&ffi_request)?;
    Ok(proxy_payload(proxy))
}
