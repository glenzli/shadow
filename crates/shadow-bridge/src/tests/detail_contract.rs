//! Full-resolution detail-session bounds and tile-request contracts.

use super::*;

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
    assert_eq!(MAX_EDIT_DETAIL_RETAINED_BYTES, 512 * 1_024 * 1_024);

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
