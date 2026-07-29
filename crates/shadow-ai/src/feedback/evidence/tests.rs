use shadow_domain::EntityId;

use super::*;
use crate::feedback::test_support::event;
use crate::{
    AI_JOB_REQUEST_CONTRACT_VERSION, AdmittedModelIdentity, AiJobRequest, AiTaskKind,
    AiTaskParameters, ArtifactReference, BackendKind, EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
    ExecutionPlanIdentity, ExecutionRouteIdentity, InputRole, ModelProvenance, NumericPrecision,
    ObservationTarget, PrivacyClass, ProviderExecutionClass, ProviderIdentity, ResourceEstimate,
    RunPlan, TaskPriority,
};

fn presented_visual(representation_id: RepresentationId) -> PresentedVisualProvenance {
    PresentedVisualProvenance {
        artifact: PresentedVisualArtifact {
            representation_id,
            source_byte_len: 48_000_000,
            source_modified_at_ms: Some(1_700_000_000_000),
            role: PresentedVisualRole::EmbeddedPreview,
            variant_key: "embedded-0".into(),
            generator_id: "libraw-preview-extractor".into(),
            generator_version: "0.21.4".into(),
            provider_preview_id: Some(0),
            blob_algorithm: "blake3".into(),
            blob_digest_hex: "0".repeat(64),
            blob_byte_len: 2_000_000,
            codec: "jpeg".into(),
            byte_order: "not_applicable".into(),
            width: 4_096,
            height: 2_731,
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 1_700_000_000_100,
        },
        frame: PresentedVisualFrame {
            surface_id: "review-compare-left".into(),
            surface_revision: 1,
            fit_mode: PresentedFitMode::PreserveAspectFit,
            decoder_id: "qt-image-jpeg".into(),
            decoder_version: "6.8.3".into(),
            auto_transform: true,
            requested_width: 1_280,
            requested_height: 960,
            decoded_width: 4_096,
            decoded_height: 2_731,
            pixel_format: "rgba8888-premultiplied".into(),
            pixel_hash_algorithm: "blake3".into(),
            pixel_hash_hex: "a".repeat(64),
        },
    }
}

fn model_provenance() -> ModelProvenance {
    let request = AiJobRequest {
        contract_version: AI_JOB_REQUEST_CONTRACT_VERSION,
        request_id: "preference-request-1".into(),
        generation: 3,
        task: AiTaskKind::ExtractSimilarityEmbedding,
        target: ObservationTarget::Library,
        priority: TaskPriority::CurrentCollectionAnalysis,
        privacy: PrivacyClass::Personal,
        inputs: vec![ArtifactReference {
            role: InputRole::DisplayProxy,
            content_hash: "c".repeat(64),
            byte_len: 4096,
            media_type: "image/jpeg".into(),
            privacy: PrivacyClass::Personal,
        }],
        parameters: AiTaskParameters::None,
        estimate: ResourceEstimate {
            peak_system_ram_bytes: 64,
            peak_device_memory_bytes: 0,
            cpu_threads: 1,
            scratch_disk_bytes: 0,
            upload_bytes: 0,
            estimated_duration_ms: Some(10),
        },
    };
    let route = ExecutionRouteIdentity {
        contract_version: EXECUTION_ROUTE_IDENTITY_CONTRACT_VERSION,
        provider: ProviderIdentity {
            provider_id: "shadow.preference".into(),
            adapter_revision: "linear-head-v1".into(),
            execution_class: ProviderExecutionClass::LocalModel,
        },
        model: AdmittedModelIdentity::LocalArtifactSet {
            model_id: "preference".into(),
            exact_revision: "r1".into(),
            artifact_set_blake3: "b".repeat(64),
            preprocessing_version: "features-v1".into(),
        },
    };
    let plan = ExecutionPlanIdentity::from_plan(RunPlan {
        backend_id: "cpu".into(),
        backend_kind: BackendKind::Cpu,
        precision: NumericPrecision::Float32,
        cpu_threads: 1,
        reserved_system_ram_bytes: 64,
        reserved_device_memory_bytes: 0,
    })
    .expect("valid execution plan");
    ModelProvenance::from_test_request(&request, route, plan).expect("valid runtime provenance")
}

fn assert_tampered_model_is_rejected(value: serde_json::Value) {
    let Ok(provenance) = serde_json::from_value::<ModelProvenance>(value) else {
        // The exact-v1 wire decoder may reject the tampering before feedback
        // validation sees it, which is the stronger fail-closed outcome.
        return;
    };
    let photo_id = PhotoId::new_v7();
    let mut evidence = event(FeedbackAction::Exported { photo_id });
    evidence.presentation.active_model = Some(provenance);
    assert_eq!(
        evidence.validate(),
        Err(FeedbackValidationError::InvalidModelRoute)
    );
}

#[test]
fn legacy_candidate_json_round_trips_byte_for_byte_without_visual_field() {
    let legacy = r#"{"photo_id":"018f0000-0000-7000-8000-000000000000","position":7,"visible_fraction":1.0,"inspected_at_one_to_one":false,"feature":null}"#;
    let candidate: PresentedCandidate =
        serde_json::from_str(legacy).expect("deserialize legacy candidate");

    assert!(candidate.visual.is_none());
    assert_eq!(
        serde_json::to_string(&candidate).expect("serialize legacy candidate"),
        legacy
    );
}

#[test]
fn presented_visual_round_trips_with_complete_artifact_and_frame_identity() {
    let photo_id = PhotoId::new_v7();
    let mut evidence = event(FeedbackAction::Exported { photo_id });
    evidence.presentation.candidates.push(PresentedCandidate {
        photo_id,
        position: 0,
        visible_fraction: UnitInterval::ONE,
        inspected_at_one_to_one: false,
        feature: None,
        visual: Some(presented_visual(RepresentationId::new_v7())),
    });
    evidence.validate().expect("valid visual provenance");

    let json = serde_json::to_string(&evidence).expect("serialize visual evidence");
    let decoded: FeedbackEvent = serde_json::from_str(&json).expect("deserialize visual evidence");
    assert_eq!(decoded, evidence);
    assert_eq!(
        serde_json::to_string(&decoded).expect("reserialize visual evidence"),
        json
    );
}

#[test]
fn presented_visual_rejects_tampered_or_incomplete_identity() {
    let photo_id = PhotoId::new_v7();
    let mut evidence = event(FeedbackAction::Exported { photo_id });
    evidence.presentation.candidates.push(PresentedCandidate {
        photo_id,
        position: 0,
        visible_fraction: UnitInterval::ONE,
        inspected_at_one_to_one: false,
        feature: None,
        visual: Some(presented_visual(RepresentationId::new_v7())),
    });

    let visual = evidence.presentation.candidates[0]
        .visual
        .as_mut()
        .expect("visual");
    visual.artifact.blob_digest_hex.replace_range(0..1, "A");
    assert!(matches!(
        evidence.validate(),
        Err(FeedbackValidationError::InvalidVisualDigest {
            field: "visual blob digest"
        })
    ));

    let visual = evidence.presentation.candidates[0]
        .visual
        .as_mut()
        .expect("visual");
    visual.artifact.blob_digest_hex = "0".repeat(64);
    visual.frame.decoded_width = 0;
    assert!(matches!(
        evidence.validate(),
        Err(FeedbackValidationError::ZeroVisualValue {
            field: "visual decoded width"
        })
    ));

    let visual = evidence.presentation.candidates[0]
        .visual
        .as_mut()
        .expect("visual");
    visual.frame.decoded_width = 4_096;
    visual.frame.decoder_id.clear();
    assert!(matches!(
        evidence.validate(),
        Err(FeedbackValidationError::EmptyString {
            field: "visual decoder id"
        })
    ));

    let visual = evidence.presentation.candidates[0]
        .visual
        .as_mut()
        .expect("visual");
    visual.frame.decoder_id = "qt-image-jpeg".into();
    visual.artifact.generator_version = "x".repeat(MAX_IDENTIFIER_LENGTH + 1);
    assert!(matches!(
        evidence.validate(),
        Err(FeedbackValidationError::StringTooLong {
            field: "visual generator version",
            maximum: MAX_IDENTIFIER_LENGTH
        })
    ));
}

#[test]
fn active_model_accepts_runtime_issued_provenance() {
    let photo_id = PhotoId::new_v7();
    let mut evidence = event(FeedbackAction::Exported { photo_id });
    evidence.presentation.active_model = Some(model_provenance());
    evidence.validate().expect("runtime provenance");
}

#[test]
fn active_model_rejects_a_tampered_execution_route() {
    let mut value = serde_json::to_value(model_provenance()).expect("serialize provenance");
    value["execution_route"]["provider"]["adapter_revision"] = serde_json::json!("");
    assert_tampered_model_is_rejected(value);
}

#[test]
fn active_model_rejects_a_tampered_input_digest() {
    let mut value = serde_json::to_value(model_provenance()).expect("serialize provenance");
    value["input_source_blake3"] = serde_json::json!("C".repeat(64));
    assert_tampered_model_is_rejected(value);
}
