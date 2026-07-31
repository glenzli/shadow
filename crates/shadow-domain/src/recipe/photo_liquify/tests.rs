use super::*;

fn point(x: f64, y: f64) -> LiquifyPoint {
    LiquifyPoint::new(
        UnitInterval::new(x).expect("normalized x"),
        UnitInterval::new(y).expect("normalized y"),
    )
}

#[test]
fn push_strokes_preserve_original_space_authoring_parameters() {
    let stroke = LiquifyStroke::push(
        vec![point(0.25, 0.4), point(0.3, 0.45)],
        UnitInterval::new(0.08).expect("radius"),
        UnitInterval::new(0.75).expect("strength"),
        UnitInterval::new(0.6).expect("hardness"),
    )
    .expect("valid push stroke");
    let node = PhotoLiquifyNode::new(vec![stroke.clone()]).expect("valid singleton node");

    assert!(node.enabled());
    assert_eq!(node.strokes(), std::slice::from_ref(&stroke));
    assert_eq!(stroke.points(), &[point(0.25, 0.4), point(0.3, 0.45)]);
    assert_eq!(stroke.radius().get().to_bits(), 0.08_f64.to_bits());
    assert_eq!(stroke.strength().get().to_bits(), 0.75_f64.to_bits());
    assert_eq!(stroke.hardness().get().to_bits(), 0.6_f64.to_bits());
}

#[test]
fn bypass_preserves_authored_liquify_gestures() {
    let stroke = LiquifyStroke::push(
        vec![point(0.25, 0.4), point(0.3, 0.45)],
        UnitInterval::new(0.08).expect("radius"),
        UnitInterval::new(0.75).expect("strength"),
        UnitInterval::new(0.6).expect("hardness"),
    )
    .expect("valid push stroke");
    let bypassed = PhotoLiquifyNode::new(vec![stroke.clone()])
        .expect("valid singleton node")
        .with_enabled(false);

    assert!(!bypassed.enabled());
    assert_eq!(bypassed.strokes(), std::slice::from_ref(&stroke));

    let encoded = serde_json::to_value(&bypassed).expect("serialize bypassed Liquify");
    let decoded: PhotoLiquifyNode =
        serde_json::from_value(encoded).expect("deserialize bypassed Liquify");
    assert_eq!(decoded, bypassed);
}

#[test]
fn legacy_liquify_without_enabled_field_defaults_to_active() {
    let decoded: PhotoLiquifyNode = serde_json::from_value(serde_json::json!({
        "strokes": [{
            "kind": "push",
            "points": [
                {"x": 0.25, "y": 0.4, "pressure": 1.0},
                {"x": 0.3, "y": 0.45, "pressure": 1.0}
            ],
            "radius": 0.08,
            "strength": 0.75,
            "hardness": 0.6
        }]
    }))
    .expect("deserialize legacy Liquify");

    assert!(decoded.enabled());
}

#[test]
fn reconstruct_is_an_ordered_single_stamp_capable_operation() {
    let push = LiquifyStroke::push(
        vec![point(0.25, 0.4), point(0.3, 0.45)],
        UnitInterval::new(0.08).expect("radius"),
        UnitInterval::new(0.75).expect("strength"),
        UnitInterval::new(0.6).expect("hardness"),
    )
    .expect("valid push stroke");
    let reconstruct = LiquifyStroke::reconstruct(
        vec![point(0.3, 0.45)],
        UnitInterval::new(0.1).expect("radius"),
        UnitInterval::new(0.5).expect("strength"),
        UnitInterval::new(0.4).expect("hardness"),
    )
    .expect("valid reconstruction stamp");
    let node = PhotoLiquifyNode::new(vec![push, reconstruct.clone()])
        .expect("reconstruct follows deformation");

    assert!(node.strokes()[1].is_reconstruct());
    let encoded = serde_json::to_value(&node).expect("serialize Reconstruct");
    assert_eq!(encoded["strokes"][1]["kind"], "reconstruct");
    assert_eq!(
        PhotoLiquifyNode::new(vec![reconstruct]),
        Err(RecipeValidationError::LiquifyReconstructWithoutPriorDeformation)
    );
}

#[test]
fn liquify_rejects_empty_nodes_and_non_effective_push_gestures() {
    assert_eq!(
        PhotoLiquifyNode::new(Vec::new()),
        Err(RecipeValidationError::EmptyLiquifyNode)
    );
    assert_eq!(
        LiquifyStroke::push(
            vec![point(0.25, 0.4)],
            UnitInterval::new(0.08).expect("radius"),
            UnitInterval::ONE,
            UnitInterval::new(0.6).expect("hardness"),
        ),
        Err(RecipeValidationError::TooFewLiquifyStrokePoints(1))
    );
    assert_eq!(
        LiquifyStroke::push(
            vec![point(0.25, 0.4), point(0.25, 0.4)],
            UnitInterval::new(0.08).expect("radius"),
            UnitInterval::ONE,
            UnitInterval::new(0.6).expect("hardness"),
        ),
        Err(RecipeValidationError::DegenerateLiquifyStroke)
    );
    let pressureless = |x, y| {
        LiquifyPoint::with_pressure(
            UnitInterval::new(x).expect("normalized x"),
            UnitInterval::new(y).expect("normalized y"),
            UnitInterval::ZERO,
        )
    };
    assert_eq!(
        LiquifyStroke::push(
            vec![pressureless(0.25, 0.4), pressureless(0.3, 0.45)],
            UnitInterval::new(0.08).expect("radius"),
            UnitInterval::ONE,
            UnitInterval::new(0.6).expect("hardness"),
        ),
        Err(RecipeValidationError::DegenerateLiquifyStroke)
    );
    assert_eq!(
        LiquifyStroke::push(
            vec![point(0.25, 0.4), point(0.3, 0.45)],
            UnitInterval::ZERO,
            UnitInterval::ONE,
            UnitInterval::new(0.6).expect("hardness"),
        ),
        Err(RecipeValidationError::DegenerateLiquifyBrushRadius)
    );
    assert_eq!(
        LiquifyStroke::push(
            vec![point(0.25, 0.4), point(0.3, 0.45)],
            UnitInterval::new(0.08).expect("radius"),
            UnitInterval::ZERO,
            UnitInterval::new(0.6).expect("hardness"),
        ),
        Err(RecipeValidationError::DegenerateLiquifyStrength)
    );
}
