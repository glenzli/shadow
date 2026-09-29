use super::*;
use shadow_domain::EntityId;

#[test]
fn cache_reuses_only_exact_source_space_and_geometry() {
    let root = std::env::temp_dir().join(format!(
        "shadow-vector-cache-{}",
        shadow_domain::RepresentationId::new_v7()
    ));
    let cache = EmbeddingCache::new(&root);
    let space = SemanticEmbeddingSpace::new(
        shadow_ai::SEMANTIC_EMBEDDING_CONTRACT_VERSION,
        "test-model:v1",
        2,
    )
    .expect("space");
    let vector = SemanticEmbedding::new(space.clone(), vec![1.0, 0.0]).expect("vector");
    cache.store("source-a", &vector, 20, 10);
    assert_eq!(
        cache
            .load("source-a", &space, 20, 10)
            .expect("cache hit")
            .values(),
        vector.values()
    );
    assert!(cache.load("source-b", &space, 20, 10).is_none());
    assert!(cache.load("source-a", &space, 10, 20).is_none());
    let changed_space = SemanticEmbeddingSpace::new(
        shadow_ai::SEMANTIC_EMBEDDING_CONTRACT_VERSION,
        "test-model:v2",
        2,
    )
    .expect("space");
    assert!(cache.load("source-a", &changed_space, 20, 10).is_none());
    fs::write(cache.path("source-a", &space), b"incomplete record").expect("corrupt cache");
    assert!(cache.load("source-a", &space, 20, 10).is_none());
    cache.store("source-a", &vector, 20, 10);
    assert!(cache.load("source-a", &space, 20, 10).is_some());
    fs::remove_dir_all(root).expect("clean fixture");
}
