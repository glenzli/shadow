//! Narrow command projection into the complete desktop draft. Canonical
//! Recipe validation, private-state preservation and CAS remain with their owners.

use anyhow::Result;
use serde_json::{Value, json};

use super::{
    fault,
    protocol::{CurveChannel, Edit, GeometryPatch},
};
use crate::{ffi, recipe_v1::new_basic_grade_node};

pub(super) fn apply(settings: &mut ffi::FfiEditSettings, edits: &[Edit]) -> Result<()> {
    if edits.len() > 64 {
        return Err(fault(
            "invalid_request",
            "at most 64 edits may be committed together",
        ));
    }
    for edit in edits {
        match edit {
            Edit::SetBasic { node_index, values } => {
                let basic = &mut local_node(settings, *node_index)?.basic;
                if let Some(value) = values.exposure_stops {
                    basic.exposure_stops = value;
                }
                if let Some(value) = values.contrast_factor {
                    basic.contrast_factor = value;
                }
                if let Some(value) = values.white_balance_temperature {
                    basic.white_balance_temperature = value;
                }
                if let Some(value) = values.white_balance_tint {
                    basic.white_balance_tint = value;
                }
                if let Some(value) = values.saturation_factor {
                    basic.saturation_factor = value;
                }
            }
            Edit::SetCurve {
                node_index,
                channel,
                points,
            } => {
                let fine = &mut local_node(settings, *node_index)?.fine;
                if points.is_empty() {
                    return Err(fault(
                        "invalid_edit",
                        "set_curve requires points; use reset_curve for a neutral curve",
                    ));
                }
                *curve_values(fine, *channel) = points.iter().flatten().copied().collect();
            }
            Edit::ResetCurve {
                node_index,
                channel,
            } => curve_values(&mut local_node(settings, *node_index)?.fine, *channel).clear(),
            Edit::SetNode {
                node_index,
                enabled,
                opacity,
                label,
            } => {
                let node = node(settings, *node_index)?;
                if let Some(value) = enabled {
                    node.enabled = *value;
                }
                if let Some(value) = opacity {
                    node.opacity = *value;
                }
                if let Some(value) = label {
                    node.label.clone_from(value);
                }
            }
            Edit::AddNode { label } => settings.grade_nodes.push(new_basic_grade_node(label)?),
            Edit::RemoveNode { node_index } => {
                node(settings, *node_index)?;
                settings.grade_nodes.remove(*node_index);
            }
            Edit::MoveNode { from, to } => {
                node(settings, *from)?;
                node(settings, *to)?;
                let moved = settings.grade_nodes.remove(*from);
                settings.grade_nodes.insert(*to, moved);
            }
            Edit::SetGeometry { values } => patch_geometry(&mut settings.geometry, values),
        }
    }
    // Shared canonical validation happens before any durable write. Do not
    // maintain a CLI copy of ranges, geometry rules or graph invariants.
    crate::recipe_v1::decode_grade_stack_draft_recipe_v1(settings)
        .map_err(|error| fault("invalid_edit", format!("{error:#}")))?;
    Ok(())
}

fn patch_geometry(geometry: &mut ffi::FfiPhotoGeometry, values: &GeometryPatch) {
    geometry.present = true;
    if let Some(value) = values.enabled {
        geometry.enabled = value;
    }
    if let Some(value) = values.crop_left {
        geometry.crop_left = value;
    }
    if let Some(value) = values.crop_top {
        geometry.crop_top = value;
    }
    if let Some(value) = values.crop_right {
        geometry.crop_right = value;
    }
    if let Some(value) = values.crop_bottom {
        geometry.crop_bottom = value;
    }
    if let Some(value) = values.quarter_turn {
        geometry.quarter_turn = value;
    }
    if let Some(value) = values.straighten_degrees {
        geometry.straighten_degrees = value;
    }
    if let Some(value) = values.perspective_vertical {
        geometry.perspective_vertical = value;
    }
    if let Some(value) = values.perspective_horizontal {
        geometry.perspective_horizontal = value;
    }
    if let Some(value) = values.flip_horizontal {
        geometry.flip_horizontal = value;
    }
    if let Some(value) = values.flip_vertical {
        geometry.flip_vertical = value;
    }
}

fn node(settings: &mut ffi::FfiEditSettings, index: usize) -> Result<&mut ffi::FfiGradeNode> {
    settings.grade_nodes.get_mut(index).ok_or_else(|| {
        fault(
            "invalid_edit",
            "Grade Node index is outside the captured stack",
        )
    })
}

fn local_node(settings: &mut ffi::FfiEditSettings, index: usize) -> Result<&mut ffi::FfiGradeNode> {
    let node = node(settings, index)?;
    if !node.shared_layer_id.is_empty() {
        return Err(fault(
            "unsupported",
            "detach shared Grade Nodes in the desktop before editing their controls",
        ));
    }
    Ok(node)
}

fn curve_values(fine: &mut ffi::FfiFineEditParameters, channel: CurveChannel) -> &mut Vec<f64> {
    match channel {
        CurveChannel::OklabLightness => &mut fine.oklab_lightness_curve_points,
        CurveChannel::RgbMaster => &mut fine.rgb_curve_master_points,
        CurveChannel::RgbRed => &mut fine.rgb_curve_red_points,
        CurveChannel::RgbGreen => &mut fine.rgb_curve_green_points,
        CurveChannel::RgbBlue => &mut fine.rgb_curve_blue_points,
    }
}

fn curve_points(values: &[f64]) -> Vec<[f64; 2]> {
    // These vectors come from the canonical, validated desktop DTO projection.
    values
        .chunks_exact(2)
        .map(|point| [point[0], point[1]])
        .collect()
}

fn curves_projection(fine: &ffi::FfiFineEditParameters) -> Value {
    json!({
        "oklab_lightness": curve_points(&fine.oklab_lightness_curve_points),
        "rgb_master": curve_points(&fine.rgb_curve_master_points),
        "rgb_red": curve_points(&fine.rgb_curve_red_points),
        "rgb_green": curve_points(&fine.rgb_curve_green_points),
        "rgb_blue": curve_points(&fine.rgb_curve_blue_points)
    })
}

pub(super) fn editable_projection(settings: &ffi::FfiEditSettings) -> Value {
    let geometry = &settings.geometry;
    json!({
        "grade_nodes": settings.grade_nodes.iter().enumerate().map(|(index, node)| json!({
            "node_index": index, "grade_node_id": node.grade_node_id,
            "label": node.label, "enabled": node.enabled, "opacity": node.opacity,
            "shared_layer_id": node.shared_layer_id,
            "curves": curves_projection(&node.fine),
            "basic": {
                "exposure_stops": node.basic.exposure_stops,
                "contrast_factor": node.basic.contrast_factor,
                "white_balance_temperature": node.basic.white_balance_temperature,
                "white_balance_tint": node.basic.white_balance_tint,
                "saturation_factor": node.basic.saturation_factor
            }
        })).collect::<Vec<_>>(),
        "geometry": {
            "present": geometry.present, "enabled": geometry.enabled,
            "crop_left": geometry.crop_left, "crop_top": geometry.crop_top,
            "crop_right": geometry.crop_right, "crop_bottom": geometry.crop_bottom,
            "quarter_turn": geometry.quarter_turn,
            "straighten_degrees": geometry.straighten_degrees,
            "perspective_vertical": geometry.perspective_vertical,
            "perspective_horizontal": geometry.perspective_horizontal,
            "flip_horizontal": geometry.flip_horizontal,
            "flip_vertical": geometry.flip_vertical
        },
        "other_settings": "preserved_in_complete_recipe_not_editable_in_cli_v1"
    })
}
