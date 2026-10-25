//! Full-resolution detail-session bounds and tile-request contracts.

use shadow_domain::ImageDimensions;

use crate::{
    AdjustmentGeometry, AdjustmentLiquify, AdjustmentLiquifyPoint, AdjustmentLiquifyPushStroke,
    AdjustmentLiquifyStroke, AdjustmentLocalMask, AdjustmentLocalMaskComponent,
    AdjustmentMaskComponentOperation, AdjustmentRenderNode, AdjustmentRenderOperation,
    AdjustmentRenderPlan, BridgeError, DetailSessionRequirements, DetailTileRect,
    DetailTileRenderBackend, DetailTileRequest, LibRawEditDetailSession,
    MAX_EDIT_DETAIL_RETAINED_BYTES, MAX_EDIT_DETAIL_TILE_SIDE, OpticsSettings,
    PhotoEditDetailSession, RawDevelopmentPlan,
};

fn liquify() -> AdjustmentLiquify {
    AdjustmentLiquify {
        enabled: true,
        strokes: vec![AdjustmentLiquifyStroke::Push(AdjustmentLiquifyPushStroke {
            points: vec![
                AdjustmentLiquifyPoint {
                    x: 0.25,
                    y: 0.5,
                    pressure: 1.0,
                },
                AdjustmentLiquifyPoint {
                    x: 0.75,
                    y: 0.5,
                    pressure: 1.0,
                },
            ],
            radius: 0.1,
            strength: 0.75,
            hardness: 0.5,
        })],
    }
}

fn plan(liquify: Option<AdjustmentLiquify>) -> AdjustmentRenderPlan {
    AdjustmentRenderPlan {
        nodes: vec![AdjustmentRenderNode {
            node_id: "neutral-exposure".to_owned(),
            parameter_schema_version: 1,
            implementation_version: 1,
            enabled: true,
            operation: AdjustmentRenderOperation::Exposure { stops: 0.0 },
        }],
        liquify,
        geometry: AdjustmentGeometry::identity(),
    }
}

fn composite_plan(layer_enabled: bool, opacity: f64) -> AdjustmentRenderPlan {
    let leaf = AdjustmentLocalMask::LinearGradient {
        start_x: 0.1,
        start_y: 0.2,
        end_x: 0.9,
        end_y: 0.8,
        invert: false,
    };
    AdjustmentRenderPlan {
        nodes: vec![
            AdjustmentRenderNode {
                node_id: "composite-start".to_owned(),
                parameter_schema_version: 1,
                implementation_version: 1,
                enabled: layer_enabled,
                operation: AdjustmentRenderOperation::LocalMaskLayerStart {
                    opacity,
                    mask: Some(AdjustmentLocalMask::Composite {
                        components: vec![
                            AdjustmentLocalMaskComponent {
                                operation: AdjustmentMaskComponentOperation::Base,
                                enabled: true,
                                mask: leaf.clone(),
                            },
                            AdjustmentLocalMaskComponent {
                                operation: AdjustmentMaskComponentOperation::Add,
                                enabled: true,
                                mask: leaf,
                            },
                        ],
                        invert: false,
                    }),
                },
            },
            AdjustmentRenderNode {
                node_id: "composite-exposure".to_owned(),
                parameter_schema_version: 1,
                implementation_version: 1,
                enabled: true,
                operation: AdjustmentRenderOperation::Exposure { stops: 0.2 },
            },
            AdjustmentRenderNode {
                node_id: "composite-end".to_owned(),
                parameter_schema_version: 1,
                implementation_version: 1,
                enabled: true,
                operation: AdjustmentRenderOperation::LocalMaskLayerEnd,
            },
        ],
        liquify: None,
        geometry: AdjustmentGeometry::identity(),
    }
}

#[test]
fn detail_source_requirements_are_derived_from_the_complete_structural_plan() {
    let ordinary = DetailSessionRequirements::for_render_plan(&plan(None));
    assert!(!ordinary.requires_cpu_replay());

    let structural = DetailSessionRequirements::for_render_plan(&plan(Some(liquify())));
    assert!(structural.requires_cpu_replay());

    let mut bypassed = liquify();
    bypassed.enabled = false;
    let bypassed_structural = DetailSessionRequirements::for_render_plan(&plan(Some(bypassed)));
    assert!(
        bypassed_structural.requires_cpu_replay(),
        "bypass preserves CPU-capable source admission so re-enable never reopens the photo"
    );

    for composite in [
        composite_plan(true, 1.0),
        composite_plan(false, 1.0),
        composite_plan(true, 0.0),
    ] {
        assert!(
            DetailSessionRequirements::for_render_plan(&composite).requires_cpu_replay(),
            "composite topology keeps the detail source CPU-capable across bypass and opacity edits"
        );
    }

    assert!(
        DetailSessionRequirements::for_high_bit_export(&plan(None)).requires_cpu_replay(),
        "RGB16 export must not admit a Metal-only retained source"
    );
}

#[test]
fn prepared_raster_detail_source_reports_the_required_cpu_replay_capability() {
    let source = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../apps/desktop/assets/lut-preview-reference.jpg");
    let requirements = DetailSessionRequirements::for_render_plan(&plan(Some(liquify())));
    let session = PhotoEditDetailSession::open_with_requirements(
        &source,
        RawDevelopmentPlan::detail(),
        &OpticsSettings::default(),
        requirements,
    )
    .expect("tracked raster fixture prepares one CPU-replay-capable detail session");
    assert!(session.cpu_replay_available());
    assert!(session.satisfies_requirements(requirements));
}

#[test]
fn prepared_raster_detail_source_renders_exact_rgb16_samples_through_cpu_export() {
    let source = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../apps/desktop/assets/lut-preview-reference.jpg");
    let mut export_plan = plan(None);
    export_plan.nodes[0].operation = AdjustmentRenderOperation::Exposure { stops: 0.37 };
    let requirements = DetailSessionRequirements::for_high_bit_export(&export_plan);
    let session = PhotoEditDetailSession::open_with_requirements(
        &source,
        RawDevelopmentPlan::detail(),
        &OpticsSettings::default(),
        requirements,
    )
    .expect("tracked raster fixture prepares one high-bit export session");
    let request = DetailTileRequest {
        rect: DetailTileRect {
            x: 0,
            y: 0,
            width: 8,
            height: 8,
        },
    };

    let rendered = session
        .render_plan_tile16(&export_plan, request)
        .expect("high-bit detail render succeeds through the direct CPU boundary");

    assert_eq!(rendered.rect, request.rect);
    assert_eq!(rendered.row_stride_bytes, 8 * 3 * 2);
    assert_eq!(rendered.samples.len(), 8 * 8 * 3);
    assert_eq!(rendered.execution.backend, DetailTileRenderBackend::Cpu);
    assert!(!rendered.execution.fell_back);
    assert!(
        rendered.samples.iter().any(|sample| sample % 257 != 0),
        "the RGB16 bridge must preserve native high-bit results rather than expand RGB8 values"
    );
}

#[test]
fn full_edit_detail_contract_is_send_sync_and_rejects_invalid_rectangles_locally() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<LibRawEditDetailSession>();
    assert_send_sync::<PhotoEditDetailSession>();
    assert_eq!(
        std::any::TypeId::of::<PhotoEditDetailSession>(),
        std::any::TypeId::of::<LibRawEditDetailSession>(),
        "the source-neutral detail API must retain the existing bounded session type"
    );
    assert_eq!(MAX_EDIT_DETAIL_TILE_SIDE, 1_024);
    // Scene-linear panoramas use the native 2 GiB cap; packed RGB and CFA
    // retain their smaller source-specific limits in the native session.
    assert_eq!(MAX_EDIT_DETAIL_RETAINED_BYTES, 2 * 1_024 * 1_024 * 1_024);

    let dimensions = ImageDimensions {
        width: 4_000,
        height: 3_000,
    };
    for rect in [
        DetailTileRect {
            x: 0,
            y: 0,
            width: 0,
            height: 1,
        },
        DetailTileRect {
            x: 0,
            y: 0,
            width: MAX_EDIT_DETAIL_TILE_SIDE + 1,
            height: 1,
        },
        DetailTileRect {
            x: dimensions.width,
            y: 0,
            width: 1,
            height: 1,
        },
        DetailTileRect {
            x: dimensions.width - 1,
            y: 0,
            width: 2,
            height: 1,
        },
        DetailTileRect {
            x: u32::MAX,
            y: 0,
            width: 1,
            height: 1,
        },
    ] {
        assert!(matches!(
            DetailTileRequest { rect }.validate(dimensions),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }
    DetailTileRequest {
        rect: DetailTileRect {
            x: dimensions.width - 1_024,
            y: dimensions.height - 1_024,
            width: 1_024,
            height: 1_024,
        },
    }
    .validate(dimensions)
    .expect("maximum in-bounds detail tile is valid without opening a RAW");
}

#[test]
fn rgb8_node_and_layer_auto_fallback_receipts_stay_consistent() {
    let source = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../apps/desktop/assets/lut-preview-reference.jpg");
    let session = PhotoEditDetailSession::open(&source).unwrap();
    let node_plan = plan(None);
    let mut layer_plan = node_plan.clone();
    layer_plan.nodes.insert(
        0,
        AdjustmentRenderNode {
            node_id: "neutral-layer-start".to_owned(),
            parameter_schema_version: 1,
            implementation_version: 1,
            enabled: true,
            operation: AdjustmentRenderOperation::LocalMaskLayerStart {
                opacity: 1.0,
                mask: None,
            },
        },
    );
    layer_plan.nodes.push(AdjustmentRenderNode {
        node_id: "neutral-layer-end".to_owned(),
        parameter_schema_version: 1,
        implementation_version: 1,
        enabled: true,
        operation: AdjustmentRenderOperation::LocalMaskLayerEnd,
    });
    let request = DetailTileRequest {
        rect: DetailTileRect {
            x: 0,
            y: 0,
            width: 8,
            height: 8,
        },
    };
    let node_tile = session.render_plan_tile(&node_plan, request).unwrap();
    let layer_tile = session.render_plan_tile(&layer_plan, request).unwrap();
    assert_eq!(node_tile.bytes, layer_tile.bytes);

    // The normal run uses auto (including on no-Metal builds). Explicit CPU remains a valid
    // caller policy and must report direct execution instead of fabricating fallback.
    let forced_cpu = std::env::var("SHADOW_IMAGE_ACCELERATION").is_ok_and(|mode| mode == "cpu");
    for tile in [node_tile, layer_tile] {
        if tile.execution.backend == DetailTileRenderBackend::Cpu && !forced_cpu {
            assert!(tile.execution.fell_back);
            assert!(
                tile.execution
                    .diagnostic
                    .as_deref()
                    .is_some_and(|diagnostic| !diagnostic.is_empty())
            );
        } else {
            assert!(!tile.execution.fell_back);
            assert!(tile.execution.diagnostic.is_none());
        }
    }
}
