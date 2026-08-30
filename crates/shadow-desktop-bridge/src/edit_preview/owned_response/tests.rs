use std::path::PathBuf;

use shadow_bridge::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentGeometry,
    AdjustmentLocalMask, AdjustmentLocalMaskComponent, AdjustmentMaskComponentOperation,
    AdjustmentRenderNode, AdjustmentRenderOperation, AdjustmentRenderPlan, CancellableEditPreview,
    EDIT_PREVIEW_MASK_COVERAGE_SCHEMA_VERSION, EditPreviewCancellation,
    EditPreviewMaskCoverageRequest, PhotoEditPreviewSession,
};

use super::*;

fn jpeg_fixture_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../apps/desktop/assets/lut-preview-reference.jpg")
}

#[test]
fn owned_desktop_response_is_send_sync() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<OwnedEditedPreview>();
}

#[test]
fn materialized_response_preserves_the_existing_projection_without_frame_views() {
    let projection = crate::edit_preview::cancelled_edited_preview();
    let owner = OwnedEditedPreview::materialized(projection);

    assert_eq!(
        owner.projection().terminal,
        ffi::FfiEditPreviewTerminal::Cancelled
    );
    assert!(!owner.interactive_frame_available());
    assert!(
        owner
            .interactive_pixels()
            .expect("materialized response has an empty compatibility slice")
            .is_empty()
    );
    assert!(owner.interactive_mask_coverage_samples().is_empty());
    assert_eq!(owner.interactive_retained_bytes(), 0);

    let projection = owner
        .into_materialized_projection()
        .expect("cancelled projection materializes");
    assert_eq!(projection.terminal, ffi::FfiEditPreviewTerminal::Cancelled);
    assert!(projection.bytes.is_empty());
    assert!(projection.mask_coverage_samples.is_empty());
}

#[test]
fn interactive_owner_keeps_projection_and_borrowed_payloads_atomic_across_moves() {
    let fixture = interactive_owner_fixture();
    assert_interactive_projection(&fixture);
    assert_borrowed_payloads(&fixture);
    assert_storage_projection(&fixture);
    assert_materialized_projection(fixture);
}

struct InteractiveOwnerFixture {
    owner: Box<OwnedEditedPreview>,
    storage: InteractiveEditPreviewStorage,
    retained_before_materialization: usize,
    expected_rgb: Vec<u8>,
    expected_coverage: Vec<u8>,
    coverage_pointer: *const u8,
}

fn interactive_owner_fixture() -> InteractiveOwnerFixture {
    let fixture = jpeg_fixture_path();
    let session = PhotoEditPreviewSession::open(&fixture, 64).expect("open JPEG preview session");
    let plan = single_masked_layer_plan();
    let request = EditPreviewMaskCoverageRequest {
        target_layer_index: 0,
        target_component_index: Some(1),
        mask_selection_revision: 73,
    };
    let reference_cancellation =
        EditPreviewCancellation::new().expect("allocate reference cancellation source");
    let reference = session
        .render_plan_rgb8_cancellable(&plan, &reference_cancellation)
        .expect("render materialized RGB8 reference");
    let CancellableEditPreview::Completed(reference) = reference else {
        panic!("active reference render must complete");
    };
    let cancellation =
        EditPreviewCancellation::new().expect("allocate preview cancellation source");
    let completed = session
        .render_plan_interactive_frame_cancellable(&plan, Some(request), &cancellation)
        .expect("render owned interactive desktop frame");
    let CancellableEditPreview::Completed(frame) = completed else {
        panic!("active interactive render must complete");
    };
    let optics = session.optics_receipt().clone();
    let level_zero_dimensions = session.level_zero_dimensions();
    drop(session);

    let storage = frame.storage();
    let retained_before_materialization = frame.retained_bytes();
    if std::env::var_os("SHADOW_TEST_REQUIRE_WARM_METAL").is_some() {
        assert!(
            matches!(
                storage,
                InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(_)
            ),
            "required Metal run must reach the desktop opaque owner as a native texture"
        );
    }
    if matches!(
        storage,
        InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(_)
    ) {
        assert_eq!(frame.materialized_pixel_bytes(), 0);
    }
    let expected_rgb = reference.bytes;
    let expected_coverage = frame
        .mask_coverage()
        .expect("requested coverage must be paired")
        .samples
        .to_vec();
    let coverage_pointer = frame
        .mask_coverage()
        .expect("requested coverage must be paired")
        .samples
        .as_ptr();

    let owner = move_owner(OwnedEditedPreview::interactive(
        frame,
        level_zero_dimensions,
        &optics,
    ));
    InteractiveOwnerFixture {
        owner,
        storage,
        retained_before_materialization,
        expected_rgb,
        expected_coverage,
        coverage_pointer,
    }
}

fn assert_interactive_projection(fixture: &InteractiveOwnerFixture) {
    let projection = fixture.owner.projection();
    assert_eq!(projection.terminal, ffi::FfiEditPreviewTerminal::Completed);
    assert_eq!(
        (projection.level_zero_width, projection.level_zero_height),
        (960, 640),
        "the bounded frame retains its exact level-zero source geometry"
    );
    assert_eq!(projection.row_stride_bytes, projection.width * 3);
    assert!(projection.bytes.is_empty());
    assert!(projection.mask_coverage_available);
    assert_eq!(
        projection.mask_coverage_version,
        EDIT_PREVIEW_MASK_COVERAGE_SCHEMA_VERSION
    );
    assert_eq!(projection.mask_coverage_target_layer_index, 0);
    assert!(projection.mask_coverage_component_selected);
    assert_eq!(projection.mask_coverage_target_component_index, 1);
    assert_eq!(projection.mask_selection_revision, 73);
    assert_eq!(projection.mask_coverage_width, projection.width);
    assert_eq!(projection.mask_coverage_height, projection.height);
    assert_eq!(
        projection.mask_coverage_row_stride_bytes,
        projection.mask_coverage_width
    );
    assert!(projection.mask_coverage_samples.is_empty());
}

fn assert_borrowed_payloads(fixture: &InteractiveOwnerFixture) {
    assert_eq!(
        fixture.owner.interactive_materialized_pixel_bytes(),
        match fixture.storage {
            InteractiveEditPreviewStorage::HostRgb8 => fixture.expected_rgb.len(),
            InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(_) => 0,
        }
    );
    assert_eq!(
        fixture.owner.interactive_retained_bytes(),
        fixture.retained_before_materialization
    );
    let rgb_pointer = fixture
        .owner
        .interactive_pixels()
        .expect("desktop owner explicitly materializes RGB")
        .as_ptr();
    assert_eq!(
        fixture
            .owner
            .interactive_pixels()
            .expect("desktop owner materializes RGB")
            .as_ptr(),
        rgb_pointer
    );
    assert_eq!(
        fixture.owner.interactive_mask_coverage_samples().as_ptr(),
        fixture.coverage_pointer
    );
    assert_eq!(
        fixture.owner.interactive_retained_bytes(),
        match fixture.storage {
            InteractiveEditPreviewStorage::HostRgb8 => fixture.retained_before_materialization,
            InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(_) => {
                fixture.retained_before_materialization + fixture.expected_rgb.len()
            }
        }
    );
    assert_eq!(
        fixture
            .owner
            .interactive_pixels()
            .expect("desktop owner returns exact RGB8 materialization"),
        fixture.expected_rgb
    );
}

fn assert_storage_projection(fixture: &InteractiveOwnerFixture) {
    match fixture.storage {
        InteractiveEditPreviewStorage::HostRgb8 => {
            assert_eq!(
                fixture.owner.interactive_storage_kind(),
                HOST_RGB8_STORAGE_KIND
            );
            assert_eq!(fixture.owner.interactive_native_texture_handle(), 0);
        }
        InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(texture) => {
            assert_eq!(
                fixture.owner.interactive_storage_kind(),
                APPLE_METAL_RGBA8_SRGB_STORAGE_KIND
            );
            assert_eq!(
                fixture.owner.interactive_native_texture_handle(),
                texture.texture_handle
            );
            assert_eq!(
                fixture.owner.interactive_native_device_handle(),
                texture.device_handle
            );
            assert_eq!(
                fixture.owner.interactive_native_resource_id(),
                texture.resource_id
            );
        }
    }
}

fn assert_materialized_projection(fixture: InteractiveOwnerFixture) {
    let materialized = fixture
        .owner
        .into_materialized_projection()
        .expect("owned response explicitly materializes");
    assert_eq!(materialized.bytes, fixture.expected_rgb);
    assert_eq!(
        materialized.mask_coverage_samples,
        fixture.expected_coverage
    );
}

fn move_owner(owner: Box<OwnedEditedPreview>) -> Box<OwnedEditedPreview> {
    owner
}

fn single_masked_layer_plan() -> AdjustmentRenderPlan {
    let node = |node_id: &str, operation| AdjustmentRenderNode {
        node_id: node_id.to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation,
    };
    AdjustmentRenderPlan {
        nodes: vec![
            node(
                "layer-start",
                AdjustmentRenderOperation::LocalMaskLayerStart {
                    opacity: 1.0,
                    mask: Some(AdjustmentLocalMask::Composite {
                        components: vec![
                            AdjustmentLocalMaskComponent {
                                operation: AdjustmentMaskComponentOperation::Base,
                                enabled: true,
                                mask: AdjustmentLocalMask::LinearGradient {
                                    start_x: 0.0,
                                    start_y: 0.0,
                                    end_x: 1.0,
                                    end_y: 1.0,
                                    invert: false,
                                },
                            },
                            AdjustmentLocalMaskComponent {
                                operation: AdjustmentMaskComponentOperation::Add,
                                enabled: true,
                                mask: AdjustmentLocalMask::RadialGradient {
                                    center_x: 0.5,
                                    center_y: 0.5,
                                    radius_x: 0.25,
                                    radius_y: 0.2,
                                    feather: 0.5,
                                    invert: false,
                                },
                            },
                        ],
                        invert: false,
                    }),
                },
            ),
            node(
                "layer-exposure",
                AdjustmentRenderOperation::Exposure { stops: 0.25 },
            ),
            node("layer-end", AdjustmentRenderOperation::LocalMaskLayerEnd),
        ],
        liquify: None,
        geometry: AdjustmentGeometry::default(),
    }
}
