//! Prompt-coordinate projection from the displayed final canvas back into the
//! original-image space where Grade Node masks are evaluated.

use shadow_ai::{MaskPointPolarity, MaskPromptPoint, RasterExtent, UnitInterval as AiUnitInterval};
use shadow_domain::{PhotoGeometry, PhotoQuarterTurn, UnitInterval};

/// Maps one normalized point from the cropped/oriented/straightened output
/// canvas into the original-image normalized coordinate space.
///
/// This mirrors the native renderer's half-pixel geometry convention. SAM is
/// therefore prompted against an identity-geometry render, and its generated
/// raster can enter the existing original-space local-mask pipeline without a
/// later lossy inverse warp.
pub(crate) fn map_output_prompt_to_original(
    point: MaskPromptPoint,
    geometry: PhotoGeometry,
    original_extent: RasterExtent,
) -> MaskPromptPoint {
    let source_width = f64::from(original_extent.width);
    let source_height = f64::from(original_extent.height);
    let crop_left = crop_start(source_width, geometry.crop_left().get());
    let crop_top = crop_start(source_height, geometry.crop_top().get());
    let crop_right = crop_end(source_width, geometry.crop_right().get());
    let crop_bottom = crop_end(source_height, geometry.crop_bottom().get());
    let crop_width = crop_right - crop_left;
    let crop_height = crop_bottom - crop_top;

    let transposed = matches!(
        geometry.quarter_turn(),
        PhotoQuarterTurn::Clockwise90 | PhotoQuarterTurn::Clockwise270
    );
    let oriented_width = if transposed { crop_height } else { crop_width };
    let oriented_height = if transposed { crop_width } else { crop_height };
    let (output_width, output_height) = auto_crop_extent(
        oriented_width,
        oriented_height,
        geometry.straighten_degrees(),
    );

    let angle = geometry.straighten_degrees().to_radians();
    let cosine = angle.cos();
    let sine = angle.sin();
    let horizontal_offset = point.x.get().mul_add(output_width, -output_width * 0.5);
    let vertical_offset = point.y.get().mul_add(output_height, -output_height * 0.5);
    let mut oriented_x =
        cosine.mul_add(horizontal_offset, sine * vertical_offset) + oriented_width * 0.5;
    let mut oriented_y =
        (-sine).mul_add(horizontal_offset, cosine * vertical_offset) + oriented_height * 0.5;
    if geometry.perspective_vertical() != 0.0 || geometry.perspective_horizontal() != 0.0 {
        let perspective = apply_perspective(
            (
                oriented_x / oriented_width * 2.0 - 1.0,
                oriented_y / oriented_height * 2.0 - 1.0,
            ),
            geometry.perspective_vertical(),
            geometry.perspective_horizontal(),
        );
        oriented_x = (perspective.0 + 1.0) * 0.5 * oriented_width;
        oriented_y = (perspective.1 + 1.0) * 0.5 * oriented_height;
    }

    let (mut crop_x, mut crop_y) = match geometry.quarter_turn() {
        PhotoQuarterTurn::Zero => (oriented_x, oriented_y),
        PhotoQuarterTurn::Clockwise90 => (oriented_y, crop_height - oriented_x),
        PhotoQuarterTurn::Clockwise180 => (crop_width - oriented_x, crop_height - oriented_y),
        PhotoQuarterTurn::Clockwise270 => (crop_width - oriented_y, oriented_x),
    };
    if geometry.flip_horizontal() {
        crop_x = crop_width - crop_x;
    }
    if geometry.flip_vertical() {
        crop_y = crop_height - crop_y;
    }

    let normalized_x = ((crop_left + crop_x) / source_width).clamp(0.0, 1.0);
    let normalized_y = ((crop_top + crop_y) / source_height).clamp(0.0, 1.0);
    MaskPromptPoint {
        x: AiUnitInterval::new(normalized_x).expect("clamped original-space prompt x is valid"),
        y: AiUnitInterval::new(normalized_y).expect("clamped original-space prompt y is valid"),
        polarity: point.polarity,
    }
}

/// Resamples an original-space Gray8 mask into the normalized final-canvas
/// space used by the prompt overlay.
///
/// The output is presentation-only. Durable rendering continues to read the
/// original immutable raster and evaluate the same geometry natively.
pub(crate) fn project_gray8_mask_to_output(
    samples: &[u8],
    raster_extent: RasterExtent,
    coordinate_extent: RasterExtent,
    geometry: PhotoGeometry,
    output_extent: RasterExtent,
) -> Option<Vec<u8>> {
    let raster_len = usize::try_from(
        u64::from(raster_extent.width).checked_mul(u64::from(raster_extent.height))?,
    )
    .ok()?;
    let output_len = usize::try_from(
        u64::from(output_extent.width).checked_mul(u64::from(output_extent.height))?,
    )
    .ok()?;
    if samples.len() != raster_len {
        return None;
    }

    let mut output = Vec::with_capacity(output_len);
    for row in 0..output_extent.height {
        let output_y = (f64::from(row) + 0.5) / f64::from(output_extent.height);
        for column in 0..output_extent.width {
            let output_x = (f64::from(column) + 0.5) / f64::from(output_extent.width);
            let original = map_output_prompt_to_original(
                MaskPromptPoint {
                    x: AiUnitInterval::new(output_x).ok()?,
                    y: AiUnitInterval::new(output_y).ok()?,
                    polarity: MaskPointPolarity::Foreground,
                },
                geometry,
                coordinate_extent,
            );
            output.push(sample_gray8_bilinear(
                samples,
                raster_extent,
                original.x.get(),
                original.y.get(),
            ));
        }
    }
    Some(output)
}

/// Projects one original-space completion patch into a full-canvas RGBA8
/// presentation raster. This is transient candidate UI only; accepted render
/// bytes continue through the native Recipe node.
#[allow(clippy::too_many_arguments)]
pub(crate) fn project_rgba8_patch_to_output(
    rgba8: &[u8],
    raster_extent: RasterExtent,
    coordinate_extent: RasterExtent,
    bounds_left: UnitInterval,
    bounds_top: UnitInterval,
    bounds_right: UnitInterval,
    bounds_bottom: UnitInterval,
    geometry: PhotoGeometry,
    output_extent: RasterExtent,
) -> Option<Vec<u8>> {
    let raster_len = usize::try_from(
        u64::from(raster_extent.width)
            .checked_mul(u64::from(raster_extent.height))?
            .checked_mul(4)?,
    )
    .ok()?;
    let output_len = usize::try_from(
        u64::from(output_extent.width)
            .checked_mul(u64::from(output_extent.height))?
            .checked_mul(4)?,
    )
    .ok()?;
    if rgba8.len() != raster_len || bounds_left >= bounds_right || bounds_top >= bounds_bottom {
        return None;
    }
    let mut output = vec![0_u8; output_len];
    for row in 0..output_extent.height {
        let output_y = (f64::from(row) + 0.5) / f64::from(output_extent.height);
        for column in 0..output_extent.width {
            let output_x = (f64::from(column) + 0.5) / f64::from(output_extent.width);
            let original = map_output_prompt_to_original(
                MaskPromptPoint {
                    x: AiUnitInterval::new(output_x).ok()?,
                    y: AiUnitInterval::new(output_y).ok()?,
                    polarity: MaskPointPolarity::Foreground,
                },
                geometry,
                coordinate_extent,
            );
            if original.x.get() < bounds_left.get()
                || original.x.get() >= bounds_right.get()
                || original.y.get() < bounds_top.get()
                || original.y.get() >= bounds_bottom.get()
            {
                continue;
            }
            let patch_x =
                (original.x.get() - bounds_left.get()) / (bounds_right.get() - bounds_left.get());
            let patch_y =
                (original.y.get() - bounds_top.get()) / (bounds_bottom.get() - bounds_top.get());
            let sample_x =
                ((patch_x * f64::from(raster_extent.width)) as u32).min(raster_extent.width - 1);
            let sample_y =
                ((patch_y * f64::from(raster_extent.height)) as u32).min(raster_extent.height - 1);
            let source = usize::try_from(
                (u64::from(sample_y) * u64::from(raster_extent.width) + u64::from(sample_x)) * 4,
            )
            .ok()?;
            let target = usize::try_from(
                (u64::from(row) * u64::from(output_extent.width) + u64::from(column)) * 4,
            )
            .ok()?;
            output[target..target + 4].copy_from_slice(&rgba8[source..source + 4]);
        }
    }
    Some(output)
}

// The coordinates are clamped to finite, non-negative raster bounds before
// these integer conversions, and the final sample is clamped to Gray8.
#[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
fn sample_gray8_bilinear(
    samples: &[u8],
    extent: RasterExtent,
    normalized_x: f64,
    normalized_y: f64,
) -> u8 {
    let width = f64::from(extent.width);
    let height = f64::from(extent.height);
    let raster_x = (normalized_x.clamp(0.0, 1.0) * width - 0.5).clamp(0.0, width - 1.0);
    let raster_y = (normalized_y.clamp(0.0, 1.0) * height - 0.5).clamp(0.0, height - 1.0);
    let x0 = raster_x.floor() as u32;
    let y0 = raster_y.floor() as u32;
    let x1 = (x0 + 1).min(extent.width - 1);
    let y1 = (y0 + 1).min(extent.height - 1);
    let fraction_x = raster_x - f64::from(x0);
    let fraction_y = raster_y - f64::from(y0);
    let at = |x: u32, y: u32| {
        f64::from(
            samples[usize::try_from(u64::from(y) * u64::from(extent.width) + u64::from(x))
                .expect("validated raster index fits usize")],
        )
    };
    let top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fraction_x;
    let bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fraction_x;
    (top + (bottom - top) * fraction_y)
        .round()
        .clamp(0.0, 255.0) as u8
}

fn crop_start(source_extent: f64, normalized: f64) -> f64 {
    (normalized * source_extent)
        .floor()
        .clamp(0.0, source_extent - 1.0)
}

fn crop_end(source_extent: f64, normalized: f64) -> f64 {
    (normalized * source_extent)
        .ceil()
        .clamp(1.0, source_extent)
}

fn auto_crop_extent(width: f64, height: f64, straighten_degrees: f64) -> (f64, f64) {
    if straighten_degrees == 0.0 {
        return (width, height);
    }
    let angle = straighten_degrees.to_radians();
    let cosine = angle.cos().abs();
    let sine = angle.sin().abs();
    let scale = (width / cosine.mul_add(width, sine * height))
        .min(height / sine.mul_add(width, cosine * height));
    (
        (width * scale).floor().max(1.0),
        (height * scale).floor().max(1.0),
    )
}

fn apply_perspective(mut coordinate: (f64, f64), vertical: f64, horizontal: f64) -> (f64, f64) {
    if vertical == 0.0 && horizontal == 0.0 {
        return coordinate;
    }
    let edge_scales = |amount: f64| {
        let reduced = 1.0 - 0.5 * amount.abs();
        if amount >= 0.0 {
            (reduced, 1.0)
        } else {
            (1.0, reduced)
        }
    };

    let vertical_scales = edge_scales(vertical);
    let vertical_sum = vertical_scales.0 + vertical_scales.1;
    let vertical_c = (vertical_scales.0 - vertical_scales.1) / vertical_sum;
    let vertical_k = 2.0 * vertical_scales.0 * vertical_scales.1 / vertical_sum;
    let vertical_denominator = 1.0 + vertical_c * coordinate.1;
    coordinate = (
        vertical_k * coordinate.0 / vertical_denominator,
        (coordinate.1 + vertical_c) / vertical_denominator,
    );

    let horizontal_scales = edge_scales(horizontal);
    let horizontal_sum = horizontal_scales.0 + horizontal_scales.1;
    let horizontal_c = (horizontal_scales.0 - horizontal_scales.1) / horizontal_sum;
    let horizontal_k = 2.0 * horizontal_scales.0 * horizontal_scales.1 / horizontal_sum;
    let horizontal_denominator = 1.0 + horizontal_c * coordinate.0;
    (
        (coordinate.0 + horizontal_c) / horizontal_denominator,
        horizontal_k * coordinate.1 / horizontal_denominator,
    )
}

#[cfg(test)]
mod tests;
