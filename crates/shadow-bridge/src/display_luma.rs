//! Bounded JPEG display-luma preprocessing for analysis consumers.
//!
//! This contract is display-referred and versioned independently from RAW development and edit
//! rendering.

use super::{BridgeError, ffi};

/// Hard longest-edge bound for a JPEG display-luma analysis plane.
pub const MAX_JPEG_DISPLAY_LUMA_EDGE: u32 = 512;

/// Stable semantic prefix returned by the JPEG display-luma preprocessor.
///
/// The complete returned version appends `:max-edge-N`, because sharpness
/// observations from different analysis scales are not directly comparable.
pub const JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX: &str = concat!(
    "shadow.jpeg-luma.v1:libjpeg-turbo-",
    env!("SHADOW_LIBJPEG_TURBO_VERSION"),
    ":rgb8:islow:no-fancy-upsampling:no-block-smoothing:assume-srgb:ignore-icc:",
    "stored-orientation:idct-scale-1-2-4-8:bilinear-center-q16:rec709-encoded-q16"
);

/// Owned normalized display-referred luminance decoded from a JPEG proxy.
///
/// This is not RAW sensor luminance. The exact JPEG/color/resize assumptions
/// are carried in [`DecodedDisplayLuma::preprocessing_version`]. `stride` is
/// measured in `f32` samples and is currently always equal to `width`.
#[derive(Debug, Clone, PartialEq)]
pub struct DecodedDisplayLuma {
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub samples: Vec<f32>,
    pub preprocessing_version: String,
}

/// Decodes JPEG bytes into a bounded, tightly packed normalized display-luma
/// plane suitable for `shadow_ai::DisplayLumaPlane`.
///
/// The pipeline uses version-pinned libjpeg-turbo RGB8 output with the integer
/// slow DCT, fancy upsampling and block smoothing disabled. It does not apply
/// ICC profiles or EXIF orientation, assumes encoded sRGB, applies fixed-point
/// Rec.709 luma, selects one of libjpeg's 1/2/4/8 IDCT scales, and
/// deterministically resizes to `max_edge`. Truncated JPEG warnings are
/// rejected rather than repaired.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidDisplayLumaRequest`] unless `max_edge` is in
/// `1..=512`, or [`BridgeError::Decoder`] for corrupt, unsupported, or
/// resource-limited JPEG data.
pub fn decode_jpeg_display_luma(
    encoded: &[u8],
    max_edge: u32,
) -> Result<DecodedDisplayLuma, BridgeError> {
    if !(1..=MAX_JPEG_DISPLAY_LUMA_EDGE).contains(&max_edge) {
        return Err(BridgeError::InvalidDisplayLumaRequest(
            "max_edge must be in 1..=512",
        ));
    }

    let decoded = ffi::decode_jpeg_display_luma(encoded, max_edge)?;
    if decoded.width == 0
        || decoded.height == 0
        || decoded.width > max_edge
        || decoded.height > max_edge
        || decoded.stride != decoded.width
    {
        return Err(BridgeError::InvalidDisplayLumaOutput(
            "dimensions or stride violate the bounded plane contract",
        ));
    }
    let expected_len = usize::try_from(decoded.stride)
        .ok()
        .and_then(|stride| {
            usize::try_from(decoded.height)
                .ok()
                .and_then(|height| stride.checked_mul(height))
        })
        .ok_or(BridgeError::InvalidDisplayLumaOutput(
            "plane length overflows addressable memory",
        ))?;
    if decoded.samples.len() != expected_len {
        return Err(BridgeError::InvalidDisplayLumaOutput(
            "sample length does not match stride times height",
        ));
    }
    if decoded
        .samples
        .iter()
        .any(|sample| !sample.is_finite() || !(0.0..=1.0).contains(sample))
    {
        return Err(BridgeError::InvalidDisplayLumaOutput(
            "samples must be finite and normalized",
        ));
    }
    let expected_version =
        format!("{JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX}:max-edge-{max_edge}");
    if decoded.preprocessing_version != expected_version {
        return Err(BridgeError::InvalidDisplayLumaOutput(
            "preprocessing version does not match the requested scale",
        ));
    }

    Ok(DecodedDisplayLuma {
        width: decoded.width,
        height: decoded.height,
        stride: decoded.stride,
        samples: decoded.samples,
        preprocessing_version: decoded.preprocessing_version,
    })
}
