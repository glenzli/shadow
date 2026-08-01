//! Full-detail viewport geometry, admission, tiling, and render-token contracts.

use std::collections::BTreeSet;

use shadow_domain::ImageDimensions;

use crate::{
    detail_viewport::{detail_viewport_rects, validate_detail_viewport_request},
    ffi,
    tests::fixtures::{edit_session::test_edit_session, grade_stack::ffi_parameters},
};

#[test]
fn detail_viewport_tiles_cover_center_and_clipped_edges_without_duplicates() {
    let dimensions = ImageDimensions {
        width: 1_300,
        height: 900,
    };
    let center =
        detail_viewport_rects(dimensions, 0.5, 0.5, 700, 600, 512).expect("tile centered viewport");
    assert_eq!(center.len(), 4);
    let unique = center
        .iter()
        .map(|rect| (rect.x, rect.y, rect.width, rect.height))
        .collect::<BTreeSet<_>>();
    assert_eq!(unique.len(), center.len());
    assert!(center.iter().all(|rect| {
        rect.x + rect.width <= dimensions.width && rect.y + rect.height <= dimensions.height
    }));

    let bottom_right =
        detail_viewport_rects(dimensions, 1.0, 1.0, 512, 512, 512).expect("tile edge viewport");
    assert_eq!(
        bottom_right,
        [shadow_bridge::DetailTileRect {
            x: 788,
            y: 388,
            width: 512,
            height: 512,
        }]
    );

    let retina_loupe = detail_viewport_rects(
        ImageDimensions {
            width: 8_256,
            height: 5_504,
        },
        0.5,
        0.5,
        588,
        320,
        1_024,
    )
    .expect("bound one exact Retina loupe region");
    assert_eq!(retina_loupe.len(), 1);
    assert_eq!(retina_loupe[0].width, 588);
    assert_eq!(retina_loupe[0].height, 320);
}

#[test]
fn detail_viewport_geometry_fails_closed() {
    let dimensions = ImageDimensions {
        width: 1_300,
        height: 900,
    };
    for (center_x, tile_side) in [(f64::NAN, 512), (0.5, 0), (0.5, 1_025)] {
        assert!(detail_viewport_rects(dimensions, center_x, 0.5, 700, 600, tile_side).is_err());
    }
    assert!(
        detail_viewport_rects(
            ImageDimensions {
                width: 0,
                height: 900,
            },
            0.5,
            0.5,
            700,
            600,
            512,
        )
        .is_err()
    );
}

#[test]
fn detail_request_rejects_an_excessive_grid_before_source_work() {
    let mut request = ffi::FfiEditDetailViewportRequest {
        base_commit_id: String::new(),
        settings: ffi_parameters(0.0, 1.0, [0.0; 2], 1.0),
        render_token: 1,
        center_x: 0.5,
        center_y: 0.5,
        viewport_width: 4_096,
        viewport_height: 4_096,
        tile_side: 512,
        use_working_recipe: true,
    };
    validate_detail_viewport_request(&request).expect("the desktop 512px grid is admitted");

    request.viewport_width = 6_016;
    request.viewport_height = 3_384;
    request.tile_side = 1_024;
    validate_detail_viewport_request(&request)
        .expect("an adaptive 1024px grid admits a 6K display viewport");

    request.viewport_width = 8_193;
    assert!(validate_detail_viewport_request(&request).is_err());

    request.viewport_width = 4_096;
    request.viewport_height = 4_096;
    request.tile_side = 1;
    let error = validate_detail_viewport_request(&request)
        .expect_err("a pathological grid must fail before source lookup or decode");
    assert!(error.to_string().contains("pre-decode admission"));
}

#[test]
fn newer_detail_render_tokens_cancel_older_tile_work() {
    let (root, session, _, _) = test_edit_session();
    let first = session.begin_basic_edit_detail();
    session
        .ensure_current_edit_detail_render(first)
        .expect("fresh token is current");
    let second = session.begin_basic_edit_detail();
    assert!(session.ensure_current_edit_detail_render(first).is_err());
    session
        .ensure_current_edit_detail_render(second)
        .expect("new token supersedes the old token");

    drop(session);
    std::fs::remove_dir_all(root).expect("remove detail-token fixture");
}
