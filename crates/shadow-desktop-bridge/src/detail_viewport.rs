//! Validation and tile geometry for cancellable full-detail edit requests.

use super::*;

pub(super) const MAX_DETAIL_VIEWPORT_SIDE: u32 = 8_192;
const MAX_DETAIL_VIEWPORT_TILES: usize = 100;

pub(super) fn validate_detail_viewport_request(
    request: &ffi::FfiEditDetailViewportRequest,
) -> AnyResult<()> {
    if request.render_token == 0 {
        bail!("detail render token must be non-zero");
    }
    if !request.center_x.is_finite()
        || !request.center_y.is_finite()
        || !(0.0..=1.0).contains(&request.center_x)
        || !(0.0..=1.0).contains(&request.center_y)
    {
        bail!("detail viewport center must be finite and normalized to 0..=1");
    }
    if request.viewport_width == 0
        || request.viewport_height == 0
        || request.viewport_width > MAX_DETAIL_VIEWPORT_SIDE
        || request.viewport_height > MAX_DETAIL_VIEWPORT_SIDE
    {
        bail!("detail viewport dimensions must be in 1..=8192");
    }
    if request.tile_side == 0 || request.tile_side > MAX_EDIT_DETAIL_TILE_SIDE {
        bail!("detail tile side must be in 1..=1024");
    }
    let worst_case_axis_tiles = |viewport: u32| {
        // For an integer-aligned interval of length L against a fixed T grid,
        // max intersected cells = ceil((L - 1) / T) + 1.
        (u64::from(viewport) + u64::from(request.tile_side) - 2) / u64::from(request.tile_side) + 1
    };
    let worst_case_tiles = worst_case_axis_tiles(request.viewport_width)
        .checked_mul(worst_case_axis_tiles(request.viewport_height))
        .ok_or_else(|| anyhow!("detail viewport tile admission count overflowed"))?;
    if worst_case_tiles > u64::try_from(MAX_DETAIL_VIEWPORT_TILES).unwrap_or(u64::MAX) {
        bail!("detail viewport exceeds the 100-tile pre-decode admission bound");
    }
    Ok(())
}

fn detail_axis_span(full: u32, center: f64, viewport: u32) -> AnyResult<(u32, u32)> {
    if full == 0 {
        bail!("detail source dimension must be non-zero");
    }
    let span = viewport.min(full);
    let max_start = full - span;
    let centered = center * f64::from(full) - f64::from(span) / 2.0;
    let rounded_start = centered.round().clamp(0.0, f64::from(max_start));
    // The finite normalized-center precondition and clamp prove this value is
    // an integral number in the complete u32 range before conversion.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let start = rounded_start as u32;
    Ok((start, start + span))
}

pub(super) fn detail_viewport_rects(
    full: ImageDimensions,
    center_x: f64,
    center_y: f64,
    viewport_width: u32,
    viewport_height: u32,
    tile_side: u32,
) -> AnyResult<Vec<DetailTileRect>> {
    if !center_x.is_finite()
        || !center_y.is_finite()
        || !(0.0..=1.0).contains(&center_x)
        || !(0.0..=1.0).contains(&center_y)
        || tile_side == 0
        || tile_side > MAX_EDIT_DETAIL_TILE_SIDE
    {
        bail!("invalid detail viewport geometry");
    }
    let (left, right) = detail_axis_span(full.width, center_x, viewport_width)?;
    let (top, bottom) = detail_axis_span(full.height, center_y, viewport_height)?;
    let first_x = left / tile_side * tile_side;
    let first_y = top / tile_side * tile_side;
    let mut rects = Vec::new();
    let mut y = first_y;
    while y < bottom {
        let mut x = first_x;
        while x < right {
            rects.push(DetailTileRect {
                x,
                y,
                width: tile_side.min(full.width - x),
                height: tile_side.min(full.height - y),
            });
            if rects.len() > MAX_DETAIL_VIEWPORT_TILES {
                bail!("detail viewport exceeds the 100-tile admission bound");
            }
            x = x
                .checked_add(tile_side)
                .ok_or_else(|| anyhow!("detail tile x coordinate overflowed"))?;
        }
        y = y
            .checked_add(tile_side)
            .ok_or_else(|| anyhow!("detail tile y coordinate overflowed"))?;
    }

    // Rendering the center first improves cancellation latency once the
    // coordinator grows cancellable streaming. The v1 presentation remains
    // atomic: Qt receives the vector only after every visible tile is ready.
    let viewport_center = (
        center_x * f64::from(full.width),
        center_y * f64::from(full.height),
    );
    rects.sort_by(|left, right| {
        let distance = |rect: &DetailTileRect| {
            let dx = f64::from(rect.x) + f64::from(rect.width) / 2.0 - viewport_center.0;
            let dy = f64::from(rect.y) + f64::from(rect.height) / 2.0 - viewport_center.1;
            dx.mul_add(dx, dy * dy)
        };
        distance(left).total_cmp(&distance(right))
    });
    Ok(rects)
}
