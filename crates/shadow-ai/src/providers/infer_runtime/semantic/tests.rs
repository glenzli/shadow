use super::*;

#[test]
fn sdk_embedding_vector_enters_only_the_exact_siglip_space() {
    let mut values = vec![0.0; SIGLIP_EMBEDDING_DIMENSIONS];
    values[0] = 1.0;
    let admitted = admit_embedding(infer_runtime_client::SemanticEmbeddingVector {
        values,
        dimensions: SIGLIP_EMBEDDING_DIMENSIONS,
        normalized: true,
        distance_metric: "cosine".into(),
        space: "siglip2-so400m@build:space:v1".into(),
    })
    .expect("valid SDK embedding");
    assert_eq!(admitted.space().dimensions(), SIGLIP_EMBEDDING_DIMENSIONS);
    assert_eq!(
        admitted.space().contract_version(),
        SEMANTIC_EMBEDDING_CONTRACT_VERSION
    );
}

#[test]
fn wrong_dimension_or_metric_fails_closed() {
    let error = admit_embedding(infer_runtime_client::SemanticEmbeddingVector {
        values: vec![1.0],
        dimensions: 1,
        normalized: true,
        distance_metric: "euclidean".into(),
        space: "wrong".into(),
    })
    .expect_err("wrong SDK vector must fail");
    assert!(matches!(
        error,
        InferRuntimeClientError::MalformedResponse(_)
    ));
}

#[test]
fn bounded_text_revision_and_language_remain_shadow_policy() {
    validate_text_request("mountain", "query-v1", Some("zh-CN")).expect("valid query");
    assert!(validate_text_request("", "query-v1", None).is_err());
    assert!(validate_text_request("mountain", "", None).is_err());
    assert!(validate_text_request("mountain", "query-v1", Some("zh_CN")).is_err());
}
