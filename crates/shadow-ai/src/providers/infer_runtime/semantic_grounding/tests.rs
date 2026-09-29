use super::*;

#[test]
fn normalized_semantic_regions_are_bounded() {
    assert!(valid_normalized_box(0.1, 0.2, 0.5, 0.6));
    assert!(!valid_normalized_box(0.8, 0.2, 0.3, 0.4));
    assert!(!valid_normalized_box(0.1, 0.2, 0.0, 0.4));
}
