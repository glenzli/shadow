use super::*;
use crate::recipe::test_support::exposure_graph;
use crate::{EntityId, LayerId, LayerInstanceId, LayerRevisionId, ShootId};

#[test]
fn only_photo_scoped_layers_can_inline_mutable_content() {
    let error = LayerInstance::new(
        LayerInstanceId::new_v7(),
        "Invalid shared inline layer",
        AdjustmentScope::Shoot(ShootId::new_v7()),
        LayerContent::Inline {
            graph: exposure_graph(),
        },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .expect_err("broader scope needs a shared revision");

    assert!(matches!(
        error,
        RecipeValidationError::InlineLayerMustBePhotoScoped { .. }
    ));
}

#[test]
fn layer_revisions_are_immutable_parented_records() {
    let layer_id = LayerId::new_v7();
    let first_id = LayerRevisionId::new_v7();
    let first = LayerRevision::new(
        first_id,
        layer_id,
        1,
        None,
        "Warm Editorial r1",
        exposure_graph(),
    )
    .expect("valid first revision");
    let second = LayerRevision::new(
        LayerRevisionId::new_v7(),
        layer_id,
        2,
        Some(first_id),
        "Warm Editorial r2",
        exposure_graph(),
    )
    .expect("valid child revision");

    assert_eq!(first.revision_number(), 1);
    assert_eq!(second.parent(), Some(first.id()));
    let encoded = serde_json::to_string(&second).expect("serialize layer revision");
    let decoded: LayerRevision =
        serde_json::from_str(&encoded).expect("deserialize layer revision");
    decoded.validate().expect("valid deserialized revision");
    assert_eq!(second, decoded);
    assert!(
        LayerRevision::new(
            LayerRevisionId::new_v7(),
            layer_id,
            3,
            None,
            "Broken r3",
            exposure_graph(),
        )
        .is_err()
    );
}
