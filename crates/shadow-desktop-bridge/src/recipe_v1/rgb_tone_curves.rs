//! Persisted RGB-curve contract shared by snapshot and execution adapters.

use super::{
    parameter_block, required_float_vector, tone_curve_parameter_value,
    tone_curve_points_from_vector, validate_tone_curve,
};
use anyhow::{Result, bail};
use shadow_bridge::RgbToneCurves;
use shadow_domain::operation::{
    RGB_TONE_CURVES_IMPLEMENTATION_VERSION, RGB_TONE_CURVES_OPERATION_ID,
    RGB_TONE_CURVES_PARAMETER_KEYS, RGB_TONE_CURVES_PARAMETER_SCHEMA_VERSION,
};
use shadow_domain::{
    AdjustmentNode, ImageDomain, NodeId, NodeInput, OperationDescriptor, OperationId, PortType,
    ProcessingStage,
};

pub(crate) fn decode_rgb_tone_curves(node: &AdjustmentNode) -> Result<RgbToneCurves> {
    let mut curves = RgbToneCurves::default();
    for (points, key) in curves
        .channels
        .iter_mut()
        .zip(RGB_TONE_CURVES_PARAMETER_KEYS)
    {
        *points =
            tone_curve_points_from_vector(&required_float_vector(node.parameters(), key, 4)?)?;
        validate_tone_curve(points)?;
    }
    Ok(curves)
}

pub(crate) fn rgb_tone_curves_render_op(
    id: NodeId,
    input: NodeInput,
    curves: &RgbToneCurves,
) -> Result<AdjustmentNode> {
    for points in &curves.channels {
        validate_tone_curve(points)?;
    }
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(RGB_TONE_CURVES_OPERATION_ID)?,
        RGB_TONE_CURVES_PARAMETER_SCHEMA_VERSION,
        RGB_TONE_CURVES_IMPLEMENTATION_VERSION,
        ProcessingStage::ToneAndLocalContrast,
        vec![rgb],
        rgb,
        None,
    )?;
    let values = [0, 1, 2, 3].map(|i| tone_curve_parameter_value(&curves.channels[i]));
    let [a, b, c, d] = values;
    Ok(AdjustmentNode::new(
        id,
        operation,
        vec![input],
        parameter_block([
            (RGB_TONE_CURVES_PARAMETER_KEYS[0], a?),
            (RGB_TONE_CURVES_PARAMETER_KEYS[1], b?),
            (RGB_TONE_CURVES_PARAMETER_KEYS[2], c?),
            (RGB_TONE_CURVES_PARAMETER_KEYS[3], d?),
        ])?,
        None,
    )?)
}

pub(crate) fn validate_rgb_tone_curves_render_op(
    node: &AdjustmentNode,
    input: NodeInput,
) -> Result<()> {
    let op = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    if op.operation_id().as_str() != RGB_TONE_CURVES_OPERATION_ID
        || op.parameter_schema_version() != RGB_TONE_CURVES_PARAMETER_SCHEMA_VERSION
        || op.implementation_version() != RGB_TONE_CURVES_IMPLEMENTATION_VERSION
        || op.stage() != ProcessingStage::ToneAndLocalContrast
        || op.input_types() != [rgb]
        || op.output_type() != rgb
        || op.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("RGB curves use an unsupported contract");
    }
    decode_rgb_tone_curves(node)?;
    Ok(())
}
