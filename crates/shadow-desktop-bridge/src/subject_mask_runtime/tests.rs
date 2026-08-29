use super::*;

#[test]
fn input_validation_keeps_provider_payload_bounded() {
    assert!(matches!(
        validate_input_jpeg(&[]),
        Err(SubjectMaskRuntimeError::InputSize(0))
    ));
    validate_input_jpeg(b"bounded JPEG bytes").expect("bounded input");
}
