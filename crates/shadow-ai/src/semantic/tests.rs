use super::*;

fn space(id: &str, dimensions: usize) -> SemanticEmbeddingSpace {
    SemanticEmbeddingSpace::new(SEMANTIC_EMBEDDING_CONTRACT_VERSION, id, dimensions)
        .expect("valid embedding space")
}

fn suggestion(id: &str) -> SemanticKeywordSuggestion {
    SemanticKeywordSuggestion {
        kind: SemanticKeywordKind::Object,
        concept_id: id.into(),
        display_label: "红色汽车".into(),
        evidence: SemanticEvidenceKind::DirectObservation,
    }
}

fn analysis() -> SemanticImageAnalysis {
    SemanticImageAnalysis::new(
        SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION,
        "qwen3-vl-4b:build-1",
        "shadow.semantic-image:prompt-1",
        vec![suggestion("object:red-car")],
        SemanticShortCaption {
            language_tag: "zh-CN".into(),
            text: "一辆红色汽车停在雪山前。".into(),
        },
    )
    .expect("valid semantic analysis")
}

#[test]
fn image_and_text_vectors_compare_only_inside_one_exact_space() {
    let image = SemanticEmbedding::new(space("siglip2-base-224:build-a", 2), vec![1.0, 0.0])
        .expect("image embedding");
    let query = SemanticEmbedding::new(space("siglip2-base-224:build-a", 2), vec![0.8, 0.6])
        .expect("text embedding");
    let other = SemanticEmbedding::new(space("siglip2-base-224:build-b", 2), vec![1.0, 0.0])
        .expect("other embedding");

    assert_eq!(image.cosine_similarity(&query), Some(0.8));
    assert_eq!(image.cosine_similarity(&other), None);
}

#[test]
fn delivered_siglip_space_identity_is_portable() {
    let delivered = "siglip2_base_patch16_224@75de2d55:fixres224:lowercase64:l2_768_fp32:v1";
    assert!(
        SemanticEmbeddingSpace::new(SEMANTIC_EMBEDDING_CONTRACT_VERSION, delivered, 768).is_ok()
    );
}

#[test]
fn embeddings_are_bounded_normalized_and_redacted() {
    assert_eq!(
        SemanticEmbedding::new(space("siglip2-base-224:build-a", 2), vec![1.0]),
        Err(SemanticContractError::EmbeddingDimensionMismatch {
            expected: 2,
            actual: 1,
        })
    );
    assert_eq!(
        SemanticEmbedding::new(space("siglip2-base-224:build-a", 2), vec![1.0, 1.0]),
        Err(SemanticContractError::EmbeddingNotNormalized)
    );

    let embedding = SemanticEmbedding::new(space("siglip2-base-224:build-a", 2), vec![1.0, 0.0])
        .expect("embedding");
    let debug = format!("{embedding:?}");
    assert!(debug.contains("redacted"));
    assert!(!debug.contains("1.0"));
}

#[test]
fn structured_analysis_round_trips_without_becoming_a_keyword_assignment() {
    let analysis = analysis();
    let json = serde_json::to_string(&analysis).expect("serialize analysis");
    let decoded: SemanticImageAnalysis = serde_json::from_str(&json).expect("decode analysis");

    assert_eq!(decoded, analysis);
    assert!(json.contains("suggestions"));
    assert!(!json.contains("ai_accepted"));
    assert!(!json.contains("confidence"));
}

#[test]
fn structured_analysis_rejects_duplicates_unknown_fields_and_unbounded_output() {
    let duplicate = SemanticImageAnalysis::new(
        SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION,
        "qwen3-vl-4b:build-1",
        "shadow.semantic-image:prompt-1",
        vec![suggestion("object:red-car"), suggestion("object:red-car")],
        analysis().short_caption,
    );
    assert_eq!(
        duplicate,
        Err(SemanticContractError::DuplicateKeywordSuggestion)
    );

    let mut value = serde_json::to_value(analysis()).expect("analysis json");
    value["provider_path"] = serde_json::json!("/private/model.onnx");
    assert!(serde_json::from_value::<SemanticImageAnalysis>(value).is_err());

    let too_many = (0..=MAX_SEMANTIC_KEYWORD_SUGGESTIONS)
        .map(|index| suggestion(&format!("object:item-{index}")))
        .collect();
    assert_eq!(
        SemanticImageAnalysis::new(
            SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION,
            "qwen3-vl-4b:build-1",
            "shadow.semantic-image:prompt-1",
            too_many,
            analysis().short_caption,
        ),
        Err(SemanticContractError::TooManyKeywordSuggestions(
            MAX_SEMANTIC_KEYWORD_SUGGESTIONS + 1,
        ))
    );
}
