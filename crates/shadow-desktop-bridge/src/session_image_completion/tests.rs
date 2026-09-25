use super::*;

#[test]
fn screen_circle_keeps_pixel_radius_after_crop_rotation_and_near_edges() {
    use shadow_domain::{PhotoGeometry, PhotoQuarterTurn};
    for quarter_turn in [
        PhotoQuarterTurn::Zero,
        PhotoQuarterTurn::Clockwise90,
        PhotoQuarterTurn::Clockwise180,
        PhotoQuarterTurn::Clockwise270,
    ] {
        let geometry = PhotoGeometry::new(
            UnitInterval::new(0.1).unwrap(),
            UnitInterval::new(0.1).unwrap(),
            UnitInterval::new(0.9).unwrap(),
            UnitInterval::new(0.9).unwrap(),
            quarter_turn,
            true,
            false,
        )
        .unwrap();
        for coordinate in [0.5, 0.99] {
            let points = original_brush_points(
                &[ffi::FfiImageCompletionBrushPoint {
                    x: coordinate,
                    y: coordinate,
                    radius: 0.04,
                    erase: false,
                    stroke_id: 1,
                }],
                geometry,
                RasterExtent::new(1200, 800).unwrap(),
            )
            .unwrap();
            let point = points[0];
            // 80% crop of the 800px short edge, times 4% brush radius.
            assert!((point.radius_x * 1200.0 - 25.6).abs() < 1e-8);
            assert!((point.radius_y * 800.0 - 25.6).abs() < 1e-8);
        }
    }
}

#[test]
fn placement_adds_context_and_remains_bounded() {
    let points = vec![OriginalBrushPoint {
        x: 0.5,
        y: 0.5,
        radius_x: 0.02,
        radius_y: 0.02,
        erase: false,
        stroke_id: 1,
    }];
    let placement = completion_placement(&points, RasterExtent::new(1200, 800).unwrap()).unwrap();
    assert!(placement.bounds_left.get() < 0.48);
    assert!(placement.bounds_right.get() > 0.52);
    let pixel_width = (placement.bounds_right.get() - placement.bounds_left.get()) * 1200.0;
    let pixel_height = (placement.bounds_bottom.get() - placement.bounds_top.get()) * 800.0;
    assert!((pixel_width - pixel_height).abs() < 1.0e-9);
    assert!(pixel_width >= f64::from(COMPLETION_MIN_CROP_EDGE));
}

#[test]
fn expansion_uses_original_context_while_growing_the_model_mask() {
    let extent = RasterExtent::new(1200, 800).unwrap();
    let image = DynamicImage::ImageRgb8(image::RgbImage::from_pixel(
        extent.width,
        extent.height,
        image::Rgb([80, 100, 120]),
    ));
    let mut jpeg = Cursor::new(Vec::new());
    image.write_to(&mut jpeg, ImageFormat::Jpeg).unwrap();
    let base = [OriginalBrushPoint {
        x: 0.5,
        y: 0.5,
        radius_x: 0.05,
        radius_y: 0.075,
        erase: false,
        stroke_id: 1,
    }];
    let expanded = [OriginalBrushPoint {
        radius_x: 0.075,
        radius_y: 0.1125,
        ..base[0]
    }];
    let jpeg = jpeg.into_inner();
    let original = prepare_completion_input(&jpeg, extent, &base).unwrap();
    let enlarged =
        prepare_completion_input_with_crop_points(&jpeg, extent, &expanded, &base).unwrap();
    assert_eq!(original.placement, enlarged.placement);
    assert_eq!(original.crop_png, enlarged.crop_png);
    let selected_pixels = |mask: &[u8]| mask.iter().filter(|pixel| **pixel != 0).count();
    assert!(selected_pixels(&enlarged.mask_gray8) > selected_pixels(&original.mask_gray8));
}

#[test]
fn expansion_outside_original_crop_recomputes_placement_without_clipping() {
    let extent = RasterExtent::new(1200, 800).unwrap();
    let base = OriginalBrushPoint {
        x: 0.5,
        y: 0.5,
        radius_x: 0.01,
        radius_y: 0.01,
        erase: false,
        stroke_id: 1,
    };
    let expanded = OriginalBrushPoint {
        radius_x: 0.2,
        radius_y: 0.2,
        ..base
    };
    let original_placement = completion_placement(&[base], extent).unwrap();
    assert!(!selection_fits_placement(&[expanded], original_placement));
    let mut jpeg = Cursor::new(Vec::new());
    DynamicImage::ImageRgb8(image::RgbImage::from_pixel(
        extent.width,
        extent.height,
        image::Rgb([80, 100, 120]),
    ))
    .write_to(&mut jpeg, ImageFormat::Jpeg)
    .unwrap();
    let prepared =
        prepare_completion_input_with_crop_points(&jpeg.into_inner(), extent, &[expanded], &[base])
            .unwrap();
    assert_ne!(prepared.placement, original_placement);
    assert!(selection_fits_placement(&[expanded], prepared.placement));
    assert!(prepared.mask_gray8.contains(&255));
}

#[test]
fn selection_expansion_preserves_erase_and_caps_painted_radius() {
    let points = [
        ffi::FfiImageCompletionBrushPoint {
            x: 0.5,
            y: 0.5,
            radius: 0.22,
            erase: false,
            stroke_id: 1,
        },
        ffi::FfiImageCompletionBrushPoint {
            x: 0.5,
            y: 0.5,
            radius: 0.03,
            erase: true,
            stroke_id: 2,
        },
    ];
    let expanded = expanded_brush_points(&points, 0.06);
    assert_eq!(expanded[0].radius, MAX_BRUSH_RADIUS);
    assert_eq!(expanded[1].radius, points[1].radius);
}

#[test]
fn narrow_subject_near_frame_edge_keeps_square_context_and_registered_mask() {
    let extent = RasterExtent::new(1200, 800).unwrap();
    let point = OriginalBrushPoint {
        x: 0.02,
        y: 0.45,
        radius_x: 0.012,
        radius_y: 0.018,
        erase: false,
        stroke_id: 1,
    };
    let placement = completion_placement(&[point], extent).unwrap();
    assert_eq!(placement.bounds_left.get(), 0.0);
    let crop_width = (placement.bounds_right.get() - placement.bounds_left.get()) * 1200.0;
    let crop_height = (placement.bounds_bottom.get() - placement.bounds_top.get()) * 800.0;
    assert!((crop_width - crop_height).abs() < 1.0e-9);
    assert!(crop_width >= 256.0);
    let mask = rasterize_mask(&[point], placement);
    let model_x = ((point.x - placement.bounds_left.get())
        / (placement.bounds_right.get() - placement.bounds_left.get())
        * f64::from(COMPLETION_MODEL_EDGE)) as usize;
    let model_y = ((point.y - placement.bounds_top.get())
        / (placement.bounds_bottom.get() - placement.bounds_top.get())
        * f64::from(COMPLETION_MODEL_EDGE)) as usize;
    assert_eq!(
        mask[model_y * COMPLETION_MODEL_EDGE as usize + model_x],
        255
    );
    assert_eq!(mask[256 * COMPLETION_MODEL_EDGE as usize + 400], 0);
}

#[test]
fn model_input_keeps_source_colours_and_selection_registered_after_square_crop() {
    let extent = RasterExtent::new(1200, 800).unwrap();
    let image = image::RgbImage::from_fn(extent.width, extent.height, |x, y| {
        image::Rgb([((x / 5) % 256) as u8, ((y / 4) % 256) as u8, 96])
    });
    let mut jpeg = Cursor::new(Vec::new());
    DynamicImage::ImageRgb8(image)
        .write_to(&mut jpeg, ImageFormat::Jpeg)
        .unwrap();
    let point = OriginalBrushPoint {
        x: 0.35,
        y: 0.7,
        radius_x: 0.012,
        radius_y: 0.018,
        erase: false,
        stroke_id: 1,
    };
    let prepared = prepare_completion_input(&jpeg.into_inner(), extent, &[point]).unwrap();
    let crop = image::load_from_memory_with_format(&prepared.crop_png, ImageFormat::Png)
        .unwrap()
        .to_rgb8();
    assert_eq!(crop.dimensions(), (512, 512));
    let mask = GrayImage::from_raw(512, 512, prepared.mask_gray8).unwrap();
    let model_x = ((point.x - prepared.placement.bounds_left.get())
        / (prepared.placement.bounds_right.get() - prepared.placement.bounds_left.get())
        * 512.0) as u32;
    let model_y = ((point.y - prepared.placement.bounds_top.get())
        / (prepared.placement.bounds_bottom.get() - prepared.placement.bounds_top.get())
        * 512.0) as u32;
    assert_eq!(mask.get_pixel(model_x, model_y)[0], 255);
    let sampled = crop.get_pixel(model_x, model_y);
    assert!((i16::from(sampled[0]) - i16::from(((point.x * 1200.0) / 5.0) as u8)).abs() <= 3);
    assert!((i16::from(sampled[1]) - i16::from(((point.y * 800.0) / 4.0) as u8)).abs() <= 3);
}

#[test]
fn broad_selection_stays_inside_source_when_square_crop_would_exceed_short_edge() {
    let extent = RasterExtent::new(1200, 800).unwrap();
    let point = OriginalBrushPoint {
        x: 0.5,
        y: 0.5,
        radius_x: 0.45,
        radius_y: 0.04,
        erase: false,
        stroke_id: 1,
    };
    let placement = completion_placement(&[point], extent).unwrap();
    assert!(placement.bounds_left.get() <= point.x - point.radius_x);
    assert!(placement.bounds_right.get() >= point.x + point.radius_x);
    assert_eq!(placement.bounds_top.get(), 0.0);
    assert_eq!(placement.bounds_bottom.get(), 1.0);
}

#[test]
fn ordered_erase_samples_remove_painted_pixels() {
    let placement = ImageCompletionPlacement {
        bounds_left: UnitInterval::ZERO,
        bounds_top: UnitInterval::ZERO,
        bounds_right: UnitInterval::ONE,
        bounds_bottom: UnitInterval::ONE,
    };
    let points = vec![
        OriginalBrushPoint {
            x: 0.5,
            y: 0.5,
            radius_x: 0.1,
            radius_y: 0.1,
            erase: false,
            stroke_id: 1,
        },
        OriginalBrushPoint {
            x: 0.5,
            y: 0.5,
            radius_x: 0.05,
            radius_y: 0.05,
            erase: true,
            stroke_id: 2,
        },
    ];
    let mask = rasterize_mask(&points, placement);
    assert_eq!(mask[(256 * 512 + 256) as usize], 0);
    assert!(mask.contains(&255));
}
