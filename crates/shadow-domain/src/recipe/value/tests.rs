use super::*;

#[test]
fn finite_numbers_and_names_reject_non_portable_values_during_deserialization() {
    assert!(FiniteF64::new(f64::NAN).is_err());
    assert!(UnitInterval::new(1.01).is_err());
    assert!(ParameterKey::new("tone exposure").is_err());
    assert!(BranchName::new(" trailing ").is_err());
    assert!(serde_json::from_str::<UnitInterval>("2.0").is_err());
}
