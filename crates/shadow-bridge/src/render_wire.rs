//! Typed adjustment, geometry, tile, and encoded-proxy mappings for the flat CXX wire.

use super::{
    adjustment::{
        AdjustmentDetailEffectsPass, AdjustmentGeometry, AdjustmentLiquify,
        AdjustmentLiquifyStroke, AdjustmentLocalMask, AdjustmentMaskComponentOperation,
        AdjustmentQuarterTurn, AdjustmentRasterMaskEncoding, AdjustmentRenderNode,
        AdjustmentRenderOperation, AdjustmentRenderPlan, OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT,
    },
    decoder::{dimensions, preview_codec},
    detail_session::{DetailTileRect, DetailTileRequest},
    ffi,
    preview_analysis::EditPreviewMaskCoverageRequest,
};

pub(super) fn ffi_render_request(
    plan: &AdjustmentRenderPlan,
    max_edge: u32,
    jpeg_quality: u8,
) -> ffi::FfiAdjustmentRenderRequest {
    ffi_render_request_with_mask_coverage(plan, max_edge, jpeg_quality, None)
}

pub(super) fn ffi_render_request_with_mask_coverage(
    plan: &AdjustmentRenderPlan,
    max_edge: u32,
    jpeg_quality: u8,
    mask_coverage: Option<EditPreviewMaskCoverageRequest>,
) -> ffi::FfiAdjustmentRenderRequest {
    ffi::FfiAdjustmentRenderRequest {
        nodes: plan.nodes.iter().map(ffi_render_node).collect(),
        liquify: ffi_photo_liquify(plan.liquify.as_ref()),
        geometry: ffi_photo_geometry(plan.geometry),
        max_edge,
        jpeg_quality,
        mask_coverage_requested: mask_coverage.is_some(),
        mask_coverage_target_layer_index: mask_coverage.map_or(0, |value| value.target_layer_index),
        mask_coverage_component_requested: mask_coverage
            .and_then(|value| value.target_component_index)
            .is_some(),
        mask_coverage_target_component_index: mask_coverage
            .and_then(|value| value.target_component_index)
            .unwrap_or(0),
    }
}

pub(super) fn ffi_detail_tile_request(
    plan: &AdjustmentRenderPlan,
    request: DetailTileRequest,
) -> ffi::FfiAdjustmentDetailTileRequest {
    ffi::FfiAdjustmentDetailTileRequest {
        nodes: plan.nodes.iter().map(ffi_render_node).collect(),
        liquify: ffi_photo_liquify(plan.liquify.as_ref()),
        geometry: ffi_photo_geometry(plan.geometry),
        rect: ffi_detail_tile_rect(request.rect),
    }
}

fn ffi_photo_liquify(liquify: Option<&AdjustmentLiquify>) -> ffi::FfiPhotoLiquify {
    let Some(liquify) = liquify else {
        return ffi::FfiPhotoLiquify {
            present: false,
            enabled: false,
            points: Vec::new(),
            stroke_kinds: Vec::new(),
            stroke_point_counts: Vec::new(),
            stroke_parameters: Vec::new(),
        };
    };
    let point_count = liquify
        .strokes
        .iter()
        .map(|stroke| match stroke {
            AdjustmentLiquifyStroke::Push(stroke) => stroke.points.len(),
            AdjustmentLiquifyStroke::Reconstruct(stroke) => stroke.points.len(),
        })
        .sum();
    let mut points = Vec::with_capacity(point_count);
    let mut stroke_kinds = Vec::with_capacity(liquify.strokes.len());
    let mut stroke_point_counts = Vec::with_capacity(liquify.strokes.len());
    let mut stroke_parameters = Vec::with_capacity(liquify.strokes.len() * 3);
    for stroke in &liquify.strokes {
        let (kind, stroke_points, radius, strength, hardness) = match stroke {
            AdjustmentLiquifyStroke::Push(stroke) => (
                0,
                stroke.points.as_slice(),
                stroke.radius,
                stroke.strength,
                stroke.hardness,
            ),
            AdjustmentLiquifyStroke::Reconstruct(stroke) => (
                1,
                stroke.points.as_slice(),
                stroke.radius,
                stroke.strength,
                stroke.hardness,
            ),
        };
        stroke_kinds.push(kind);
        stroke_point_counts.push(
            u32::try_from(stroke_points.len()).expect("validated Liquify point count fits u32"),
        );
        points.extend(stroke_points.iter().map(|point| ffi::FfiPhotoLiquifyPoint {
            x: point.x,
            y: point.y,
            pressure: point.pressure,
        }));
        stroke_parameters.extend([radius, strength, hardness]);
    }
    ffi::FfiPhotoLiquify {
        present: true,
        enabled: liquify.enabled,
        points,
        stroke_kinds,
        stroke_point_counts,
        stroke_parameters,
    }
}

const fn ffi_photo_geometry(geometry: AdjustmentGeometry) -> ffi::FfiPhotoGeometry {
    ffi::FfiPhotoGeometry {
        crop_left: geometry.crop_left,
        crop_top: geometry.crop_top,
        crop_right: geometry.crop_right,
        crop_bottom: geometry.crop_bottom,
        quarter_turn: match geometry.quarter_turn {
            AdjustmentQuarterTurn::Zero => 0,
            AdjustmentQuarterTurn::Clockwise90 => 1,
            AdjustmentQuarterTurn::Clockwise180 => 2,
            AdjustmentQuarterTurn::Clockwise270 => 3,
        },
        straighten_degrees: geometry.straighten_degrees,
        perspective_vertical: geometry.perspective_vertical,
        perspective_horizontal: geometry.perspective_horizontal,
        flip_horizontal: geometry.flip_horizontal,
        flip_vertical: geometry.flip_vertical,
    }
}

const fn ffi_detail_tile_rect(rect: DetailTileRect) -> ffi::FfiDetailTileRect {
    ffi::FfiDetailTileRect {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
    }
}

pub(super) const fn detail_tile_rect(rect: ffi::FfiDetailTileRect) -> DetailTileRect {
    DetailTileRect {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
    }
}

fn ffi_local_mask_leaf(mask: &AdjustmentLocalMask) -> (Vec<f64>, Vec<u32>, Vec<u8>) {
    let (kind, x0, y0, x1, y1, radius_x, radius_y, feather, invert, brush_points, payload) =
        match mask {
            AdjustmentLocalMask::LinearGradient {
                start_x,
                start_y,
                end_x,
                end_y,
                invert,
            } => (
                1.0,
                *start_x,
                *start_y,
                *end_x,
                *end_y,
                0.0,
                0.0,
                0.0,
                if *invert { 1.0 } else { 0.0 },
                Vec::new(),
                Vec::new(),
            ),
            AdjustmentLocalMask::RadialGradient {
                center_x,
                center_y,
                radius_x,
                radius_y,
                feather,
                invert,
            } => (
                2.0,
                *center_x,
                *center_y,
                0.0,
                0.0,
                *radius_x,
                *radius_y,
                *feather,
                if *invert { 1.0 } else { 0.0 },
                Vec::new(),
                Vec::new(),
            ),
            AdjustmentLocalMask::Brush {
                points,
                radius,
                feather,
                invert,
            } => (
                3.0,
                0.0,
                0.0,
                0.0,
                0.0,
                *radius,
                0.0,
                *feather,
                if *invert { 1.0 } else { 0.0 },
                points
                    .iter()
                    .flat_map(|point| {
                        [
                            point.x,
                            point.y,
                            if point.begins_stroke { 1.0 } else { 0.0 },
                        ]
                    })
                    .collect(),
                Vec::new(),
            ),
            AdjustmentLocalMask::LuminanceRange {
                lower,
                upper,
                softness,
                invert,
            } => (
                4.0,
                *lower,
                0.0,
                *upper,
                0.0,
                0.0,
                0.0,
                *softness,
                if *invert { 1.0 } else { 0.0 },
                Vec::new(),
                Vec::new(),
            ),
            AdjustmentLocalMask::ColorRange {
                center_hue_degrees,
                width_degrees,
                softness,
                invert,
            } => (
                5.0,
                *center_hue_degrees / 360.0,
                0.0,
                *width_degrees / 180.0,
                0.0,
                0.0,
                0.0,
                *softness,
                if *invert { 1.0 } else { 0.0 },
                Vec::new(),
                Vec::new(),
            ),
            AdjustmentLocalMask::ManagedRaster {
                raster_width,
                raster_height,
                coordinate_width,
                coordinate_height,
                encoding,
                samples,
                expansion,
                feather,
                invert,
            } => (
                6.0,
                f64::from(*raster_width),
                f64::from(*raster_height),
                f64::from(*coordinate_width),
                f64::from(*coordinate_height),
                match encoding {
                    AdjustmentRasterMaskEncoding::Gray8 => 1.0,
                    AdjustmentRasterMaskEncoding::Gray16Float => 2.0,
                },
                *expansion,
                *feather,
                if *invert { 1.0 } else { 0.0 },
                Vec::new(),
                samples.clone(),
            ),
            AdjustmentLocalMask::Composite { .. } => {
                unreachable!("validated composite local masks may not nest")
            }
        };
    let point_count =
        u32::try_from(brush_points.len() / 3).expect("validated brush point count fits in u32");
    let mut parameters = vec![kind, x0, y0, x1, y1, radius_x, radius_y, feather, invert];
    parameters.extend(brush_points);
    (
        parameters,
        if kind == 3.0 {
            vec![point_count]
        } else {
            vec![]
        },
        payload,
    )
}

fn ffi_local_mask_component(
    component: &super::adjustment::AdjustmentLocalMaskComponent,
) -> ffi::FfiAdjustmentMaskComponent {
    let (parameters, parameter_group_lengths, payload) = ffi_local_mask_leaf(&component.mask);
    ffi::FfiAdjustmentMaskComponent {
        operation: match component.operation {
            AdjustmentMaskComponentOperation::Base => 0,
            AdjustmentMaskComponentOperation::Add => 1,
            AdjustmentMaskComponentOperation::Subtract => 2,
            AdjustmentMaskComponentOperation::Intersect => 3,
        },
        enabled: component.enabled,
        parameters,
        payload,
        parameter_group_lengths,
    }
}

// The flat CXX wire record is intentionally assembled in one auditable operation.
#[allow(clippy::too_many_lines)]
pub(crate) fn ffi_render_node(node: &AdjustmentRenderNode) -> ffi::FfiAdjustmentNode {
    let mut mask_components = Vec::new();
    let mut mask_final_invert = false;
    let (operation, parameters, parameter_group_lengths, payload) = match &node.operation {
        AdjustmentRenderOperation::LocalMaskLayerStart { opacity, mask } => {
            let (parameters, parameter_group_lengths, payload) = match mask {
                None => (
                    vec![*opacity, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
                    vec![],
                    vec![],
                ),
                Some(AdjustmentLocalMask::Composite { components, invert }) => {
                    mask_components = components.iter().map(ffi_local_mask_component).collect();
                    mask_final_invert = *invert;
                    (vec![*opacity], vec![], vec![])
                }
                Some(mask) => {
                    let (mut parameters, groups, payload) = ffi_local_mask_leaf(mask);
                    parameters.insert(0, *opacity);
                    (parameters, groups, payload)
                }
            };
            (
                ffi::FfiAdjustmentOperation::LocalMaskLayerStart,
                parameters,
                parameter_group_lengths,
                payload,
            )
        }
        AdjustmentRenderOperation::LocalMaskLayerEnd => (
            ffi::FfiAdjustmentOperation::LocalMaskLayerEnd,
            vec![],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::Exposure { stops } => (
            ffi::FfiAdjustmentOperation::Exposure,
            vec![*stops],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::Contrast { factor, pivot } => (
            ffi::FfiAdjustmentOperation::Contrast,
            vec![*factor, *pivot],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::OklabLightnessToneCurve { curve } => (
            ffi::FfiAdjustmentOperation::OklabLightnessToneCurve,
            curve
                .lightness
                .iter()
                .flat_map(|point| [point.x, point.y])
                .collect(),
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::RgbWhiteBalance { temperature, tint } => (
            ffi::FfiAdjustmentOperation::RgbWhiteBalance,
            vec![*temperature, *tint],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::Saturation { factor } => (
            ffi::FfiAdjustmentOperation::Saturation,
            vec![*factor],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::SelectiveTone { parameters } => (
            ffi::FfiAdjustmentOperation::SelectiveTone,
            vec![
                parameters.highlights,
                parameters.shadows,
                parameters.whites,
                parameters.blacks,
                parameters.highlight_red_suppression,
                parameters.highlight_green_suppression,
                parameters.highlight_blue_suppression,
            ],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::PerceptualColor { parameters } => {
            let mut flattened =
                Vec::with_capacity(72 + parameters.additional_color_ranges.len() * 7);
            flattened.push(parameters.vibrance);
            flattened.extend(parameters.hue_shifts);
            flattened.extend(parameters.saturation);
            flattened.extend(parameters.lightness);
            flattened.extend([
                if parameters.color_range.enabled {
                    1.0
                } else {
                    0.0
                },
                parameters.color_range.center_hue_degrees,
                parameters.color_range.width_degrees,
                parameters.color_range.softness,
                parameters.color_range.hue_shift_degrees,
                parameters.color_range.saturation,
                parameters.color_range.lightness,
            ]);
            flattened.push(if parameters.selective_color_relative {
                1.0
            } else {
                0.0
            });
            flattened.push(parameters.selective_color_lightness_protection);
            flattened.extend(parameters.selective_color_cmyk);
            // Preserve the original v1 Color Mixer / Point Color order and
            // add global opponent controls before the variable range tail.
            flattened.extend([parameters.global_a_balance, parameters.global_b_balance]);
            for range in &parameters.additional_color_ranges {
                flattened.extend([
                    if range.enabled { 1.0 } else { 0.0 },
                    range.center_hue_degrees,
                    range.width_degrees,
                    range.softness,
                    range.hue_shift_degrees,
                    range.saturation,
                    range.lightness,
                ]);
            }
            (
                ffi::FfiAdjustmentOperation::PerceptualColor,
                flattened,
                vec![
                    u32::try_from(parameters.additional_color_ranges.len())
                        .expect("validated Point Color range count fits u32"),
                ],
                vec![],
            )
        }
        AdjustmentRenderOperation::OklabColorWarper { parameters } => {
            // The wire order is strength followed by row-major `(a, b)` pairs.
            // It is deliberately fixed-size: a Recipe and the native lattice
            // can never disagree about topology.
            let mut flattened = Vec::with_capacity(1 + OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT * 2);
            flattened.push(parameters.strength);
            for point in &parameters.control_points {
                flattened.extend([point.a_offset, point.b_offset]);
            }
            (
                ffi::FfiAdjustmentOperation::OklabColorWarper,
                flattened,
                vec![],
                vec![],
            )
        }
        AdjustmentRenderOperation::Lut3D {
            document,
            intensity,
        } => (
            ffi::FfiAdjustmentOperation::Lut3D,
            vec![*intensity],
            vec![],
            document.clone(),
        ),
        AdjustmentRenderOperation::Sharpen { parameters, .. } => {
            let mut flattened = vec![
                parameters.amount,
                parameters.radius,
                parameters.threshold,
                parameters.masking,
            ];
            flattened.extend([
                parameters.clarity,
                parameters.texture,
                parameters.local_contrast,
                parameters.local_contrast_scale,
                parameters.denoise_luminance,
                parameters.denoise_detail,
                parameters.denoise_color,
                parameters.dehaze,
                parameters.defringe_purple_amount,
                parameters.defringe_purple_hue_low,
                parameters.defringe_purple_hue_high,
                parameters.defringe_green_amount,
                parameters.defringe_green_hue_low,
                parameters.defringe_green_hue_high,
                parameters.shadows_hue,
                parameters.shadows_saturation,
                parameters.shadows_luminance,
                parameters.midtones_hue,
                parameters.midtones_saturation,
                parameters.midtones_luminance,
                parameters.highlights_hue,
                parameters.highlights_saturation,
                parameters.highlights_luminance,
                parameters.grading_blending,
                parameters.grading_balance,
                parameters.grain_amount,
                parameters.grain_size,
                parameters.grain_roughness,
                parameters.vignette_amount,
                parameters.vignette_midpoint,
                parameters.vignette_roundness,
                parameters.vignette_feather,
                parameters.vignette_highlights,
            ]);
            (
                ffi::FfiAdjustmentOperation::Sharpen,
                flattened,
                vec![],
                vec![],
            )
        }
        AdjustmentRenderOperation::SpotHeal { targets, strokes } => {
            let stroke_parameters = strokes
                .iter()
                .map(|stroke| 10 + stroke.points.len() * 2)
                .sum::<usize>();
            let mut flattened = Vec::with_capacity(targets.len() * 12 + stroke_parameters);
            let mut parameter_group_lengths = vec![
                u32::try_from(targets.len()).expect("validated spot-heal target count fits u32"),
                u32::try_from(strokes.len()).expect("validated continuous stroke count fits u32"),
            ];
            for target in targets {
                flattened.extend([
                    target.center_x,
                    target.center_y,
                    f64::from(target.radius_level_zero_pixels),
                    f64::from(target.mode),
                    target.source_offset_x_radii,
                    target.source_offset_y_radii,
                    target.source_rotation_degrees,
                    target.source_scale,
                    f64::from(target.source_flip_horizontal),
                    f64::from(target.source_flip_vertical),
                    target.feather,
                    target.strength,
                ]);
            }
            for stroke in strokes {
                parameter_group_lengths.push(
                    u32::try_from(stroke.points.len())
                        .expect("validated continuous stroke point count fits u32"),
                );
                flattened.extend([
                    f64::from(stroke.radius_level_zero_pixels),
                    f64::from(stroke.mode),
                    stroke.source_offset_x_radii,
                    stroke.source_offset_y_radii,
                    stroke.source_rotation_degrees,
                    stroke.source_scale,
                    f64::from(stroke.source_flip_horizontal),
                    f64::from(stroke.source_flip_vertical),
                    stroke.feather,
                    stroke.strength,
                ]);
                for point in &stroke.points {
                    flattened.extend([point.x, point.y]);
                }
            }
            (
                ffi::FfiAdjustmentOperation::SpotHeal,
                flattened,
                parameter_group_lengths,
                vec![],
            )
        }
        AdjustmentRenderOperation::ImageCompletion { patches } => {
            let mut parameters = Vec::with_capacity(patches.len() * 9);
            let mut group_lengths = Vec::with_capacity(patches.len() + 1);
            let payload_len = patches.iter().map(|patch| patch.rgba8.len()).sum();
            let mut payload = Vec::with_capacity(payload_len);
            group_lengths.push(
                u32::try_from(patches.len()).expect("validated completion patch count fits u32"),
            );
            for patch in patches {
                parameters.extend([
                    f64::from(patch.raster_width),
                    f64::from(patch.raster_height),
                    f64::from(patch.coordinate_width),
                    f64::from(patch.coordinate_height),
                    patch.bounds_left,
                    patch.bounds_top,
                    patch.bounds_right,
                    patch.bounds_bottom,
                    patch.strength,
                ]);
                group_lengths.push(
                    u32::try_from(patch.rgba8.len())
                        .expect("validated completion patch byte length fits u32"),
                );
                payload.extend_from_slice(&patch.rgba8);
            }
            (
                ffi::FfiAdjustmentOperation::ImageCompletion,
                parameters,
                group_lengths,
                payload,
            )
        }
    };
    let detail_effects_pass = match &node.operation {
        AdjustmentRenderOperation::Sharpen {
            pass: AdjustmentDetailEffectsPass::ColorGrading,
            ..
        } => ffi::FfiDetailEffectsPass::ColorGrading,
        AdjustmentRenderOperation::Sharpen {
            pass: AdjustmentDetailEffectsPass::FinishingEffects,
            ..
        } => ffi::FfiDetailEffectsPass::FinishingEffects,
        _ => ffi::FfiDetailEffectsPass::TechnicalDetail,
    };
    ffi::FfiAdjustmentNode {
        node_id: node.node_id.clone(),
        operation,
        detail_effects_pass,
        parameter_schema_version: node.parameter_schema_version,
        implementation_version: node.implementation_version,
        enabled: node.enabled,
        parameters,
        payload,
        parameter_group_lengths,
        mask_components,
        mask_final_invert,
    }
}

pub(super) fn proxy_payload(proxy: ffi::FfiEncodedProxy) -> shadow_domain::ProxyPayload {
    shadow_domain::ProxyPayload {
        dimensions: dimensions(&proxy.dimensions),
        codec: preview_codec(proxy.format),
        bits_per_channel: proxy.bits_per_channel,
        channels: proxy.channels,
        bytes: proxy.bytes,
    }
}

#[cfg(test)]
mod tests;
