use super::*;

fn provenance() -> ImageUnderstandingProvenance {
    ImageUnderstandingProvenance {
        job_id: "vision-job".into(),
        provider: "ollama-local".into(),
        deployment: "qwen-vl-basic".into(),
        model_profile: "qwen-vl-4b".into(),
        model_build: "qwen-vl-4b-build-1".into(),
        physical_model: "qwen3-vl:4b".into(),
        runtime: OLLAMA_NATIVE_RUNTIME.into(),
        schema_revision: "image-description-v1".into(),
        prompt_revision: "shadow-description-v1".into(),
        total_duration_ms: Some(1_000),
        load_duration_ms: Some(100),
        prompt_eval_count: Some(120),
        eval_count: Some(40),
    }
}

fn geometry() -> RawImageGeometry {
    RawImageGeometry {
        width: 640,
        height: 480,
        orientation: NORMALIZED_DISPLAY_ORIENTATION.into(),
    }
}

fn categories() -> Vec<ClassificationReviewCategory> {
    vec![
        ClassificationReviewCategory {
            id: "portrait".into(),
            name: "人像".into(),
            description: Some("以人物为主要视觉对象".into()),
        },
        ClassificationReviewCategory {
            id: "landscape".into(),
            name: "风光".into(),
            description: None,
        },
    ]
}

#[test]
fn description_admits_open_ended_keywords_and_exact_qwen_provenance() {
    let response = RawImageDescriptionResponse {
        object: "vision.image_description".into(),
        status: "completed".into(),
        source_revision: "shadow:photo/artifact:abc".into(),
        language: "zh-CN".into(),
        image: geometry(),
        result: RawImageDescriptionResult {
            description: "一只鸟停在松枝上。".into(),
            keyword_suggestions: vec!["鸟".into(), "松树".into()],
        },
        provenance: provenance(),
    }
    .validate("shadow:photo/artifact:abc", "zh-CN")
    .expect("valid description");

    assert_eq!(response.analysis.suggestions.len(), 2);
    assert!(
        response
            .analysis
            .suggestions
            .iter()
            .all(|suggestion| suggestion.kind == SemanticKeywordKind::Concept)
    );
    assert_eq!(response.analysis.short_caption.language_tag, "zh-CN");
    assert_eq!(response.provenance.runtime, OLLAMA_NATIVE_RUNTIME);
}

#[test]
fn description_rejects_case_folded_duplicates_and_wrong_runtime() {
    let duplicate = RawImageDescriptionResponse {
        object: "vision.image_description".into(),
        status: "completed".into(),
        source_revision: "source".into(),
        language: "en".into(),
        image: geometry(),
        result: RawImageDescriptionResult {
            description: "A bird.".into(),
            keyword_suggestions: vec!["Bird".into(), "bird".into()],
        },
        provenance: provenance(),
    };
    assert!(duplicate.validate("source", "en").is_err());

    let mut wrong_provenance = provenance();
    wrong_provenance.runtime = "onnxruntime".into();
    assert!(!valid_image_understanding_provenance(&wrong_provenance));
}

#[test]
fn classification_review_cannot_escape_the_submitted_closed_set() {
    let response = RawClassificationReviewResponse {
        object: "vision.classification_review".into(),
        status: "completed".into(),
        source_revision: "source".into(),
        taxonomy_revision: "shadow:categories:1".into(),
        image: geometry(),
        suggestion: RawClassificationReviewSuggestion {
            disposition: "matched".into(),
            category_id: Some("portrait".into()),
        },
        provenance: provenance(),
    }
    .validate("source", "shadow:categories:1", &categories())
    .expect("closed-set match");
    assert_eq!(
        response.suggestion.disposition,
        ClassificationReviewDisposition::Matched
    );

    let escaped = RawClassificationReviewResponse {
        object: "vision.classification_review".into(),
        status: "completed".into(),
        source_revision: "source".into(),
        taxonomy_revision: "shadow:categories:1".into(),
        image: geometry(),
        suggestion: RawClassificationReviewSuggestion {
            disposition: "matched".into(),
            category_id: Some("invented".into()),
        },
        provenance: provenance(),
    };
    assert!(
        escaped
            .validate("source", "shadow:categories:1", &categories())
            .is_err()
    );
}

#[test]
fn classification_request_is_bounded_and_has_unique_ids() {
    assert!(validate_taxonomy("shadow:categories:1", &categories()).is_ok());
    assert!(validate_taxonomy("", &categories()).is_err());
    let mut duplicate = categories();
    duplicate[1].id = duplicate[0].id.clone();
    assert!(validate_taxonomy("shadow:categories:1", &duplicate).is_err());
    assert!(validate_taxonomy("shadow:categories:1", &[]).is_err());
}

#[test]
fn additive_response_fields_are_ignored() {
    let value = serde_json::json!({
        "object": "vision.image_description",
        "status": "completed",
        "source_revision": "source",
        "language": "zh-CN",
        "image": {
            "width": 10,
            "height": 20,
            "orientation": NORMALIZED_DISPLAY_ORIENTATION,
            "future": true
        },
        "result": {
            "description": "测试照片。",
            "keyword_suggestions": ["测试"],
            "future": "ignored"
        },
        "provenance": {
            "job_id": "job",
            "provider": "ollama-local",
            "deployment": "qwen",
            "model_profile": "basic",
            "model_build": "build",
            "physical_model": "qwen3-vl:4b",
            "runtime": OLLAMA_NATIVE_RUNTIME,
            "schema_revision": "schema-v1",
            "prompt_revision": "prompt-v1",
            "future": "ignored"
        },
        "future": "ignored"
    });
    let response: RawImageDescriptionResponse =
        serde_json::from_value(value).expect("decode additive response");
    assert!(response.validate("source", "zh-CN").is_ok());
}

#[test]
#[ignore = "set SHADOW_TEST_SEMANTIC_IMAGE and SHADOW_INFER_TOKEN_FILE to run real local Qwen HTTP inference"]
fn real_qwen_description_and_closed_set_review_use_typed_contracts() {
    let image_path =
        std::env::var("SHADOW_TEST_SEMANTIC_IMAGE").expect("image-understanding image path");
    let token_file = std::env::var("SHADOW_INFER_TOKEN_FILE").expect("token file path");
    let image = std::fs::read(&image_path).expect("read image-understanding image");
    let media_type = if image_path.to_ascii_lowercase().ends_with(".png") {
        "image/png"
    } else {
        "image/jpeg"
    };
    let source_revision = format!("shadow:test:artifact:{}", blake3::hash(&image).to_hex());
    let client = InferRuntimeClient::from_credential_file(
        "http://127.0.0.1:8787",
        std::path::Path::new(&token_file),
    )
    .expect("configure infer-runtime client");

    let description = client
        .describe_image(
            &image,
            media_type,
            &source_revision,
            "zh-CN",
            ImageUnderstandingQuality::Basic,
            SemanticRequestPriority::Background,
        )
        .expect("describe image");
    assert_eq!(description.source_revision, source_revision);
    assert_eq!(description.orientation, NORMALIZED_DISPLAY_ORIENTATION);
    assert!(!description.analysis.short_caption.text.is_empty());

    let taxonomy_revision = "shadow:test-categories:v1";
    let review_categories = categories();
    let review = client
        .review_classification(&ClassificationReviewRequest {
            image: &image,
            media_type,
            source_revision: &source_revision,
            taxonomy_revision,
            categories: &review_categories,
            quality: ImageUnderstandingQuality::General,
            priority: SemanticRequestPriority::Interactive,
        })
        .expect("review classification");
    assert_eq!(review.source_revision, source_revision);
    assert_eq!(review.taxonomy_revision, taxonomy_revision);
    assert_eq!(review.orientation, NORMALIZED_DISPLAY_ORIENTATION);
}
