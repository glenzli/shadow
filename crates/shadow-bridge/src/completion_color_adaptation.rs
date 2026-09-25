//! Small render-only source-response binding; accepted raster bytes never change.
//! RAW remains CFA/DCP-developed normally. This matrix only transports generated RGB.
use crate::{AdjustmentRenderOperation, AdjustmentRenderPlan, RawPipelineReceipt, ffi};
use shadow_domain::ImageCompletionColorBasis;

pub(crate) const IDENTITY: [f64; 9] = [1., 0., 0., 0., 1., 0., 0., 0., 1.];

pub(crate) fn response(
    source: &ImageCompletionColorBasis,
    current: &ImageCompletionColorBasis,
) -> Option<[f64; 9]> {
    if !source.valid() || !current.valid() || source.calibration_id != current.calibration_id {
        return None;
    }
    if source == current {
        return Some(IDENTITY);
    }
    let a = source.matrix();
    let b = current.matrix();
    let d = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6])
        + a[2] * (a[3] * a[7] - a[4] * a[6]);
    let inv = [
        a[4] * a[8] - a[5] * a[7],
        a[2] * a[7] - a[1] * a[8],
        a[1] * a[5] - a[2] * a[4],
        a[5] * a[6] - a[3] * a[8],
        a[0] * a[8] - a[2] * a[6],
        a[2] * a[3] - a[0] * a[5],
        a[3] * a[7] - a[4] * a[6],
        a[1] * a[6] - a[0] * a[7],
        a[0] * a[4] - a[1] * a[3],
    ]
    .map(|v| v / d);
    let out = std::array::from_fn(|i| {
        (0..3)
            .map(|k| b[(i / 3) * 3 + k] * inv[k * 3 + i % 3])
            .sum::<f64>()
    });
    out.iter()
        .all(|v| v.is_finite() && v.abs() <= 64.)
        .then_some(out)
}

pub(crate) fn bind(
    nodes: &mut [ffi::FfiAdjustmentNode],
    plan: &AdjustmentRenderPlan,
    receipt: &RawPipelineReceipt,
) {
    let Some(current) = &receipt.completion_color_basis else {
        return;
    };
    for (node, typed) in nodes.iter_mut().zip(&plan.nodes) {
        let AdjustmentRenderOperation::ImageCompletion { patches } = &typed.operation else {
            continue;
        };
        for (fields, patch) in node.parameters.chunks_exact_mut(19).zip(patches) {
            if !patch.linear_rgba_f32 {
                continue;
            }
            if let Some(matrix) = patch
                .source_color_basis
                .as_ref()
                .and_then(|source| response(source, current))
            {
                fields[10..19].copy_from_slice(&matrix);
            }
        }
    }
}

#[cfg(test)]
mod tests;
