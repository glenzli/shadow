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

    assert_eq!(node.strokes(), std::slice::from_ref(&stroke));
    assert_eq!(stroke.points(), &[point(0.25, 0.4), point(0.3, 0.45)]);
    assert_eq!(stroke.radius().get().to_bits(), 0.08_f64.to_bits());
    assert_eq!(stroke.strength().get().to_bits(), 0.75_f64.to_bits());
    assert_eq!(stroke.hardness().get().to_bits(), 0.6_f64.to_bits());
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
