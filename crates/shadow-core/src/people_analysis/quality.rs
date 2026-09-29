//! Conservative admission for automatic people discovery, measured before SFace.
//!
//! This is a detail heuristic, not a face-recognition confidence or an aesthetic
//! score. Work is bounded to one 64-pixel face crop, excluding its outer edges so
//! sharp background objects do not rescue a defocused face.
use image::{DynamicImage, imageops::FilterType};
use shadow_ai::FaceBoundingBox;

pub(super) const ANALYSIS_REVISION: &str = "jpeg4096q90v1/people-quality-v1";
const MIN_FACE_EDGE: f32 = 40.0;
const DETAIL_EDGE: u32 = 64;

pub(super) fn portrait_quality(image: &DynamicImage, bounds: FaceBoundingBox) -> Option<f32> {
    if ![bounds.x, bounds.y, bounds.width, bounds.height]
        .iter()
        .all(|n| n.is_finite())
        || bounds.width <= 0.0
        || bounds.height <= 0.0
    {
        return None;
    }
    let left = bounds.x.max(0.0);
    let top = bounds.y.max(0.0);
    let right = (bounds.x + bounds.width).min(image.width() as f32);
    let bottom = (bounds.y + bounds.height).min(image.height() as f32);
    let width = right - left;
    let height = bottom - top;
    let edge = width.min(height);
    if edge < MIN_FACE_EDGE {
        return None;
    }
    let crop = image.crop_imm(
        (left + width * 0.12) as u32,
        (top + height * 0.12) as u32,
        (width * 0.76) as u32,
        (height * 0.76) as u32,
    );
    // Never upscale the measurement: interpolated pixels do not add face detail.
    let gray = crop
        .resize(DETAIL_EDGE, DETAIL_EDGE, FilterType::Triangle)
        .to_luma8();
    let (w, h) = gray.dimensions();
    let mut sum = 0.0_f64;
    let mut square_sum = 0.0_f64;
    let mut lap_sum = 0.0_f64;
    let mut lap_square_sum = 0.0_f64;
    let mut count = 0.0_f64;
    for y in 1..h - 1 {
        for x in 1..w - 1 {
            let value = f64::from(gray[(x, y)][0]);
            let lap = f64::from(gray[(x - 1, y)][0])
                + f64::from(gray[(x + 1, y)][0])
                + f64::from(gray[(x, y - 1)][0])
                + f64::from(gray[(x, y + 1)][0])
                - 4.0 * value;
            sum += value;
            square_sum += value * value;
            lap_sum += lap;
            lap_square_sum += lap * lap;
            count += 1.0;
        }
    }
    let variance = (square_sum / count - (sum / count).powi(2)).max(0.0);
    let detail = (lap_square_sum / count - (lap_sum / count).powi(2)).max(0.0);
    let relative_detail = detail / (variance + 64.0);
    // Both floors must pass: a flat/noisy patch and a high-contrast blur should
    // not be admitted merely because one of the two measurements is favorable.
    if detail < 9.0 || relative_detail < 0.018 {
        return None;
    }
    Some((relative_detail.min(1.0) * 0.75 + f64::from((edge / 256.0).min(1.0)) * 0.25) as f32)
}

#[cfg(test)]
mod tests;
