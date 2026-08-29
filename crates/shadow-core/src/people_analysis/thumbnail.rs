//! Bounded request-local face crops for the People session projection.

use std::io::Cursor;

use image::{DynamicImage, codecs::jpeg::JpegEncoder, imageops::FilterType};
use shadow_ai::FaceBoundingBox;

const PEOPLE_THUMBNAIL_EDGE: u32 = 88;
const PEOPLE_THUMBNAIL_JPEG_QUALITY: u8 = 82;
const MAX_PEOPLE_THUMBNAIL_BYTES: usize = 64 * 1_024;
const MAX_THUMBNAIL_SOURCE_PIXELS: u64 = 16_777_216;
pub(super) const MAX_RESIDENT_PEOPLE_THUMBNAIL_BYTES: usize = 16 * 1_024 * 1_024;

pub(super) fn source_dimensions_admitted(width: u32, height: u32) -> bool {
    width > 0
        && height > 0
        && u64::from(width).saturating_mul(u64::from(height)) <= MAX_THUMBNAIL_SOURCE_PIXELS
}

pub(super) fn face_thumbnail_jpeg(
    image: &DynamicImage,
    bounds: FaceBoundingBox,
) -> Option<Vec<u8>> {
    let image_width = image.width() as f32;
    let image_height = image.height() as f32;
    if image_width <= 0.0 || image_height <= 0.0 {
        return None;
    }
    let center_x = bounds.x + bounds.width * 0.5;
    let center_y = bounds.y + bounds.height * 0.5;
    let side = bounds.width.max(bounds.height).mul_add(1.6, 0.0).max(1.0);
    let left = (center_x - side * 0.5)
        .floor()
        .clamp(0.0, image_width - 1.0);
    let top = (center_y - side * 0.5)
        .floor()
        .clamp(0.0, image_height - 1.0);
    let right = (center_x + side * 0.5)
        .ceil()
        .clamp(left + 1.0, image_width);
    let bottom = (center_y + side * 0.5)
        .ceil()
        .clamp(top + 1.0, image_height);
    let crop = image.crop_imm(
        left as u32,
        top as u32,
        (right - left) as u32,
        (bottom - top) as u32,
    );
    let thumbnail = crop.resize_to_fill(
        PEOPLE_THUMBNAIL_EDGE,
        PEOPLE_THUMBNAIL_EDGE,
        FilterType::Triangle,
    );
    let mut encoded = Vec::new();
    JpegEncoder::new_with_quality(Cursor::new(&mut encoded), PEOPLE_THUMBNAIL_JPEG_QUALITY)
        .encode_image(&thumbnail)
        .ok()?;
    (!encoded.is_empty() && encoded.len() <= MAX_PEOPLE_THUMBNAIL_BYTES).then_some(encoded)
}

#[cfg(test)]
mod tests;
