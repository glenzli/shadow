//! Real-DNG warm-preview and full-detail edit-session rendering contracts.

use std::path::PathBuf;

use shadow_domain::PreviewCodec;

use crate::{
    AdjustmentRenderNode, AdjustmentRenderOperation, BasicEditParameters, CancellableEditPreview,
    DetailTileRect, DetailTileRequest, EDIT_PREVIEW_ANALYSIS_VERSION, EditPreviewCancellation,
    LibRawEditDetailSession, LibRawEditPreviewSession, MAX_EDIT_DETAIL_RETAINED_BYTES,
    OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION, OklabLightnessToneCurve,
    SensorClippingMask, ToneCurvePoint, basic_adjustment_render_plan,
};

#[test]
#[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
#[allow(clippy::too_many_lines)]
fn real_dng_warm_edit_session_renders_twice() {
    let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));
    std::thread::Builder::new()
        .name("small-edit-worker".to_owned())
        .stack_size(512 * 1_024)
        .spawn(move || {
            let session = LibRawEditPreviewSession::open(&path, 1_024)
                .expect("prepare warm local DNG edit session on a small worker stack");
            let sensor_clipping = session.sensor_clipping_mask();
            assert!(sensor_clipping.available);
            assert_eq!(sensor_clipping.dimensions, session.dimensions());
            assert_eq!(
                sensor_clipping.samples.len() as u64,
                sensor_clipping.dimensions.pixel_count()
            );
            assert_eq!(
                sensor_clipping
                    .samples
                    .iter()
                    .filter(|sample| **sample & SensorClippingMask::HIGHLIGHT_BIT != 0)
                    .count() as u64,
                sensor_clipping.highlight_pixel_count
            );
            assert_eq!(
                sensor_clipping
                    .samples
                    .iter()
                    .filter(|sample| **sample & SensorClippingMask::SHADOW_BIT != 0)
                    .count() as u64,
                sensor_clipping.shadow_pixel_count
            );
            let neutral = session
                .render(BasicEditParameters::default(), 86)
                .expect("render neutral warm preview");
            let neutral_plan = basic_adjustment_render_plan(BasicEditParameters::default())
                .expect("build neutral typed plan");
            let cancelled = EditPreviewCancellation::new().expect("allocate cancellation source");
            assert!(cancelled.cancel());
            assert!(matches!(
                session
                    .render_plan_cancellable(&neutral_plan, 86, &cancelled)
                    .expect("pre-cancelled warm render is control flow"),
                CancellableEditPreview::Cancelled
            ));
            assert!(matches!(
                session
                    .render_plan_with_analysis_cancellable(&neutral_plan, 86, &cancelled)
                    .expect("pre-cancelled analyzed render is control flow"),
                CancellableEditPreview::Cancelled
            ));
            assert!(matches!(
                session
                    .render_plan_rgb8_cancellable(&neutral_plan, &cancelled)
                    .expect("pre-cancelled RGB8 render is control flow"),
                CancellableEditPreview::Cancelled
            ));
            let active_rgb8 =
                EditPreviewCancellation::new().expect("allocate RGB8 cancellation source");
            let rgb8 = session
                .render_plan_rgb8_cancellable(&neutral_plan, &active_rgb8)
                .expect("render neutral RGB8 preview");
            let CancellableEditPreview::Completed(rgb8) = rgb8 else {
                panic!("active RGB8 render must complete");
            };
            assert_eq!(rgb8.codec, shadow_domain::PreviewCodec::Bitmap);
            assert_eq!(rgb8.bytes.len() as u64, rgb8.dimensions.pixel_count() * 3);
            let neutral_from_plan = session
                .render_plan(&neutral_plan, 86)
                .expect("render neutral typed plan");
            let neutral_analyzed = session
                .render_plan_with_analysis(&neutral_plan, 86)
                .expect("render neutral typed plan with analysis");
            let adjusted = session
                .render(
                    BasicEditParameters {
                        exposure_stops: 1.0,
                        contrast_factor: 1.1,
                        white_balance_temperature: 0.12,
                        white_balance_tint: 0.04,
                        saturation_factor: 1.15,
                    },
                    86,
                )
                .expect("render adjusted warm preview");
            let mut curved_plan = basic_adjustment_render_plan(BasicEditParameters::default())
                .expect("build neutral typed plan");
            curved_plan.nodes.insert(
                4,
                AdjustmentRenderNode {
                    node_id: "test-tone-curve".to_owned(),
                    enabled: true,
                    parameter_schema_version: OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
                    implementation_version: OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
                    operation: AdjustmentRenderOperation::OklabLightnessToneCurve {
                        curve: Box::new(OklabLightnessToneCurve {
                            lightness: vec![
                                ToneCurvePoint { x: 0.0, y: 0.0 },
                                ToneCurvePoint { x: 0.5, y: 0.7 },
                                ToneCurvePoint { x: 1.0, y: 1.0 },
                            ],
                        }),
                    },
                },
            );
            let curved = session
                .render_plan(&curved_plan, 86)
                .expect("render typed Tone Curve plan");

            assert_eq!(session.dimensions(), neutral.dimensions);
            assert_eq!(neutral_from_plan.bytes, neutral.bytes);
            assert_eq!(neutral_analyzed.proxy.bytes, neutral.bytes);
            assert_eq!(
                neutral_analyzed.analysis.sample_dimensions,
                neutral.dimensions
            );
            assert_eq!(
                neutral_analyzed.analysis.pixel_count,
                neutral.dimensions.pixel_count()
            );
            for histogram in [
                &neutral_analyzed.analysis.red,
                &neutral_analyzed.analysis.green,
                &neutral_analyzed.analysis.blue,
                &neutral_analyzed.analysis.luma,
            ] {
                assert_eq!(
                    histogram.iter().sum::<u64>(),
                    neutral_analyzed.analysis.pixel_count
                );
            }
            assert_eq!(
                neutral_analyzed.analysis.version,
                EDIT_PREVIEW_ANALYSIS_VERSION
            );
            assert_eq!(adjusted.dimensions, neutral.dimensions);
            assert_eq!(curved.dimensions, neutral.dimensions);
            assert_eq!(neutral.codec, PreviewCodec::Jpeg);
            assert!(neutral.bytes.starts_with(&[0xff, 0xd8]));
            assert!(adjusted.bytes.ends_with(&[0xff, 0xd9]));
            assert_ne!(adjusted.bytes, neutral.bytes);
            assert_ne!(curved.bytes, neutral.bytes);
        })
        .expect("spawn small edit worker")
        .join()
        .expect("small edit worker did not panic");
}

#[test]
#[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
fn real_dng_full_edit_detail_session_renders_deterministic_tiles() {
    let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));
    std::thread::Builder::new()
        .name("small-detail-worker".to_owned())
        .stack_size(512 * 1_024)
        .spawn(move || {
            let session = LibRawEditDetailSession::open(&path)
                .expect("prepare full local DNG detail session on a small worker stack");
            let full = session.dimensions();
            assert!(full.width > 0 && full.height > 0);
            assert!((1..=MAX_EDIT_DETAIL_RETAINED_BYTES).contains(&session.retained_bytes()));
            let width = full.width.min(512);
            let height = full.height.min(512);
            let request = DetailTileRequest {
                rect: DetailTileRect {
                    x: (full.width - width) / 2,
                    y: (full.height - height) / 2,
                    width,
                    height,
                },
            };
            let neutral_plan = basic_adjustment_render_plan(BasicEditParameters::default())
                .expect("build neutral detail plan");
            let first = session
                .render_plan_tile(&neutral_plan, request)
                .expect("render neutral full-resolution detail tile");
            let second = session
                .render_plan_tile(&neutral_plan, request)
                .expect("repeat neutral full-resolution detail tile");
            assert_eq!(first.rect, second.rect);
            assert_eq!(first.full_dimensions, second.full_dimensions);
            assert_eq!(first.row_stride_bytes, second.row_stride_bytes);
            assert_eq!(first.bytes, second.bytes);
            assert_eq!(first.rect, request.rect);
            assert_eq!(first.full_dimensions, full);
            assert_eq!(first.row_stride_bytes, width * 3);
            assert_eq!(
                first.bytes.len(),
                usize::try_from(width * 3)
                    .unwrap()
                    .checked_mul(usize::try_from(height).unwrap())
                    .unwrap()
            );

            let adjusted_plan = basic_adjustment_render_plan(BasicEditParameters {
                exposure_stops: 1.0,
                ..BasicEditParameters::default()
            })
            .expect("build adjusted detail plan");
            let adjusted = session
                .render_plan_tile(&adjusted_plan, request)
                .expect("render adjusted full-resolution detail tile");
            assert_ne!(adjusted.bytes, first.bytes);
        })
        .expect("spawn small detail worker")
        .join()
        .expect("small detail worker did not panic");
}
