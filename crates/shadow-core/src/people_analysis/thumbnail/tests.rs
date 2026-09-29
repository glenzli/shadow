use image::{DynamicImage, Rgb, RgbImage};

use super::*;

#[test]
fn crop_is_square_bounded_and_decodable() {
    let source = DynamicImage::ImageRgb8(RgbImage::from_pixel(320, 180, Rgb([80, 120, 160])));
    let encoded = face_thumbnail_jpeg(
        &source,
        FaceBoundingBox {
            x: 120.0,
            y: 40.0,
            width: 60.0,
            height: 80.0,
        },
    )
    .expect("bounded thumbnail");
    let decoded = image::load_from_memory(&encoded).expect("decode thumbnail");
    assert_eq!(decoded.width(), PEOPLE_THUMBNAIL_EDGE);
    assert_eq!(decoded.height(), PEOPLE_THUMBNAIL_EDGE);
    assert!(encoded.len() <= MAX_PEOPLE_THUMBNAIL_BYTES);
}

#[test]
fn source_pixel_budget_rejects_oversized_decode() {
    assert!(source_dimensions_admitted(4_096, 4_096));
    assert!(!source_dimensions_admitted(4_097, 4_096));
}
