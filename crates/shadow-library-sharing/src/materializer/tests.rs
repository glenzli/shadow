use super::safe_extension;

#[test]
fn materialized_original_retains_only_a_decoder_safe_extension() {
    assert_eq!(safe_extension("photo.NEF"), "nef");
    assert_eq!(safe_extension("photo.../../bad"), "raw");
    assert_eq!(safe_extension("photo.extension-is-far-too-long"), "raw");
    assert_eq!(safe_extension("photo.💥"), "raw");
}
