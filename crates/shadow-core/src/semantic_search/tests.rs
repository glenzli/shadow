use std::{
    cell::{Cell, RefCell},
    path::PathBuf,
};

use shadow_ai::{
    ImageEmbeddingEvidence, SemanticEmbedding, SemanticEmbeddingSpace, TextEmbeddingEvidence,
    VisionProvenance, VisionTokenizerProvenance,
};
use shadow_cache::ContentAddressedStore;
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogActor, RecordCachedArtifact,
    RecordCachedArtifactStatus, RegisterAsset, RepresentationFingerprint,
};
use shadow_domain::{
    AssetLocation, EntityId, ImageDimensions, Platform, PreviewByteOrder, RepresentationKind,
};

use super::*;

const SPACE: &str = "siglip-test@build:space:v1";

struct FakeSemanticProvider {
    image_calls: Cell<usize>,
    catalog: Option<CatalogHandle>,
    replacement: RefCell<Option<RecordCachedArtifact>>,
}

impl FakeSemanticProvider {
    fn new() -> Self {
        Self {
            image_calls: Cell::new(0),
            catalog: None,
            replacement: RefCell::new(None),
        }
    }

    fn replacing(catalog: CatalogHandle, replacement: RecordCachedArtifact) -> Self {
        Self {
            image_calls: Cell::new(0),
            catalog: Some(catalog),
            replacement: RefCell::new(Some(replacement)),
        }
    }
}

impl SemanticEmbeddingProvider for FakeSemanticProvider {
    fn embed_image_semantics(
        &self,
        image: &[u8],
        _media_type: &str,
        source_revision: &str,
        _priority: SemanticRequestPriority,
    ) -> Result<ImageEmbeddingEvidence, InferRuntimeClientError> {
        self.image_calls.set(self.image_calls.get() + 1);
        if let Some(replacement) = self.replacement.borrow_mut().take() {
            let status = self
                .catalog
                .as_ref()
                .expect("replacement catalog")
                .record_cached_artifact(&replacement)
                .expect("replace current artifact");
            assert_eq!(status, RecordCachedArtifactStatus::Recorded);
        }
        let values = if image == b"first-jpeg" {
            vec![1.0, 0.0]
        } else {
            vec![0.0, 1.0]
        };
        Ok(ImageEmbeddingEvidence {
            source_revision: source_revision.into(),
            width: 2,
            height: 2,
            orientation: "display_pixels_orientation_normalized".into(),
            embedding: embedding(values),
            provenance: provenance(false),
        })
    }

    fn embed_text_semantics(
        &self,
        _text: &str,
        query_revision: &str,
        language: Option<&str>,
        _priority: SemanticRequestPriority,
    ) -> Result<TextEmbeddingEvidence, InferRuntimeClientError> {
        Ok(TextEmbeddingEvidence {
            query_revision: query_revision.into(),
            language: language.map(str::to_owned),
            embedding: embedding(vec![1.0, 0.0]),
            provenance: provenance(true),
        })
    }
}

fn embedding(values: Vec<f32>) -> SemanticEmbedding {
    let space =
        SemanticEmbeddingSpace::new(shadow_ai::SEMANTIC_EMBEDDING_CONTRACT_VERSION, SPACE, 2)
            .expect("space");
    SemanticEmbedding::new(space, values).expect("embedding")
}

fn provenance(tokenizer: bool) -> VisionProvenance {
    VisionProvenance {
        job_id: "vision-job".into(),
        provider: "fake".into(),
        deployment: "fake".into(),
        model_build: "fake-build".into(),
        artifact_sha256: "fake-sha256".into(),
        preprocessing_identity: "fake-preprocess".into(),
        postprocessing_identity: "fake-postprocess".into(),
        tokenizer: tokenizer.then(|| VisionTokenizerProvenance {
            identity: "fake-tokenizer".into(),
            artifact_sha256: "fake-tokenizer-sha256".into(),
            max_length: 64,
            lowercase: true,
        }),
        runtime: "fake-runtime".into(),
        requested_execution_provider: "cpu".into(),
        actual_execution_provider: "cpu".into(),
        execution_provider_fallback_reason: None,
        precision: "fp32".into(),
    }
}

#[test]
fn bounded_current_visuals_rank_without_exposing_vectors_or_paths() {
    let fixture = Fixture::new();
    fixture.add_photo(1, "/photos/a.dng", b"first-jpeg");
    fixture.add_photo(2, "/photos/b.dng", b"second-jpeg");
    let provider = FakeSemanticProvider::new();

    let report = search_review_semantics(
        &fixture.catalog,
        &fixture.cache_root,
        &provider,
        "第一张照片",
        "shadow:query:v1",
        Some("zh-CN"),
        SemanticSearchPolicy::default(),
    )
    .expect("semantic search");

    assert_eq!(report.considered_photos, 2);
    assert_eq!(report.embedded_photos, 2);
    assert_eq!(provider.image_calls.get(), 2);
    assert!((report.matches[0].cosine_similarity - 1.0).abs() < f32::EPSILON);
    assert!(report.matches[1].cosine_similarity.abs() < f32::EPSILON);
    let debug = format!("{report:?}");
    assert!(!debug.contains("first-jpeg"));
    assert!(!debug.contains("/photos/"));
    assert!(!debug.contains("values"));
}

#[test]
fn similar_review_keeps_anchor_first_and_reuses_candidate_embeddings() {
    let fixture = Fixture::new();
    let (anchor_representation, _, _) = fixture.add_photo(1, "/photos/a.dng", b"first-jpeg");
    let (unrelated_representation, _, _) = fixture.add_photo(2, "/photos/b.dng", b"second-jpeg");
    let (similar_representation, _, _) = fixture.add_photo(3, "/photos/c.dng", b"first-jpeg");
    let page = fixture.catalog.review_page(None, 8).expect("review page");
    let anchor = page.items.first().expect("anchor");
    let provider = FakeSemanticProvider::new();

    for pass in 0..2 {
        let report = suggest_similar_review_photos_with_control(
            &fixture.catalog,
            &fixture.cache_root,
            &provider,
            anchor.photo_id,
            anchor_representation,
            &|| false,
        )
        .expect("similar review");
        assert_eq!(report.considered_photos, 3);
        assert_eq!(report.matches.len(), 3);
        assert_eq!(report.matches[0].representation_id, anchor_representation);
        assert_eq!(report.matches[1].representation_id, similar_representation);
        assert_eq!(
            report.matches[2].representation_id,
            unrelated_representation
        );
        assert_eq!(provider.image_calls.get(), if pass == 0 { 3 } else { 4 });
    }
}

#[test]
fn similar_review_rejects_a_replaced_anchor_and_honors_cancellation() {
    let fixture = Fixture::new();
    let (representation_id, source, mut replacement) =
        fixture.add_photo(1, "/photos/a.dng", b"first-jpeg");
    let anchor = fixture
        .catalog
        .review_page(None, 8)
        .expect("review page")
        .items[0]
        .photo_id;
    replacement.generator_version = "2".into();
    replacement.created_at_ms += 1;
    let provider = FakeSemanticProvider::replacing(
        fixture.catalog.clone(),
        RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: replacement,
        },
    );
    assert!(matches!(
        suggest_similar_review_photos_with_control(
            &fixture.catalog,
            &fixture.cache_root,
            &provider,
            anchor,
            representation_id,
            &|| true,
        ),
        Err(SemanticSearchError::Cancelled)
    ));
    assert_eq!(provider.image_calls.get(), 0);
    assert!(matches!(
        suggest_similar_review_photos_with_control(
            &fixture.catalog,
            &fixture.cache_root,
            &provider,
            anchor,
            representation_id,
            &|| false,
        ),
        Err(SemanticSearchError::AnchorUnavailable)
    ));
}

#[test]
fn maximum_photo_policy_prevents_an_unbounded_library_scan() {
    let fixture = Fixture::new();
    fixture.add_photo(1, "/photos/a.dng", b"first-jpeg");
    fixture.add_photo(2, "/photos/b.dng", b"second-jpeg");
    let provider = FakeSemanticProvider::new();

    let report = search_review_semantics(
        &fixture.catalog,
        &fixture.cache_root,
        &provider,
        "照片",
        "shadow:query:v1",
        Some("zh-CN"),
        SemanticSearchPolicy { maximum_photos: 1 },
    )
    .expect("bounded search");

    assert_eq!(report.considered_photos, 1);
    assert_eq!(provider.image_calls.get(), 1);
    assert!(report.truncated);
}

#[test]
fn result_is_discarded_when_the_selected_artifact_changes_during_inference() {
    let fixture = Fixture::new();
    let (representation_id, source, mut replacement) =
        fixture.add_photo(1, "/photos/a.dng", b"first-jpeg");
    replacement.generator_version = "2".into();
    replacement.created_at_ms += 1;
    let provider = FakeSemanticProvider::replacing(
        fixture.catalog.clone(),
        RecordCachedArtifact {
            representation_id,
            expected_source: source,
            artifact: replacement,
        },
    );

    let report = search_review_semantics(
        &fixture.catalog,
        &fixture.cache_root,
        &provider,
        "照片",
        "shadow:query:v1",
        Some("zh-CN"),
        SemanticSearchPolicy::default(),
    )
    .expect("stale result is normal");

    assert_eq!(report.skipped.stale_input, 1);
    assert_eq!(report.embedded_photos, 0);
    assert!(report.matches.is_empty());
}

struct Fixture {
    root: PathBuf,
    actor: Option<CatalogActor>,
    catalog: CatalogHandle,
    cache_root: PathBuf,
}

impl Fixture {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!(
            "shadow-semantic-search-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create fixture");
        let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("start catalog");
        let catalog = actor.handle();
        let cache_root = root.join("cache");
        Self {
            root,
            actor: Some(actor),
            catalog,
            cache_root,
        }
    }

    fn add_photo(
        &self,
        ordinal: u8,
        path: &str,
        visual: &[u8],
    ) -> (RepresentationId, RepresentationFingerprint, CachedArtifact) {
        let source = RepresentationFingerprint {
            byte_len: 4_096 + u64::from(ordinal),
            modified_at_ms: Some(i64::from(ordinal)),
        };
        let registered = self
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: i64::from(ordinal),
            })
            .expect("register photo");
        let cache = ContentAddressedStore::open(&self.cache_root).expect("open cache");
        let blob = cache.put(visual).expect("store visual");
        let artifact = CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: format!("semantic-test-{ordinal}"),
            generator_id: "semantic-test".into(),
            generator_version: "1".into(),
            recipe_snapshot_digest: None,
            provider_preview_id: None,
            blob_algorithm: blob.digest.algorithm().into(),
            blob_digest: *blob.digest.as_bytes(),
            blob_byte_len: blob.byte_len,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 2,
                height: 2,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 100 + i64::from(ordinal),
        };
        assert_eq!(
            self.catalog
                .record_cached_artifact(&RecordCachedArtifact {
                    representation_id: registered.representation_id,
                    expected_source: source,
                    artifact: artifact.clone(),
                })
                .expect("record visual"),
            RecordCachedArtifactStatus::Recorded
        );
        (registered.representation_id, source, artifact)
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        if let Some(actor) = self.actor.take() {
            actor.shutdown().expect("stop catalog");
        }
        std::fs::remove_dir_all(&self.root).expect("remove fixture");
    }
}

#[test]
fn second_query_reuses_image_vectors_and_cancelled_query_never_runs_inference() {
    let fixture = Fixture::new();
    fixture.add_photo(1, "/photos/a.dng", b"first-jpeg");
    let provider = FakeSemanticProvider::new();
    for query in ["first", "second"] {
        let report = search_review_semantics(
            &fixture.catalog,
            &fixture.cache_root,
            &provider,
            query,
            query,
            None,
            SemanticSearchPolicy::default(),
        )
        .expect("search");
        assert_eq!(report.matches.len(), 1);
    }
    assert_eq!(provider.image_calls.get(), 1);
    assert!(matches!(
        search_review_semantics_with_control(
            &fixture.catalog,
            &fixture.cache_root,
            &provider,
            "cancelled",
            "cancelled",
            None,
            SemanticSearchPolicy::default(),
            &|| true
        ),
        Err(SemanticSearchError::Cancelled)
    ));
    assert_eq!(provider.image_calls.get(), 1);
}
