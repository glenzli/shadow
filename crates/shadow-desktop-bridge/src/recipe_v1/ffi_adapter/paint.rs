//! Lossless paint DTO projection, separate from image-completion provenance.
use crate::ffi;
use anyhow::{Result, bail};
use shadow_domain::{PaintBlendMode, PaintLayer, PaintPoint, PaintStroke, UnitInterval};

pub(super) fn decode(value: &ffi::FfiPaintLayer) -> Result<PaintLayer> {
    let layer = PaintLayer {
        id: value.id.parse()?,
        label: value.label.clone(),
        enabled: value.enabled,
        opacity: UnitInterval::new(value.opacity)?,
        blend: match value.blend {
            0 => PaintBlendMode::Normal,
            1 => PaintBlendMode::Color,
            2 => PaintBlendMode::SoftLight,
            _ => bail!("unsupported paint blend mode"),
        },
        coordinate_width: value.coordinate_width,
        coordinate_height: value.coordinate_height,
        strokes: value
            .strokes
            .iter()
            .map(|s| {
                Ok(PaintStroke {
                    radius: UnitInterval::new(s.radius)?,
                    hardness: UnitInterval::new(s.hardness)?,
                    opacity: UnitInterval::new(s.opacity)?,
                    flow: UnitInterval::new(s.flow)?,
                    color: [
                        UnitInterval::new(s.red)?,
                        UnitInterval::new(s.green)?,
                        UnitInterval::new(s.blue)?,
                    ],
                    erase: s.erase,
                    points: s
                        .points
                        .iter()
                        .map(|p| {
                            Ok(PaintPoint {
                                x: UnitInterval::new(p.x)?,
                                y: UnitInterval::new(p.y)?,
                                pressure: UnitInterval::new(p.pressure)?,
                            })
                        })
                        .collect::<Result<_>>()?,
                })
            })
            .collect::<Result<_>>()?,
    };
    layer.validate()?;
    Ok(layer)
}
pub(super) fn encode(value: &PaintLayer) -> ffi::FfiPaintLayer {
    ffi::FfiPaintLayer {
        id: value.id.to_string(),
        label: value.label.clone(),
        enabled: value.enabled,
        opacity: value.opacity.get(),
        blend: match value.blend {
            PaintBlendMode::Normal => 0,
            PaintBlendMode::Color => 1,
            PaintBlendMode::SoftLight => 2,
        },
        coordinate_width: value.coordinate_width,
        coordinate_height: value.coordinate_height,
        strokes: value
            .strokes
            .iter()
            .map(|s| ffi::FfiPaintStroke {
                radius: s.radius.get(),
                hardness: s.hardness.get(),
                opacity: s.opacity.get(),
                flow: s.flow.get(),
                red: s.color[0].get(),
                green: s.color[1].get(),
                blue: s.color[2].get(),
                erase: s.erase,
                points: s
                    .points
                    .iter()
                    .map(|p| ffi::FfiPaintPoint {
                        x: p.x.get(),
                        y: p.y.get(),
                        pressure: p.pressure.get(),
                    })
                    .collect(),
            })
            .collect(),
    }
}
