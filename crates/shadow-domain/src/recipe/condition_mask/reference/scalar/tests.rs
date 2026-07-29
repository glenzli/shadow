use super::*;
use crate::recipe::{ConditionMaskNode, RecipeValidationError};

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("test unit interval")
}

fn sample(lightness: f64, a: f64, b: f64, local_detail: f64) -> ConditionMaskScalarSample {
    ConditionMaskScalarSample::new(lightness, a, b, unit(local_detail))
        .expect("finite scalar sample")
}

fn assert_close(actual: f64, expected: f64) {
    assert!(
        (actual - expected).abs() <= 1.0e-12,
        "actual {actual:.17} != expected {expected:.17}"
    );
}

#[test]
fn range_vectors_pin_quintic_smootherstep_and_inclusive_hard_edges() {
    let feathered =
        ConditionMaskPredicate::oklab_lightness_range(unit(0.25), unit(0.75), unit(0.1));
    for (lightness, expected) in [
        (0.15, 0.0),
        (0.20, 0.5),
        (0.25, 1.0),
        (0.75, 1.0),
        (0.80, 0.5),
        (0.85, 0.0),
    ] {
        assert_close(
            feathered.reference_coverage(sample(lightness, 0.0, 0.0, 0.0)),
            expected,
        );
    }

    let hard =
        ConditionMaskPredicate::oklab_lightness_range(unit(0.25), unit(0.75), UnitInterval::ZERO);
    assert_close(hard.reference_coverage(sample(0.25, 0.0, 0.0, 0.0)), 1.0);
    assert_close(hard.reference_coverage(sample(0.75, 0.0, 0.0, 0.0)), 1.0);

    let chroma = ConditionMaskPredicate::oklch_chroma_range(unit(0.25), unit(0.75), unit(0.1));
    assert_close(chroma.reference_coverage(sample(0.5, 0.08, 0.0, 0.0)), 0.5);
}

#[test]
fn expression_vectors_pin_min_max_and_complement_operators() {
    let lower = ConditionMaskNode::leaf(ConditionMaskPredicate::oklab_lightness_range(
        unit(0.25),
        unit(0.75),
        unit(0.1),
    ));
    let upper = ConditionMaskNode::leaf(ConditionMaskPredicate::oklab_lightness_range(
        unit(0.35),
        unit(0.85),
        unit(0.1),
    ));
    let scalar = sample(0.30, 0.0, 0.0, 0.0);
    let all =
        ConditionMaskExpression::all(vec![lower.clone(), upper.clone()]).expect("intersection");
    let any = ConditionMaskExpression::any(vec![lower, upper.clone()]).expect("union");
    let not = ConditionMaskExpression::not(upper).expect("complement");

    assert_close(all.reference_coverage(scalar), 0.5);
    assert_close(any.reference_coverage(scalar), 1.0);
    assert_close(not.reference_coverage(scalar), 0.5);
}

#[test]
fn hue_vectors_preserve_legacy_relative_chroma_confidence_and_feather() {
    let hue = ConditionMaskPredicate::oklch_hue_range(0.0, 40.0, UnitInterval::ZERO, unit(0.25))
        .expect("legacy-compatible hue");

    // L=0.5 and C=0.0055 yield relative chroma 0.011: the exact midpoint
    // between the legacy 0.002 and 0.02 confidence bounds.
    assert_close(hue.reference_coverage(sample(0.5, 0.0055, 0.0, 0.0)), 0.5);
    assert_close(hue.reference_coverage(sample(0.5, 0.001, 0.0, 0.0)), 0.0);

    // Half-width 40° with softness 0.25 has a 30° fully-selected edge and a
    // 40° zero edge. A saturated 35° sample therefore receives 0.5.
    let angle = 35.0_f64.to_radians();
    assert_close(
        hue.reference_coverage(sample(0.5, 0.1 * angle.cos(), 0.1 * angle.sin(), 0.0)),
        0.5,
    );
}

#[test]
fn absolute_chroma_gate_has_an_independent_cubic_feather() {
    let hue = ConditionMaskPredicate::oklch_hue_range_with_chroma_feather(
        0.0,
        40.0,
        unit(0.25),
        unit(0.1),
        unit(0.25),
    )
    .expect("chroma-gated hue");
    // C=0.08 normalizes to 0.2, the midpoint between the gate's 0.15
    // feather start and 0.25 threshold. Relative confidence and hue weight are 1.
    assert_close(hue.reference_coverage(sample(0.5, 0.08, 0.0, 0.0)), 0.5);

    assert_eq!(
        ConditionMaskPredicate::oklch_hue_range_with_chroma_feather(
            0.0,
            40.0,
            UnitInterval::ZERO,
            unit(0.1),
            unit(0.25),
        ),
        Err(RecipeValidationError::UnusedConditionMaskChromaFeather)
    );
}

#[test]
fn local_detail_response_uses_the_same_quintic_range_contract() {
    let detail_predicate =
        ConditionMaskPredicate::local_detail_range(3, unit(0.4), unit(0.8), unit(0.1))
            .expect("detail predicate");
    assert_close(
        detail_predicate.reference_coverage(sample(0.5, 0.0, 0.0, 0.35)),
        0.5,
    );
}
