use image::{DynamicImage, GrayImage, ImageFormat, Luma, Rgb, RgbImage};
use std::io::Cursor;

use super::*;

fn png(image: &DynamicImage) -> Vec<u8> {
    let mut encoded = Cursor::new(Vec::new());
    image.write_to(&mut encoded, ImageFormat::Png).unwrap();
    encoded.into_inner()
}

#[test]
fn rejects_mismatched_or_non_gray_inputs_before_runtime_discovery() {
    let rgb = png(&DynamicImage::ImageRgb8(RgbImage::from_pixel(
        512,
        512,
        Rgb([1, 2, 3]),
    )));
    let small_mask = png(&DynamicImage::ImageLuma8(GrayImage::from_pixel(
        256,
        256,
        Luma([255]),
    )));
    let error = png_extent(&small_mask, true).unwrap();
    assert_eq!(error.width, 256);
    assert_ne!(png_extent(&rgb, false).unwrap(), error);
}

#[test]
fn gray8_mask_admission_is_exact() {
    let mask = png(&DynamicImage::ImageLuma8(GrayImage::from_pixel(
        512,
        512,
        Luma([255]),
    )));
    assert_eq!(
        png_extent(&mask, true).unwrap(),
        RasterExtent::new(512, 512).unwrap()
    );
}
