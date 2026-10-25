use super::parse_descriptor;

#[test]
fn camera_preview_protocol_preserves_identity_dimensions_and_absence() {
    let preview = parse_descriptor(
        b"shadow-embedded-preview-v1 request jpeg 17 640 480 8 3 4096\n",
        "request",
    )
    .unwrap()
    .unwrap();
    assert_eq!(preview.provider_id, 17);
    assert_eq!(preview.dimensions.width, 640);
    assert_eq!(preview.dimensions.height, 480);
    assert_eq!(preview.encoded_bytes, 4096);
    assert!(
        parse_descriptor(b"shadow-embedded-preview-v1 request none\n", "request")
            .unwrap()
            .is_none()
    );
}

#[test]
fn camera_preview_protocol_rejects_stale_unbounded_or_malformed_results() {
    for message in [
        "shadow-embedded-preview-v1 stale none",
        "shadow-embedded-preview-v1 request none extra",
        "shadow-embedded-preview-v1 request jpeg 0 0 480 8 3 4096",
        "shadow-embedded-preview-v1 request jpeg 0 20000 480 8 3 4096",
        "shadow-embedded-preview-v1 request jpeg 0 640 480 16 3 4096",
        "shadow-embedded-preview-v1 request jpeg 0 640 480 8 4 4096",
        "shadow-embedded-preview-v1 request jpeg 0 640 480 8 3 999999999",
    ] {
        assert!(
            parse_descriptor(message.as_bytes(), "request").is_err(),
            "{message}"
        );
    }
}
