//! Full-resolution detail-session bounds and tile-request contracts.

use shadow_domain::ImageDimensions;

use crate::{
    AdjustmentGeometry, AdjustmentLiquify, AdjustmentLiquifyPoint, AdjustmentLiquifyPushStroke,
    AdjustmentRenderNode, AdjustmentRenderOperation, AdjustmentRenderPlan, BridgeError,
    DetailSessionRequirements, DetailTileRect, DetailTileRequest, LibRawEditDetailSession,
    MAX_EDIT_DETAIL_RETAINED_BYTES, MAX_EDIT_DETAIL_TILE_SIDE, OpticsSettings,
    PhotoEditDetailSession, RawDevelopmentPlan,
};

fn liquify() -> AdjustmentLiquify {
    AdjustmentLiquify {
        strokes: vec![AdjustmentLiquifyPushStroke {
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
        }],
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

#[test]
fn detail_source_requirements_are_derived_from_the_complete_structural_plan() {
    let ordinary = DetailSessionRequirements::for_render_plan(&plan(None));
    assert!(!ordinary.requires_cpu_replay());

    let structural = DetailSessionRequirements::for_render_plan(&plan(Some(liquify())));
    assert!(structural.requires_cpu_replay());
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
    assert_eq!(MAX_EDIT_DETAIL_RETAINED_BYTES, 1_024 * 1_024 * 1_024);

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
