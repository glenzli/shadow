use super::*;

#[test]
fn confidence_never_grants_direct_application() {
    assert_eq!(
        ProposalReviewLevel::from_confidence(
            UnitInterval::ONE,
            UnitInterval::new(0.5).expect("valid threshold"),
            UnitInterval::new(0.85).expect("valid threshold"),
        ),
        ProposalReviewLevel::ProposalOnly
    );
}

#[test]
fn every_task_maps_to_a_provider_capability() {
    assert_eq!(
        AiTaskKind::GenerateInpaintPatch.capability(),
        AiCapability::InpaintPatch
    );
}
