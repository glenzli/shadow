use super::*;

#[test]
fn serde_rejects_invalid_confidence() {
    let result = serde_json::from_str::<UnitInterval>("1.2");
    assert!(result.is_err());
}

#[test]
fn weighted_average_ignores_zero_weight() {
    let result = UnitInterval::weighted_average(&[
        (UnitInterval::ONE, 0.0),
        (UnitInterval::new(0.25).expect("valid score"), 2.0),
    ]);
    assert_eq!(result, UnitInterval::new(0.25).expect("valid score"));
}
