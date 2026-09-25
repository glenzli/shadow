use super::*;
fn basis(m: [f64; 9]) -> ImageCompletionColorBasis {
    ImageCompletionColorBasis {
        matrix_bits: m.map(f64::to_bits),
        calibration_id: "same-camera-profile".into(),
    }
}
fn apply(m: [f64; 9], p: [f64; 3]) -> [f64; 3] {
    std::array::from_fn(|i| (0..3).map(|k| m[i * 3 + k] * p[k]).sum())
}
#[test]
fn completion_source_response_follows_camera_calibration_without_rewriting_content() {
    let old = basis([1.3, -0.2, 0.1, -0.1, 1.1, 0., 0.1, -0.3, 1.2]);
    let new = basis([1.8, -0.3, 0.2, -0.2, 1.2, 0.1, 0.05, -0.2, 0.8]);
    for camera in [[0.1, 0.2, 0.3], [2.0, 0.8, -0.04], [1.2, 1.2, 1.2]] {
        let accepted = apply(old.matrix(), camera);
        let expected = apply(new.matrix(), camera);
        let result = apply(response(&old, &new).unwrap(), accepted);
        for c in 0..3 {
            assert!((expected[c] - result[c]).abs() < 1e-10);
        }
        // Every render uses the original accepted basis, never the preceding slider result.
        assert_eq!(response(&old, &old), Some(IDENTITY));
        assert_eq!(accepted, apply(IDENTITY, accepted));
    }
}
#[test]
fn completion_source_response_does_not_guess_across_profiles_or_singular_bases() {
    let old = basis(IDENTITY);
    let mut changed = old.clone();
    changed.calibration_id = "different".into();
    assert_eq!(response(&old, &changed), None);
    changed = old.clone();
    changed.matrix_bits = [0; 9];
    assert_eq!(response(&old, &changed), None);
}

#[test]
fn completion_wire_binds_source_response_for_preview_and_detail_without_pixel_copies() {
    let original = basis(IDENTITY);
    let current = basis([1.5, 0.1, 0., 0., 1., 0., 0., -0.1, 0.7]);
    let mut plan =
        crate::basic_adjustment_render_plan(crate::BasicEditParameters::default()).unwrap();
    plan.nodes.truncate(1);
    plan.nodes[0].operation = AdjustmentRenderOperation::ImageCompletion {
        patches: vec![crate::AdjustmentImageCompletionPatch {
            raster_width: 1,
            raster_height: 1,
            coordinate_width: 100,
            coordinate_height: 100,
            bounds_left: 0.,
            bounds_top: 0.,
            bounds_right: 1.,
            bounds_bottom: 1.,
            strength: 1.,
            linear_rgba_f32: true,
            source_color_basis: Some(original),
            rgba8: [2.0_f32, 0.2, -0.03, 1.]
                .into_iter()
                .flat_map(f32::to_le_bytes)
                .collect(),
        }],
    };
    plan.validate().unwrap();
    let receipt = RawPipelineReceipt {
        completion_color_basis: Some(current.clone()),
        ..Default::default()
    };
    let mut wire = crate::render_wire::ffi_render_request(&plan, 100, 95);
    let bytes = wire.nodes[0].payload.clone();
    bind(&mut wire.nodes, &plan, &receipt);
    assert_eq!(&wire.nodes[0].parameters[10..19], current.matrix());
    assert_eq!(wire.nodes[0].payload, bytes);
    let mut tile = crate::render_wire::ffi_detail_tile_request(
        &plan,
        crate::DetailTileRequest {
            rect: crate::DetailTileRect {
                x: 10,
                y: 20,
                width: 30,
                height: 40,
            },
        },
    );
    bind(&mut tile.nodes, &plan, &receipt);
    assert_eq!(tile.nodes[0].parameters, wire.nodes[0].parameters);
    assert_eq!(tile.nodes[0].payload, bytes);
}
