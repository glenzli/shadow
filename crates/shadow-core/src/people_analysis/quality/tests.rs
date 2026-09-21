use super::*;
use image::{Rgb, RgbImage};

fn textured_face(edge: u32) -> DynamicImage {
    DynamicImage::ImageRgb8(RgbImage::from_fn(edge, edge, |x, y| {
        let value = if (x / 5 + y / 7) % 2 == 0 { 60 } else { 160 };
        Rgb([value, value, value])
    }))
}

fn bounds(edge: f32) -> FaceBoundingBox {
    FaceBoundingBox {
        x: 0.0,
        y: 0.0,
        width: edge,
        height: edge,
    }
}

#[test]
fn tiny_or_clipped_faces_do_not_gain_quality_by_upscaling() {
    assert!(portrait_quality(&textured_face(24), bounds(24.0)).is_none());
    assert!(
        portrait_quality(
            &textured_face(96),
            FaceBoundingBox {
                x: 80.0,
                ..bounds(96.0)
            }
        )
        .is_none()
    );
    assert!(
        portrait_quality(
            &textured_face(96),
            FaceBoundingBox {
                width: f32::NAN,
                ..bounds(96.0)
            }
        )
        .is_none()
    );
}

#[test]
fn clear_detail_passes_but_defocus_and_uniform_noise_free_patches_do_not() {
    let sharp = textured_face(96);
    assert!(portrait_quality(&sharp, bounds(96.0)).is_some());
    assert!(portrait_quality(&sharp.blur(5.0), bounds(96.0)).is_none());
    let flat = DynamicImage::ImageRgb8(RgbImage::from_pixel(96, 96, Rgb([100; 3])));
    assert!(portrait_quality(&flat, bounds(96.0)).is_none());
}

#[test]
fn sharp_background_border_does_not_rescue_a_blurred_face() {
    let mut image = textured_face(96).to_rgb8();
    for y in 10..86 {
        for x in 10..86 {
            image[(x, y)] = Rgb([110; 3]);
        }
    }
    assert!(portrait_quality(&DynamicImage::ImageRgb8(image), bounds(96.0)).is_none());
}

#[test]
fn shadow_detail_is_not_rejected_merely_for_low_brightness() {
    let source = textured_face(96);
    assert!(portrait_quality(&source.brighten(-40), bounds(96.0)).is_some());
}
