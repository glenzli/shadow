use super::parse_helper_dimensions;

#[test]
fn parses_the_tiny_helper_metadata_protocol() {
    let dimensions =
        parse_helper_dimensions(b"shadow-proxy-v1 320 240 8 3\n").expect("parse helper dimensions");
    assert_eq!(dimensions.width, 320);
    assert_eq!(dimensions.height, 240);
}

#[test]
fn rejects_a_protocol_with_unexpected_pixel_layout() {
    assert!(parse_helper_dimensions(b"shadow-proxy-v1 320 240 16 4\n").is_err());
}
