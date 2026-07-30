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

#[test]
fn raw_foundation_is_not_the_ordinary_denoise_capability() {
    assert_eq!(
        AiTaskKind::MaterializeRawFoundation.capability(),
        AiCapability::RawFoundationDenoise
    );
    assert_ne!(
        AiTaskKind::MaterializeRawFoundation.capability(),
        AiTaskKind::Denoise.capability()
    );
}

fn wire_request() -> AiJobRequest {
    AiJobRequest {
        contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
        request_id: "wire-request-1".into(),
        generation: 1,
        task: AiTaskKind::GenerateCaption,
        target: ObservationTarget::Library,
        priority: TaskPriority::CurrentInput,
        privacy: PrivacyClass::Personal,
        inputs: vec![ArtifactReference {
            role: InputRole::DisplayProxy,
            content_hash: "a".repeat(64),
            byte_len: 1024,
            media_type: "image/jpeg".into(),
            privacy: PrivacyClass::Personal,
        }],
        parameters: AiTaskParameters::None,
        estimate: ResourceEstimate::default(),
    }
}

#[test]
fn request_wire_is_exact_recursive_and_requires_typed_parameters() {
    let request = wire_request();
    let value = serde_json::to_value(&request).expect("serialize request");

    let mut unsupported = value.clone();
    unsupported["contract_version"] = serde_json::json!(AI_JOB_REQUEST_CONTRACT_VERSION + 1);
    assert!(serde_json::from_value::<AiJobRequest>(unsupported).is_err());

    let mut missing_parameters = value.clone();
    missing_parameters
        .as_object_mut()
        .expect("request object")
        .remove("parameters");
    assert!(serde_json::from_value::<AiJobRequest>(missing_parameters).is_err());

    let mut unknown_resource_fact = value.clone();
    unknown_resource_fact["estimate"]["invented_budget"] = serde_json::json!(1);
    assert!(serde_json::from_value::<AiJobRequest>(unknown_resource_fact).is_err());

    let mut unknown_artifact_fact = value;
    unknown_artifact_fact["inputs"][0]["local_path"] = serde_json::json!("/private/source.raw");
    assert!(serde_json::from_value::<AiJobRequest>(unknown_artifact_fact).is_err());
}

#[test]
fn request_input_inventory_is_stream_bounded() {
    let request = wire_request();
    let mut value = serde_json::to_value(&request).expect("serialize request");
    value["inputs"] = serde_json::Value::Array(
        (0..=64)
            .map(|index| {
                let mut artifact =
                    serde_json::to_value(&request.inputs[0]).expect("serialize artifact");
                artifact["content_hash"] = serde_json::json!(format!("{index:064x}"));
                artifact
            })
            .collect(),
    );

    assert!(serde_json::from_value::<AiJobRequest>(value).is_err());
}
