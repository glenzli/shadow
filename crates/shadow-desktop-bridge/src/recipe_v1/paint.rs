//! Photo-local paint executes after repair/completion, before Liquify and Canvas.
use anyhow::{Result, bail};
use shadow_bridge::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentPaintLayer,
    AdjustmentPaintPoint, AdjustmentPaintStroke, AdjustmentRenderNode, AdjustmentRenderOperation,
};
use shadow_domain::{PaintBlendMode, RecipeSnapshot};

pub(super) fn append_paint_nodes(
    snapshot: &RecipeSnapshot,
    boundaries: bool,
    nodes: &mut Vec<AdjustmentRenderNode>,
) -> Result<()> {
    if snapshot.paint_layers().is_empty() {
        return Ok(());
    }
    let node = |id: String, enabled, operation| AdjustmentRenderNode {
        node_id: id,
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled,
        operation,
    };
    // Keep all fixed photo operations in one unmasked native layer when possible.
    let saved_end = if boundaries {
        if nodes.last().is_some_and(|n| {
            n.node_id == "recipe-v1-photo-retouch:end"
                || n.node_id == "recipe-v1-photo-ai-completion:end"
        }) {
            nodes.pop()
        } else {
            nodes.push(node(
                "recipe-v1-photo-paint:start".into(),
                true,
                AdjustmentRenderOperation::LocalMaskLayerStart {
                    opacity: 1.0,
                    mask: None,
                },
            ));
            Some(node(
                "recipe-v1-photo-paint:end".into(),
                true,
                AdjustmentRenderOperation::LocalMaskLayerEnd,
            ))
        }
    } else {
        None
    };
    for layer in snapshot.paint_layers() {
        layer.validate()?;
        let id = format!("recipe-v1-photo-paint:{}", layer.id);
        if nodes.iter().any(|n| n.node_id == id) {
            bail!("duplicate paint render identity")
        }
        nodes.push(node(
            id,
            layer.enabled,
            AdjustmentRenderOperation::PaintLayer {
                layer: Box::new(AdjustmentPaintLayer {
                    coordinate_width: layer.coordinate_width,
                    coordinate_height: layer.coordinate_height,
                    opacity: layer.opacity.get(),
                    blend: match layer.blend {
                        PaintBlendMode::Normal => 0,
                        PaintBlendMode::Color => 1,
                        PaintBlendMode::SoftLight => 2,
                    },
                    strokes: layer
                        .strokes
                        .iter()
                        .map(|s| AdjustmentPaintStroke {
                            radius: s.radius.get(),
                            hardness: s.hardness.get(),
                            opacity: s.opacity.get(),
                            flow: s.flow.get(),
                            color: s.color.map(shadow_domain::UnitInterval::get),
                            erase: s.erase,
                            roundness: s.roundness,
                            angle_degrees: s.angle_degrees,
                            spacing: s.spacing,
                            texture: s.texture,
                            texture_strength: s.texture_strength,
                            pressure_size: s.pressure_size,
                            pressure_flow: s.pressure_flow,

                            points: s
                                .points
                                .iter()
                                .map(|p| AdjustmentPaintPoint {
                                    x: p.x.get(),
                                    y: p.y.get(),
                                    pressure: p.pressure.get(),
                                })
                                .collect(),
                        })
                        .collect(),
                }),
            },
        ));
    }
    if let Some(end) = saved_end {
        nodes.push(end);
    }
    Ok(())
}
#[cfg(test)]
mod tests;
