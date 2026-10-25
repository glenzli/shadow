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

#[test]
fn following_is_compatible_policy_on_a_materialized_pinned_instance() {
    let layer = LayerInstance::new(
        LayerInstanceId::new_v7(),
        "Pinned",
        AdjustmentScope::Photo,
        LayerContent::Shared {
            layer_id: LayerId::new_v7(),
            revision: LayerRevisionSelector::Pinned(LayerRevisionId::new_v7()),
            graph: exposure_graph(),
        },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .unwrap();
    let legacy_json = serde_json::to_string(&layer).unwrap();
    assert!(!legacy_json.contains("shared_follow_head"));
    let decoded: LayerInstance = serde_json::from_str(&legacy_json).unwrap();
    assert!(!decoded.shared_follow_head());
    let following = decoded.with_shared_follow_head(true).unwrap();
    let json = serde_json::to_string(&following).unwrap();
    let reloaded: LayerInstance = serde_json::from_str(&json).unwrap();
    assert!(reloaded.shared_follow_head());
    assert!(!reloaded.content().follows_head());
    let commit = crate::RecipeCommit::new(
        crate::RecipeCommitId::new_v7(),
        crate::RecipeId::new_v7(),
        Vec::new(),
        crate::RecipeSnapshot::new(1, vec![reloaded]).unwrap(),
        None,
        100,
    )
    .unwrap();
    assert!(commit.snapshot().layers()[0].shared_follow_head());
}

#[test]
fn independent_inline_content_cannot_claim_a_following_subscription() {
    let inline = crate::recipe::test_support::inline_layer();
    assert!(matches!(
        inline.with_shared_follow_head(true),
        Err(RecipeValidationError::SharedFollowPolicyRequiresPinnedLayer(_))
    ));
}
