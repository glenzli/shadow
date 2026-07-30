//! Strict projection of the flat Qt Liquify DTO into authored Recipe intent.

use anyhow::{Context, Result as AnyResult, bail};
use shadow_domain::{
    LiquifyPoint, LiquifyStroke, MAX_LIQUIFY_POINTS_PER_STROKE, MAX_LIQUIFY_STROKES_PER_NODE,
    PhotoLiquifyNode, UnitInterval,
};

use crate::ffi;

pub(super) fn photo_liquify_from_ffi(
    strokes: &[ffi::FfiLiquifyPushStroke],
) -> AnyResult<Option<PhotoLiquifyNode>> {
    if strokes.is_empty() {
        return Ok(None);
    }
    if strokes.len() > MAX_LIQUIFY_STROKES_PER_NODE {
        bail!("Liquify supports at most {MAX_LIQUIFY_STROKES_PER_NODE} strokes");
    }

    let strokes = strokes
        .iter()
        .enumerate()
        .map(|(stroke_index, stroke)| {
            if stroke.points.len() > MAX_LIQUIFY_POINTS_PER_STROKE {
                bail!(
                    "Liquify stroke {stroke_index} supports at most \
                     {MAX_LIQUIFY_POINTS_PER_STROKE} points"
                );
            }
            let points = stroke
                .points
                .iter()
                .enumerate()
                .map(|(point_index, point)| {
                    Ok(LiquifyPoint::with_pressure(
                        unit(stroke_index, point_index, "x", point.x)?,
                        unit(stroke_index, point_index, "y", point.y)?,
                        unit(stroke_index, point_index, "pressure", point.pressure)?,
                    ))
                })
                .collect::<AnyResult<Vec<_>>>()?;
            LiquifyStroke::push(
                points,
                UnitInterval::new(stroke.radius).with_context(|| {
                    format!("Liquify stroke {stroke_index} radius must be in [0, 1]")
                })?,
                UnitInterval::new(stroke.strength).with_context(|| {
                    format!("Liquify stroke {stroke_index} strength must be in [0, 1]")
                })?,
                UnitInterval::new(stroke.hardness).with_context(|| {
                    format!("Liquify stroke {stroke_index} hardness must be in [0, 1]")
                })?,
            )
            .with_context(|| format!("Liquify stroke {stroke_index} is invalid"))
        })
        .collect::<AnyResult<Vec<_>>>()?;
    Ok(Some(
        PhotoLiquifyNode::new(strokes).context("Liquify node is invalid")?,
    ))
}

fn unit(
    stroke_index: usize,
    point_index: usize,
    field: &str,
    value: f64,
) -> AnyResult<UnitInterval> {
    UnitInterval::new(value).with_context(|| {
        format!("Liquify stroke {stroke_index} point {point_index} {field} must be in [0, 1]")
    })
}
