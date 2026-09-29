use super::*;

#[test]
fn input_validation_keeps_provider_payload_bounded() {
    assert!(matches!(
        validate_input_jpeg(&[]),
        Err(SubjectMaskRuntimeError::InputSize(0))
    ));
    validate_input_jpeg(b"bounded JPEG bytes").expect("bounded input");
}

#[test]
fn semantic_stage_maps_not_found_without_string_matching() {
    assert!(matches!(
        SubjectMaskRuntime::classify_semantic_stage(Err(
            SubjectMaskRuntimeError::SemanticQueryNotFound
        )),
        SemanticMaskStageOutcome::NotFound
    ));
}

#[test]
fn semantic_stage_keeps_provider_unavailability_typed() {
    assert!(matches!(
        SubjectMaskRuntime::classify_semantic_stage(Err(SubjectMaskRuntimeError::InvalidPhotoId)),
        SemanticMaskStageOutcome::Unavailable(SubjectMaskRuntimeError::InvalidPhotoId)
    ));
}

#[test]
fn semantic_stage_keeps_provider_terminal_unavailability_typed() {
    let outcome = SubjectMaskRuntime::classify_semantic_stage(Ok(DerivedRasterStageReceipt {
        request_id: "semantic-unavailable".into(),
        generation: 7,
        usage: RuntimeUsage::default(),
        outcome: shadow_core::DerivedRasterStageOutcome::Unavailable {
            reason: shadow_ai::ProviderUnavailable::ModelNotInstalled,
        },
    }));
    assert!(matches!(
        outcome,
        SemanticMaskStageOutcome::ProviderUnavailable(_)
    ));
}

#[test]
fn semantic_stage_keeps_cancellation_distinct_from_failure() {
    let outcome = SubjectMaskRuntime::classify_semantic_stage(Ok(DerivedRasterStageReceipt {
        request_id: "semantic-cancelled".into(),
        generation: 7,
        usage: RuntimeUsage::default(),
        outcome: shadow_core::DerivedRasterStageOutcome::Cancelled,
    }));
    assert!(matches!(outcome, SemanticMaskStageOutcome::Cancelled));
}
