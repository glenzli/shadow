use super::*;

#[test]
fn large_jpeg_is_bounded_with_aspect_and_small_inputs_are_reused() {
    let source = image::RgbImage::from_pixel(2400, 1200, image::Rgb([80, 120, 180]));
    let mut bytes = Vec::new();
    JpegEncoder::new_with_quality(&mut bytes, 90)
        .encode_image(&source)
        .expect("fixture");
    let input = bounded_jpeg(
        &bytes,
        ImageDimensions {
            width: 2400,
            height: 1200,
        },
        1024,
    )
    .expect("bounded");
    assert_eq!(
        input.dimensions,
        ImageDimensions {
            width: 1024,
            height: 512
        }
    );
    let decoded = image::load_from_memory(&input.bytes).expect("valid jpeg");
    assert_eq!((decoded.width(), decoded.height()), (1024, 512));
    let reused = bounded_jpeg(&input.bytes, input.dimensions, 1024).expect("reuse");
    assert!(matches!(reused.bytes, Cow::Borrowed(_)));
    assert!(
        bounded_jpeg(
            &bytes,
            ImageDimensions {
                width: 2500,
                height: 1200
            },
            1024
        )
        .is_err()
    );
}
