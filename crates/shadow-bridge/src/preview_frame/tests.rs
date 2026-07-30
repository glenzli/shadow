use std::{
    path::PathBuf,
    sync::{Arc, Barrier},
};

use shadow_domain::ImageDimensions;

use crate::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentGeometry,
    AdjustmentLocalMask, AdjustmentRenderNode, AdjustmentRenderOperation, AdjustmentRenderPlan,
    BasicEditParameters, CancellableEditPreview, EDIT_PREVIEW_MASK_COVERAGE_VERSION,
    EditPreviewCancellation, EditPreviewMaskCoverageRequest, InteractiveEditPreviewStorage,
    OwnedInteractivePreviewFrame, PhotoEditPreviewSession, basic_adjustment_render_plan, ffi,
};

fn jpeg_fixture_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../apps/desktop/assets/lut-preview-reference.jpg")
}

#[test]
fn owned_interactive_frame_and_native_handle_are_send_sync() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ffi::InteractiveEditPreviewFrameHandle>();
    assert_send_sync::<OwnedInteractivePreviewFrame>();
}

#[test]
fn null_native_owner_maps_only_to_cancelled_terminal() {
    let native = cxx::UniquePtr::<ffi::InteractiveEditPreviewFrameHandle>::null();
    let outcome = OwnedInteractivePreviewFrame::from_nullable_native(
        native,
        ImageDimensions {
            width: 1,
            height: 1,
        },
        None,
    )
    .expect("null cancellation sentinel is not an invalid descriptor");
    assert!(outcome.is_none());
}

#[test]
fn frame_outlives_session_and_keeps_pixel_pointer_after_wrapper_move() {
    let fixture = jpeg_fixture_path();
    let frame = {
        let session =
            PhotoEditPreviewSession::open(&fixture, 64).expect("open JPEG preview session");
        let plan = basic_adjustment_render_plan(BasicEditParameters::default())
            .expect("build neutral preview plan");
        let cancellation =
            EditPreviewCancellation::new().expect("allocate preview cancellation source");
        let completed = session
            .render_plan_interactive_frame_cancellable(&plan, None, &cancellation)
            .expect("render owned interactive frame");
        let CancellableEditPreview::Completed(frame) = completed else {
            panic!("active render must publish one complete frame");
        };
        frame
    };

    let dimensions = frame.dimensions();
    let storage = frame.storage();
    let retained_before_materialization = frame.retained_bytes();
    let pixels = frame
        .materialize_pixels()
        .expect("explicit RGB8 materialization succeeds");
    let pointer = pixels.as_ptr();
    let byte_len = pixels.len();
    assert_eq!(frame.row_stride_bytes(), dimensions.width * 3);
    assert_eq!(
        byte_len,
        usize::try_from(u64::from(frame.row_stride_bytes()) * u64::from(dimensions.height))
            .expect("small fixture byte count fits")
    );
    assert!(frame.mask_coverage().is_none());

    let moved = Box::new(frame);
    assert_eq!(
        moved
            .materialize_pixels()
            .expect("moved frame keeps materialized pixels")
            .as_ptr(),
        pointer
    );
    assert_eq!(
        moved
            .materialize_pixels()
            .expect("moved frame keeps materialized pixels")
            .len(),
        byte_len
    );
    let expected_retained_bytes = match storage {
        InteractiveEditPreviewStorage::HostRgb8 => byte_len,
        InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(_) => {
            retained_before_materialization + byte_len
        }
    };
    assert_eq!(moved.retained_bytes(), expected_retained_bytes);
}

#[test]
fn rgb_and_requested_coverage_share_one_stable_owner() {
    let fixture = jpeg_fixture_path();
    let session = PhotoEditPreviewSession::open(&fixture, 64).expect("open JPEG preview session");
    let plan = single_masked_layer_plan();
    let request = EditPreviewMaskCoverageRequest {
        target_layer_index: 0,
        mask_selection_revision: 73,
    };
    let cancellation =
        EditPreviewCancellation::new().expect("allocate preview cancellation source");
    let completed = session
        .render_plan_interactive_frame_cancellable(&plan, Some(request), &cancellation)
        .expect("render owned frame with paired coverage");
    let CancellableEditPreview::Completed(frame) = completed else {
        panic!("active coverage render must complete");
    };

    let rgb_pointer = frame
        .materialize_pixels()
        .expect("materialize paired RGB frame")
        .as_ptr();
    let coverage = frame
        .mask_coverage()
        .expect("requested coverage must be present");
    let coverage_pointer = coverage.samples.as_ptr();
    assert_eq!(coverage.version, EDIT_PREVIEW_MASK_COVERAGE_VERSION);
    assert_eq!(coverage.target_layer_index, 0);
    assert_eq!(coverage.mask_selection_revision, 73);
    assert_eq!(coverage.dimensions, frame.dimensions());
    assert_eq!(coverage.row_stride_bytes, coverage.dimensions.width);
    assert_eq!(
        coverage.samples.len(),
        usize::try_from(coverage.dimensions.pixel_count()).expect("small coverage dimensions fit")
    );

    let moved = Box::new(frame);
    assert_eq!(
        moved
            .materialize_pixels()
            .expect("moved paired frame keeps RGB")
            .as_ptr(),
        rgb_pointer
    );
    assert_eq!(
        moved
            .mask_coverage()
            .expect("coverage stays paired after move")
            .samples
            .as_ptr(),
        coverage_pointer
    );
}

#[test]
fn native_metal_descriptor_crosses_unchanged_and_materializes_exact_rgb8() {
    let fixture = jpeg_fixture_path();
    let session = PhotoEditPreviewSession::open(&fixture, 64).expect("open JPEG preview session");
    let plan = basic_adjustment_render_plan(BasicEditParameters::default())
        .expect("build neutral preview plan");

    let legacy_cancellation =
        EditPreviewCancellation::new().expect("allocate legacy preview cancellation source");
    let legacy = session
        .render_plan_rgb8_cancellable(&plan, &legacy_cancellation)
        .expect("render legacy RGB8 reference");
    let CancellableEditPreview::Completed(legacy) = legacy else {
        panic!("active legacy render must complete");
    };

    let interactive_cancellation =
        EditPreviewCancellation::new().expect("allocate interactive cancellation source");
    let interactive = session
        .render_plan_interactive_frame_cancellable(&plan, None, &interactive_cancellation)
        .expect("render owned interactive frame");
    let CancellableEditPreview::Completed(frame) = interactive else {
        panic!("active interactive render must complete");
    };

    let storage = frame.storage();
    let presentation_failure_forced =
        std::env::var("SHADOW_TEST_WARM_METAL_FORCE_PRESENTATION_PIPELINE_FAILURE")
            .is_ok_and(|value| value == "1");
    if std::env::var_os("SHADOW_TEST_REQUIRE_WARM_METAL").is_some() && !presentation_failure_forced
    {
        assert!(
            matches!(
                storage,
                InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(_)
            ),
            "required Metal run must publish a native texture owner"
        );
    }
    if presentation_failure_forced {
        assert_eq!(
            storage,
            InteractiveEditPreviewStorage::HostRgb8,
            "a named presentation failure must fall back to the completed host RGB8 frame"
        );
        assert!(
            frame
                .presentation_fallback_diagnostic()
                .contains("test-injected Metal presentation surface pipeline failure")
        );
    }
    if let InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(texture) = storage {
        assert_eq!(
            frame.materialized_pixel_bytes(),
            0,
            "crossing CXX and Rust must not eagerly read the texture back"
        );
        assert!(frame.presentation_fallback_diagnostic().is_empty());
        assert_eq!(
            frame.retained_bytes(),
            usize::try_from(
                u64::from(texture.row_stride_bytes) * u64::from(frame.dimensions().height)
            )
            .expect("bounded preview texture byte count fits")
        );
    }

    let materialized = frame
        .materialize_pixels()
        .expect("explicit native-frame materialization succeeds");
    assert_eq!(
        materialized, legacy.bytes,
        "native surface materialization must equal the ordinary RGB8 renderer byte for byte"
    );
    assert_eq!(
        frame.storage(),
        storage,
        "materialization must not replace or mutate the native texture descriptor"
    );
    assert_eq!(frame.materialized_pixel_bytes(), materialized.len());
}

#[test]
fn descriptor_reads_and_lazy_materialization_are_concurrently_stable() {
    let fixture = jpeg_fixture_path();
    let session = PhotoEditPreviewSession::open(&fixture, 64).expect("open JPEG preview session");
    let plan = basic_adjustment_render_plan(BasicEditParameters::default())
        .expect("build neutral preview plan");
    let cancellation =
        EditPreviewCancellation::new().expect("allocate preview cancellation source");
    let completed = session
        .render_plan_interactive_frame_cancellable(&plan, None, &cancellation)
        .expect("render owned interactive frame");
    let CancellableEditPreview::Completed(frame) = completed else {
        panic!("active render must publish one complete frame");
    };
    let storage = frame.storage();
    let initial_materialized_bytes = frame.materialized_pixel_bytes();
    if matches!(
        storage,
        InteractiveEditPreviewStorage::AppleMetalRgba8Srgb(_)
    ) {
        assert_eq!(initial_materialized_bytes, 0);
    }

    let frame = Arc::new(frame);
    let barrier = Arc::new(Barrier::new(8));
    let mut workers = Vec::new();
    for index in 0..8 {
        let frame = Arc::clone(&frame);
        let barrier = Arc::clone(&barrier);
        workers.push(std::thread::spawn(move || {
            barrier.wait();
            if index % 2 == 0 {
                let pixels = frame
                    .materialize_pixels()
                    .expect("concurrent materialization succeeds");
                (pixels.as_ptr() as usize, pixels.len())
            } else {
                for _ in 0..1_000 {
                    assert_eq!(frame.storage(), storage);
                    let _ = frame.materialized_pixel_bytes();
                    let _ = frame.retained_bytes();
                }
                let pixels = frame
                    .materialize_pixels()
                    .expect("descriptor reader observes published materialization");
                (pixels.as_ptr() as usize, pixels.len())
            }
        }));
    }
    let observations: Vec<_> = workers
        .into_iter()
        .map(|worker| {
            worker
                .join()
                .expect("materialization worker does not panic")
        })
        .collect();
    assert!(observations.iter().all(|entry| *entry == observations[0]));
    assert_eq!(frame.storage(), storage);
    assert_eq!(frame.materialized_pixel_bytes(), observations[0].1);
}

#[test]
fn pre_cancelled_render_publishes_no_native_frame() {
    let fixture = jpeg_fixture_path();
    let session = PhotoEditPreviewSession::open(&fixture, 64).expect("open JPEG preview session");
    let plan = basic_adjustment_render_plan(BasicEditParameters::default())
        .expect("build neutral preview plan");
    let cancellation =
        EditPreviewCancellation::new().expect("allocate preview cancellation source");
    assert!(cancellation.cancel());
    assert!(matches!(
        session
            .render_plan_interactive_frame_cancellable(&plan, None, &cancellation)
            .expect("cooperative cancellation is control flow"),
        CancellableEditPreview::Cancelled
    ));
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
                    mask: Some(AdjustmentLocalMask::LinearGradient {
                        start_x: 0.0,
                        start_y: 0.0,
                        end_x: 1.0,
                        end_y: 1.0,
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
