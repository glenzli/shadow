//! Bounded local model input preparation; Catalog source identity remains separate.
use image::{ImageReader, codecs::jpeg::JpegEncoder};
use shadow_domain::ImageDimensions;
use std::{borrow::Cow, io::Cursor};

pub(crate) struct PreparedVisionInput<'a> {
    pub bytes: Cow<'a, [u8]>,
    pub dimensions: ImageDimensions,
}

pub(crate) fn bounded_jpeg(
    bytes: &[u8],
    dimensions: ImageDimensions,
    maximum_edge: u32,
) -> Result<PreparedVisionInput<'_>, String> {
    if dimensions.width.max(dimensions.height) <= maximum_edge && bytes.len() <= 20 * 1024 * 1024 {
        return Ok(PreparedVisionInput {
            bytes: Cow::Borrowed(bytes),
            dimensions,
        });
    }
    let mut reader = ImageReader::with_format(Cursor::new(bytes), image::ImageFormat::Jpeg);
    let mut limits = image::Limits::default();
    limits.max_alloc = Some(512 * 1024 * 1024);
    reader.limits(limits);
    let decoded = reader
        .decode()
        .map_err(|e| format!("decode bounded vision input: {e}"))?;
    if decoded.width() != dimensions.width || decoded.height() != dimensions.height {
        return Err("vision input dimensions disagree with the Catalog".into());
    }
    let scaled = decoded.thumbnail(maximum_edge, maximum_edge);
    let mut encoded = Vec::new();
    JpegEncoder::new_with_quality(&mut encoded, 90)
        .encode_image(&scaled)
        .map_err(|e| format!("encode bounded vision input: {e}"))?;
    Ok(PreparedVisionInput {
        bytes: Cow::Owned(encoded),
        dimensions: ImageDimensions {
            width: scaled.width(),
            height: scaled.height(),
        },
    })
}

#[cfg(test)]
mod tests;
