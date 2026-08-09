use shadow_ai::MaskPointPolarity;
use shadow_domain::{PhotoGeometry, PhotoQuarterTurn, UnitInterval as DomainUnitInterval};

use super::*;

#[test]
fn identity_geometry_preserves_normalized_prompt_coordinates() {
    let point = foreground(0.2, 0.8);
    let mapped = map_output_prompt_to_original(
        point,
        PhotoGeometry::identity(),
        RasterExtent::new(400, 200).unwrap(),
    );

    assert_close(mapped.x.get(), 0.2);
    assert_close(mapped.y.get(), 0.8);
    assert_eq!(mapped.polarity, MaskPointPolarity::Foreground);
}

#[test]
fn crop_maps_output_edges_back_to_original_crop_edges() {
    let geometry = PhotoGeometry::new(
        domain_unit(0.25),
        domain_unit(0.125),
        domain_unit(0.75),
        domain_unit(0.875),
        PhotoQuarterTurn::Zero,
        false,
        false,
    )
    .unwrap();
    let top_left = map_output_prompt_to_original(
        foreground(0.0, 0.0),
        geometry,
        RasterExtent::new(400, 200).unwrap(),
    );
    let bottom_right = map_output_prompt_to_original(
        foreground(1.0, 1.0),
        geometry,
        RasterExtent::new(400, 200).unwrap(),
    );

    assert_close(top_left.x.get(), 0.25);
    assert_close(top_left.y.get(), 0.125);
    assert_close(bottom_right.x.get(), 0.75);
    assert_close(bottom_right.y.get(), 0.875);
}

#[test]
fn clockwise_rotation_and_flips_follow_the_native_inverse_mapping() {
    let clockwise = PhotoGeometry::new(
        domain_unit(0.0),
        domain_unit(0.0),
        domain_unit(1.0),
        domain_unit(1.0),
        PhotoQuarterTurn::Clockwise90,
        false,
        false,
    )
    .unwrap();
    let mapped = map_output_prompt_to_original(
        foreground(0.0, 0.0),
        clockwise,
        RasterExtent::new(400, 200).unwrap(),
    );
    assert_close(mapped.x.get(), 0.0);
    assert_close(mapped.y.get(), 1.0);

    let flipped = PhotoGeometry::new(
        domain_unit(0.0),
        domain_unit(0.0),
        domain_unit(1.0),
        domain_unit(1.0),
        PhotoQuarterTurn::Zero,
        true,
        true,
    )
    .unwrap();
    let mapped = map_output_prompt_to_original(
        foreground(0.2, 0.7),
        flipped,
        RasterExtent::new(400, 200).unwrap(),
    );
    assert_close(mapped.x.get(), 0.8);
    assert_close(mapped.y.get(), 0.3);
}

#[test]
fn straighten_keeps_the_output_center_at_the_crop_center() {
    let geometry = PhotoGeometry::new(
        domain_unit(0.1),
        domain_unit(0.2),
        domain_unit(0.9),
        domain_unit(0.8),
        PhotoQuarterTurn::Clockwise270,
        true,
        false,
    )
    .unwrap()
    .with_straighten_degrees(17.5)
    .unwrap();
    let mapped = map_output_prompt_to_original(
        foreground(0.5, 0.5),
        geometry,
        RasterExtent::new(1000, 600).unwrap(),
    );

    assert_close(mapped.x.get(), 0.5);
    assert_close(mapped.y.get(), 0.5);
}

#[test]
fn perspective_projects_prompts_through_the_same_interior_homography() {
    let vertical = PhotoGeometry::identity()
        .with_perspective(1.0, 0.0)
        .unwrap();
    let top_left = map_output_prompt_to_original(
        foreground(0.0, 0.0),
        vertical,
        RasterExtent::new(400, 200).unwrap(),
    );
    let bottom_left = map_output_prompt_to_original(
        foreground(0.0, 1.0),
        vertical,
        RasterExtent::new(400, 200).unwrap(),
    );

    assert_close(top_left.x.get(), 0.25);
    assert_close(top_left.y.get(), 0.0);
    assert_close(bottom_left.x.get(), 0.0);
    assert_close(bottom_left.y.get(), 1.0);
}

#[test]
fn identity_projection_preserves_gray8_pixel_centers() {
    let extent = RasterExtent::new(2, 2).unwrap();
    let projected = project_gray8_mask_to_output(
        &[0, 64, 128, 255],
        extent,
        RasterExtent::new(400, 200).unwrap(),
        PhotoGeometry::identity(),
        extent,
    )
    .expect("valid projection");

    assert_eq!(projected, [0, 64, 128, 255]);
}

#[test]
fn projection_applies_final_canvas_flip_without_mutating_source() {
    let samples = [0, 64, 128, 255];
    let extent = RasterExtent::new(2, 2).unwrap();
    let geometry = PhotoGeometry::new(
        domain_unit(0.0),
        domain_unit(0.0),
        domain_unit(1.0),
        domain_unit(1.0),
        PhotoQuarterTurn::Zero,
        true,
        false,
    )
    .unwrap();
    let projected = project_gray8_mask_to_output(
        &samples,
        extent,
        RasterExtent::new(400, 200).unwrap(),
        geometry,
        extent,
    )
    .expect("valid projection");

    assert_eq!(projected, [64, 0, 255, 128]);
    assert_eq!(samples, [0, 64, 128, 255]);
}

fn foreground(x: f64, y: f64) -> MaskPromptPoint {
    MaskPromptPoint {
        x: UnitInterval::new(x).unwrap(),
        y: UnitInterval::new(y).unwrap(),
        polarity: MaskPointPolarity::Foreground,
    }
}

fn domain_unit(value: f64) -> DomainUnitInterval {
    DomainUnitInterval::new(value).unwrap()
}

fn assert_close(actual: f64, expected: f64) {
    assert!(
        (actual - expected).abs() < 1.0e-12,
        "expected {expected}, received {actual}"
    );
}
