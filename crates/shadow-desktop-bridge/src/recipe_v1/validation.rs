//! Validation policy for editable Recipe v1 drafts and retained identities.

use std::{
    collections::{HashMap, HashSet},
    path::Path,
};

use anyhow::{Context, Result as AnyResult, bail};
use shadow_bridge::{
    BasicEditParameters, MAX_POINT_COLOR_RANGES, MAX_TONE_CURVE_POINTS,
    OKLAB_COLOR_WARPER_MAXIMUM_OFFSET, ToneCurvePoint,
};
use shadow_domain::{
    MAX_RETOUCH_SPOTS_PER_RECIPE, MAX_RETOUCH_STROKES_PER_RECIPE, RecipeSnapshot, RetouchSpot,
    RetouchStroke,
};

use super::{
    FineEditParameters, GradeStackDraft, LutEditParameters, MAX_GRADE_NODES,
    decode_grade_stack_draft_from_recipe_v1_snapshot,
};

pub(crate) fn validate_grade_stack_draft_recipe_v1(grade_stack: &GradeStackDraft) -> AnyResult<()> {
    if !(1..=MAX_GRADE_NODES).contains(&grade_stack.grade_nodes.len()) {
        bail!("Grade Stack must contain 1 through 16 Grade Nodes");
    }
    if grade_stack.retouch_spots.len() > MAX_RETOUCH_SPOTS_PER_RECIPE {
        bail!(
            "Grade Stack contains {} repair spots, but at most {} are supported",
            grade_stack.retouch_spots.len(),
            MAX_RETOUCH_SPOTS_PER_RECIPE
        );
    }
    for spot in &grade_stack.retouch_spots {
        let _validated = RetouchSpot::new(
            spot.center_x(),
            spot.center_y(),
            spot.radius_level_zero_pixels(),
        )?
        .with_behavior(
            spot.mode(),
            spot.source_offset_x_radii(),
            spot.source_offset_y_radii(),
            spot.feather(),
        )?
        .with_source_transform(
            spot.source_rotation_degrees(),
            spot.source_scale(),
            spot.source_flip_horizontal(),
            spot.source_flip_vertical(),
        )?
        .with_strength(spot.strength());
    }
    if grade_stack.retouch_strokes.len() > MAX_RETOUCH_STROKES_PER_RECIPE {
        bail!(
            "Grade Stack contains {} repair strokes, but at most {} are supported",
            grade_stack.retouch_strokes.len(),
            MAX_RETOUCH_STROKES_PER_RECIPE
        );
    }
    for stroke in &grade_stack.retouch_strokes {
        let _validated =
            RetouchStroke::new(stroke.points().to_vec(), stroke.radius_level_zero_pixels())?
                .with_behavior(
                    stroke.mode(),
                    stroke.source_offset_x_radii(),
                    stroke.source_offset_y_radii(),
                    stroke.feather(),
                )?
                .with_source_transform(
                    stroke.source_rotation_degrees(),
                    stroke.source_scale(),
                    stroke.source_flip_horizontal(),
                    stroke.source_flip_vertical(),
                )?
                .with_strength(stroke.strength());
    }
    let mut grade_node_ids = HashSet::with_capacity(grade_stack.grade_nodes.len());
    let mut render_op_ids = HashSet::with_capacity(grade_stack.grade_nodes.len() * 11);
    for (index, grade_node) in grade_stack.grade_nodes.iter().enumerate() {
        if grade_node.local_mask.is_some() && grade_node.preserved_managed_raster.is_some() {
            bail!(
                "Grade Node {index} cannot carry both an editable local mask and an opaque managed raster marker"
            );
        }
        let identity = &grade_node.recipe_v1_identity;
        if !grade_node_ids.insert(identity.grade_node_id) {
            bail!(
                "Grade Stack contains duplicate Grade Node id {}",
                identity.grade_node_id
            );
        }
        for (role, render_op_id) in identity.recipe_v1_render_op_ids() {
            if !render_op_ids.insert(render_op_id) {
                bail!(
                    "Grade Stack contains duplicate Recipe v1 render-op id {render_op_id} at Grade Node {index} role {role}"
                );
            }
        }
        validate_basic_parameters(grade_node.basic)?;
        validate_fine_parameters(&grade_node.fine)?;
    }
    Ok(())
}

pub(crate) fn validate_grade_stack_draft_against_recipe_v1_template(
    grade_stack: &GradeStackDraft,
    template: &RecipeSnapshot,
) -> AnyResult<()> {
    let template_grade_stack = decode_grade_stack_draft_from_recipe_v1_snapshot(template)
        .context("validate base Grade Stack Recipe v1")?;
    let template_grade_nodes = template_grade_stack
        .grade_nodes
        .iter()
        .map(|grade_node| (grade_node.recipe_v1_identity.grade_node_id, grade_node))
        .collect::<HashMap<_, _>>();
    let template_render_ops = template_grade_stack
        .grade_nodes
        .iter()
        .flat_map(|grade_node| {
            let grade_node_id = grade_node.recipe_v1_identity.grade_node_id;
            grade_node
                .recipe_v1_identity
                .recipe_v1_render_op_ids()
                .map(move |(role, render_op_id)| (render_op_id, (grade_node_id, role)))
        })
        .collect::<HashMap<_, _>>();

    for grade_node in &grade_stack.grade_nodes {
        let identity = &grade_node.recipe_v1_identity;
        if let Some(template_grade_node) = template_grade_nodes.get(&identity.grade_node_id)
            && identity != &template_grade_node.recipe_v1_identity
        {
            bail!(
                "retained Grade Node {} must preserve every stable Recipe v1 render-op identity from its base Recipe",
                identity.grade_node_id
            );
        }
        for (role, render_op_id) in identity.recipe_v1_render_op_ids() {
            if let Some((template_grade_node_id, template_role)) =
                template_render_ops.get(&render_op_id)
                && (*template_grade_node_id != identity.grade_node_id || *template_role != role)
            {
                bail!(
                    "Grade Node {} role {role} reuses base Recipe v1 render-op id {render_op_id} owned by Grade Node {template_grade_node_id} role {template_role}",
                    identity.grade_node_id
                );
            }
        }
    }
    Ok(())
}

#[allow(clippy::float_cmp)] // The persisted contract requires exact normalized x endpoints.
pub(crate) fn validate_tone_curve(points: &[ToneCurvePoint]) -> AnyResult<()> {
    if !(2..=MAX_TONE_CURVE_POINTS).contains(&points.len()) {
        bail!("Tone Curve must contain 2 through 256 points");
    }
    if points
        .iter()
        .any(|point| !point.x.is_finite() || !point.y.is_finite())
    {
        bail!("Tone Curve points must contain only finite values");
    }
    if points.first().is_none_or(|point| point.x != 0.0)
        || points.last().is_none_or(|point| point.x != 1.0)
    {
        bail!("Tone Curve x coordinates must start at zero and end at one");
    }
    for pair in points.windows(2) {
        let [left, right] = pair else {
            unreachable!("windows(2) always returns two points")
        };
        if right.x <= left.x {
            bail!("Tone Curve x coordinates must be strictly increasing");
        }
        if !((right.y - left.y) / (right.x - left.x)).is_finite() {
            bail!("Tone Curve segment slopes must be finite");
        }
    }
    Ok(())
}

pub(crate) fn validate_basic_parameters(parameters: BasicEditParameters) -> AnyResult<()> {
    validate_range(parameters.exposure_stops, -16.0, 16.0, "exposure stops")?;
    validate_range(parameters.contrast_factor, 0.0, 8.0, "contrast factor")?;
    validate_range(
        parameters.white_balance_temperature,
        -1.0,
        1.0,
        "RGB white balance temperature",
    )?;
    validate_range(
        parameters.white_balance_tint,
        -1.0,
        1.0,
        "RGB white balance tint",
    )?;
    validate_range(parameters.saturation_factor, 0.0, 8.0, "saturation factor")
}

pub(crate) fn validate_lut_parameters(parameters: &LutEditParameters) -> AnyResult<()> {
    validate_range(parameters.intensity, 0.0, 1.0, "LUT intensity")?;
    if parameters.resource_id.is_empty() {
        if !parameters.title.is_empty() || !parameters.managed_path.is_empty() {
            bail!("an unselected LUT must not retain title or managed path");
        }
        return Ok(());
    }
    if parameters.resource_id.len() != 64
        || !parameters
            .resource_id
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        bail!("LUT resource id must be a lowercase SHA-256 digest");
    }
    if parameters.title.trim().is_empty() || parameters.title.len() > 512 {
        bail!("selected LUT title must contain 1 through 512 bytes");
    }
    let managed_path = Path::new(&parameters.managed_path);
    if !managed_path.is_absolute()
        || managed_path.extension().and_then(|value| value.to_str()) != Some("cube")
        || managed_path.file_stem().and_then(|value| value.to_str())
            != Some(parameters.resource_id.as_str())
    {
        bail!("selected LUT must reference its content-addressed managed .cube path");
    }
    Ok(())
}

pub(crate) fn validate_fine_parameters(parameters: &FineEditParameters) -> AnyResult<()> {
    let tone = parameters.selective_tone;
    for (name, value) in [
        ("highlights", tone.highlights),
        ("shadows", tone.shadows),
        ("whites", tone.whites),
        ("blacks", tone.blacks),
    ] {
        validate_range(value, -1.0, 1.0, name)?;
    }
    for (name, value) in [
        ("highlight red suppression", tone.highlight_red_suppression),
        (
            "highlight green suppression",
            tone.highlight_green_suppression,
        ),
        (
            "highlight blue suppression",
            tone.highlight_blue_suppression,
        ),
    ] {
        validate_range(value, 0.0, 1.0, name)?;
    }
    validate_perceptual_color_parameters(parameters)?;
    if let Some(curve) = &parameters.oklab_lightness_curve {
        validate_tone_curve(&curve.lightness).context("validate Oklab lightness curve")?;
    }
    validate_lut_parameters(&parameters.lut)?;
    validate_detail_and_finishing_parameters(parameters)
}

fn validate_perceptual_color_parameters(parameters: &FineEditParameters) -> AnyResult<()> {
    let color = &parameters.perceptual_color;
    validate_range(color.global_a_balance, -1.0, 1.0, "global Oklab a balance")?;
    validate_range(color.global_b_balance, -1.0, 1.0, "global Oklab b balance")?;
    validate_range(color.vibrance, -1.0, 1.0, "vibrance")?;
    for (name, values) in [
        ("Color Mixer hue", color.hue_shifts),
        ("Color Mixer saturation", color.saturation),
        ("Color Mixer lightness", color.lightness),
    ] {
        for value in values {
            validate_range(value, -1.0, 1.0, name)?;
        }
    }
    if 1 + color.additional_color_ranges.len() > MAX_POINT_COLOR_RANGES {
        bail!("Point Color supports at most {MAX_POINT_COLOR_RANGES} ordered ranges");
    }
    for range in std::iter::once(&color.color_range).chain(color.additional_color_ranges.iter()) {
        validate_range(range.center_hue_degrees, 0.0, 360.0, "color range center")?;
        validate_range(range.width_degrees, 1.0, 180.0, "color range width")?;
        validate_range(range.softness, 0.0, 1.0, "color range softness")?;
        validate_range(range.hue_shift_degrees, -180.0, 180.0, "color range hue")?;
        validate_range(range.saturation, -1.0, 1.0, "color range saturation")?;
        validate_range(range.lightness, -1.0, 1.0, "color range lightness")?;
    }
    validate_range(
        color.selective_color_lightness_protection,
        0.0,
        1.0,
        "Selective Color lightness protection",
    )?;
    for value in color.selective_color_cmyk {
        validate_range(value, -1.0, 1.0, "Selective Color CMYK")?;
    }
    let color_warper = &parameters.oklab_color_warper;
    validate_range(
        color_warper.strength,
        0.0,
        1.0,
        "Oklab Color Warper strength",
    )?;
    for point in color_warper.control_points {
        validate_range(
            point.a_offset,
            -OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            "Oklab Color Warper a offset",
        )?;
        validate_range(
            point.b_offset,
            -OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
            "Oklab Color Warper b offset",
        )?;
    }
    Ok(())
}

fn validate_detail_and_finishing_parameters(parameters: &FineEditParameters) -> AnyResult<()> {
    let sharpen = parameters.sharpen;
    validate_range(sharpen.amount, 0.0, 2.0, "sharpen amount")?;
    validate_range(sharpen.radius, 0.1, 5.0, "sharpen radius")?;
    validate_range(sharpen.threshold, 0.0, 1.0, "sharpen threshold")?;
    validate_range(sharpen.masking, 0.0, 1.0, "sharpen masking")?;
    validate_range(sharpen.local_contrast, -1.0, 1.0, "local contrast")?;
    validate_range(
        sharpen.local_contrast_scale,
        0.0,
        1.0,
        "local contrast scale",
    )?;
    for (name, value) in [
        ("luminance noise reduction", sharpen.denoise_luminance),
        ("noise reduction detail", sharpen.denoise_detail),
        ("color noise reduction", sharpen.denoise_color),
        ("purple defringe amount", sharpen.defringe_purple_amount),
        ("green defringe amount", sharpen.defringe_green_amount),
        ("shadow grading saturation", sharpen.shadows_saturation),
        ("midtone grading saturation", sharpen.midtones_saturation),
        (
            "highlight grading saturation",
            sharpen.highlights_saturation,
        ),
        ("grading blending", sharpen.grading_blending),
        ("grain amount", sharpen.grain_amount),
        ("grain size", sharpen.grain_size),
        ("grain roughness", sharpen.grain_roughness),
        ("vignette midpoint", sharpen.vignette_midpoint),
        ("vignette feather", sharpen.vignette_feather),
        ("vignette highlights", sharpen.vignette_highlights),
    ] {
        validate_range(value, 0.0, 1.0, name)?;
    }
    for (name, value) in [
        ("dehaze", sharpen.dehaze),
        ("shadow grading luminance", sharpen.shadows_luminance),
        ("midtone grading luminance", sharpen.midtones_luminance),
        ("highlight grading luminance", sharpen.highlights_luminance),
        ("grading balance", sharpen.grading_balance),
        ("vignette amount", sharpen.vignette_amount),
        ("vignette roundness", sharpen.vignette_roundness),
    ] {
        validate_range(value, -1.0, 1.0, name)?;
    }
    for (name, value) in [
        ("shadow grading hue", sharpen.shadows_hue),
        ("midtone grading hue", sharpen.midtones_hue),
        ("highlight grading hue", sharpen.highlights_hue),
    ] {
        validate_range(value, 0.0, 360.0, name)?;
    }
    for (name, value) in [
        ("purple defringe hue low", sharpen.defringe_purple_hue_low),
        ("purple defringe hue high", sharpen.defringe_purple_hue_high),
        ("green defringe hue low", sharpen.defringe_green_hue_low),
        ("green defringe hue high", sharpen.defringe_green_hue_high),
    ] {
        validate_range(value, 0.0, 360.0, name)?;
    }
    if sharpen.defringe_purple_hue_low + 10.0 > sharpen.defringe_purple_hue_high
        || sharpen.defringe_green_hue_low + 10.0 > sharpen.defringe_green_hue_high
    {
        bail!("defringe hue ranges must have at least a 10 degree span");
    }
    Ok(())
}

pub(crate) fn validate_range(value: f64, minimum: f64, maximum: f64, name: &str) -> AnyResult<()> {
    if value.is_finite() && (minimum..=maximum).contains(&value) {
        Ok(())
    } else {
        bail!("{name} must be finite and in {minimum}..={maximum}")
    }
}
