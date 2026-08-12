use super::*;

#[test]
fn sdk_description_becomes_bounded_shadow_semantic_evidence() {
    let response = infer_runtime_client::ImageDescriptionResponse {
        id: "qwen-job".into(),
        object: "vision.image_description".into(),
        created_at: 1,
        status: "completed".into(),
        source_revision: "source-v1".into(),
        language: "zh-CN".into(),
        image: infer_runtime_client::ImageGeometry {
            width: 640,
            height: 480,
            orientation: NORMALIZED_DISPLAY_ORIENTATION.into(),
        },
        result: infer_runtime_client::ImageDescriptionResult {
            description: "山间日落".into(),
            keyword_suggestions: vec!["山景".into(), "日落".into()],
        },
        provenance: sdk_provenance(),
    };
    let evidence =
        admit_description(response, "source-v1", "zh-CN").expect("bounded semantic evidence");
    assert_eq!(evidence.analysis.suggestions.len(), 2);
    assert_eq!(evidence.provenance.model_build, "qwen-build");
}

#[test]
fn duplicate_keywords_and_taxonomy_escape_fail_closed() {
    let response = infer_runtime_client::ImageDescriptionResponse {
        id: "qwen-job".into(),
        object: "vision.image_description".into(),
        created_at: 1,
        status: "completed".into(),
        source_revision: "source-v1".into(),
        language: "en".into(),
        image: infer_runtime_client::ImageGeometry {
            width: 32,
            height: 24,
            orientation: NORMALIZED_DISPLAY_ORIENTATION.into(),
        },
        result: infer_runtime_client::ImageDescriptionResult {
            description: "Mountain".into(),
            keyword_suggestions: vec!["Rock".into(), "rock".into()],
        },
        provenance: sdk_provenance(),
    };
    assert!(admit_description(response, "source-v1", "en").is_err());

    let categories = vec![ClassificationReviewCategory {
        id: "portrait".into(),
        name: "Portrait".into(),
        description: None,
    }];
    let response = infer_runtime_client::ClassificationReviewResponse {
        id: "review-job".into(),
        object: "vision.classification_review".into(),
        created_at: 1,
        status: "completed".into(),
        source_revision: "source-v1".into(),
        taxonomy_revision: "taxonomy-v1".into(),
        image: infer_runtime_client::ImageGeometry {
            width: 32,
            height: 24,
            orientation: NORMALIZED_DISPLAY_ORIENTATION.into(),
        },
        suggestion: infer_runtime_client::ClassificationSuggestion {
            disposition: infer_runtime_client::ClassificationDisposition::Matched,
            category_id: Some("escaped".into()),
        },
        provenance: sdk_provenance(),
    };
    assert!(admit_classification(response, "source-v1", "taxonomy-v1", &categories).is_err());
}

#[test]
fn qwen_metadata_is_local_only_offline_and_no_fallback() {
    let metadata = local_metadata(
        "interactive",
        Some(ImageUnderstandingQuality::General.capability_floor()),
    );
    assert_eq!(metadata["infer.placement"], "local_only");
    assert_eq!(metadata["infer.offline_required"], "true");
    assert_eq!(metadata["infer.fallback"], "none");
    assert_eq!(metadata["infer.capability_floor"], "capable");
}

#[test]
fn classification_taxonomy_remains_bounded() {
    let categories = vec![ClassificationReviewCategory {
        id: "portrait".into(),
        name: "Portrait".into(),
        description: Some("A person-centred photograph".into()),
    }];
    validate_taxonomy("taxonomy-v1", &categories).expect("valid taxonomy");
    assert!(validate_taxonomy("", &categories).is_err());
}

fn sdk_provenance() -> infer_runtime_client::ImageUnderstandingProvenance {
    infer_runtime_client::ImageUnderstandingProvenance {
        job_id: "qwen-job".into(),
        provider: "ollama-local".into(),
        deployment: "qwen-local".into(),
        model_profile: "qwen-profile".into(),
        model_build: "qwen-build".into(),
        physical_model: "qwen/model".into(),
        runtime: OLLAMA_NATIVE_RUNTIME.into(),
        schema_revision: "schema-v1".into(),
        prompt_revision: "prompt-v1".into(),
        total_duration_ms: Some(100),
        load_duration_ms: Some(10),
        prompt_eval_count: Some(20),
        eval_count: Some(30),
    }
}
