//! One-way admission of RGB model output into bounded scene/working-linear patches.
//! This is not sensor RAW recovery. Clipped colours have no exact inverse.

pub(super) const IDENTITY: &str = "linear-rgba32fle-neutral-v2-context-ceiling64";

fn srgb_decode(value: u8) -> f64 {
    let e = f64::from(value) / 255.0;
    if e <= 0.04045 {
        e / 12.92
    } else {
        ((e + 0.055) / 1.055).powf(2.4)
    }
}
fn scene_luminance(rgb: [u8; 3]) -> (f64, f64) {
    let d = rgb.map(srgb_decode);
    let y = d[0] * 0.2126 + d[1] * 0.7152 + d[2] * 0.0722;
    let e = if y <= 0.003_130_8 {
        y * 12.92
    } else {
        1.055 * y.powf(1.0 / 2.4) - 0.055
    };
    let t = if e < 0.081 {
        e / 4.5
    } else {
        ((e + 0.099) / 1.099).powf(1.0 / 0.45)
    };
    let scene = if t <= 0.75 {
        t
    } else {
        0.75 + 0.25 * (t - 0.75) / (1.0 - t).max(1.0e-9)
    };
    (y, scene)
}

pub(super) fn decode_rgb(rgb: [u8; 3], scene_referred: bool, ceiling: f64) -> [f32; 3] {
    let display = rgb.map(srgb_decode);
    let gain = if scene_referred {
        let (y, scene) = scene_luminance(rgb);
        if y > 0.0 { scene.min(ceiling) / y } else { 1.0 }
    } else {
        1.0
    };
    display.map(|channel| (channel * gain) as f32)
}

pub(super) fn encode_linear_patch(
    rgb: &[u8],
    alpha: &[u8],
    reference: &[u8],
    mask: &[u8],
    scene_referred: bool,
) -> Vec<u8> {
    // Only unchanged context may constrain unknown highlights. Never transfer the
    // removed object's luminance field into the generated background. Saturated
    // white is ill-conditioned; the explicit finite ceiling is a reconstruction
    // policy, not evidence of recovered radiance.
    let ceiling = reference
        .chunks_exact(3)
        .zip(mask)
        .filter(|(_, m)| **m == 0)
        .map(|(p, _)| scene_luminance([p[0], p[1], p[2]]).1.min(64.0))
        .fold(1.0_f64, f64::max)
        .mul_add(1.25, 0.0)
        .min(64.0);
    let mut bytes = Vec::with_capacity(alpha.len() * 16);
    for (p, a) in rgb.chunks_exact(3).zip(alpha) {
        let decoded = if *a == 0 {
            [0.0; 3]
        } else {
            decode_rgb([p[0], p[1], p[2]], scene_referred, ceiling)
        };
        for channel in decoded {
            bytes.extend_from_slice(&channel.to_le_bytes());
        }
        bytes.extend_from_slice(&(f32::from(*a) / 255.0).to_le_bytes());
    }
    bytes
}

#[cfg(test)]
mod tests;
