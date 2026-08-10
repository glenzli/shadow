use std::{
    path::PathBuf,
    sync::atomic::{AtomicUsize, Ordering},
};

use shadow_ai::{
    ImageUnderstandingEvidence, ImageUnderstandingProvenance, ImageUnderstandingProvider,
    ImageUnderstandingQuality, InferRuntimeClientError, SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION,
    SemanticEvidenceKind, SemanticImageAnalysis, SemanticKeywordKind, SemanticKeywordSuggestion,
    SemanticRequestPriority, SemanticShortCaption,
};
use shadow_cache::ContentAddressedStore;
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogActor, LibraryKeywordAssignmentOrigin,
    RecordCachedArtifact, RecordCachedArtifactStatus, RegisterAsset, RepresentationFingerprint,
};
use shadow_domain::{
    AssetLocation, EntityId as _, ImageDimensions, PhotoId, Platform, PreviewByteOrder,
    PreviewCodec, RepresentationId, RepresentationKind,
};

use super::super::store::ImageUnderstandingStore;
use super::super::*;
use crate::display_jpeg_fixture::DISPLAY_JPEG_BYTES;

#[derive(Debug, Default)]
struct FakeUnderstandingProvider {
    calls: AtomicUsize,
}

impl ImageUnderstandingProvider for FakeUnderstandingProvider {
    fn describe_image(
        &self,
        _image: &[u8],
        media_type: &str,
        source_revision: &str,
        language: &str,
        quality: ImageUnderstandingQuality,
        priority: SemanticRequestPriority,
    ) -> Result<ImageUnderstandingEvidence, InferRuntimeClientError> {
        assert_eq!(media_type, "image/jpeg");
        assert_eq!(language, "zh-CN");
        assert_eq!(quality, ImageUnderstandingQuality::Basic);
        assert_eq!(priority, SemanticRequestPriority::Background);
        self.calls.fetch_add(1, Ordering::Relaxed);
        Ok(ImageUnderstandingEvidence {
            source_revision: source_revision.into(),
            width: 2,
            height: 2,
            orientation: "display_pixels_orientation_normalized".into(),
            analysis: analysis(),
            provenance: provenance(),
        })
    }
}

#[test]
fn one_page_batches_resume_and_publish_rebuildable_proposals() {
    let fixture = WorkflowFixture::new();
    let first = fixture.add_photo(1, "/photos/one.dng");
    let second = fixture.add_photo(2, "/photos/two.dng");
    let provider = FakeUnderstandingProvider::default();
    let policy = ImageUnderstandingScanPolicy::new(ImageUnderstandingScanScope::All, None, 1)
        .expect("all policy");

    let first_batch = process_image_understanding_batch(
        &fixture.catalog,
        &fixture.cache_root,
        &provider,
        policy,
        &ImageUnderstandingRequest {
            generation: None,
            start_new: true,
            auto_apply_keywords: false,
        },
    )
    .expect("first batch");
    assert_eq!(
        first_batch.snapshot.status,
        ImageUnderstandingRunStatus::Running
    );
    assert_eq!(first_batch.snapshot.total_photos, 2);
    assert_eq!(first_batch.snapshot.processed_photos, 1);
    assert_eq!(first_batch.analyzed_photos, 1);

    let second_batch = process_image_understanding_batch(
        &fixture.catalog,
        &fixture.cache_root,
        &provider,
        policy,
        &ImageUnderstandingRequest {
            generation: Some(first_batch.snapshot.generation.clone()),
            start_new: false,
            auto_apply_keywords: false,
        },
    )
    .expect("resumed batch");
    assert_eq!(
        second_batch.snapshot.status,
        ImageUnderstandingRunStatus::Complete
    );
    assert_eq!(second_batch.snapshot.processed_photos, 2);
    assert_eq!(provider.calls.load(Ordering::Relaxed), 2);

    let store = ImageUnderstandingStore::open(&fixture.cache_root).expect("open proposal store");
    for (photo_id, representation_id) in [first, second] {
        let proposal = store
            .proposal(photo_id, representation_id)
            .expect("proposal lookup")
            .expect("stored proposal");
        assert_eq!(
            proposal.analysis.short_caption.text,
            "山间的日落。\n".trim()
        );
        assert_eq!(
            proposal.disposition,
            ImageUnderstandingProposalDisposition::Suggested
        );
    }
    let manual_keyword = fixture
        .catalog
        .create_library_keyword(None, "山景", 500)
        .expect("create matching manual keyword");
    fixture
        .catalog
        .assign_library_keyword_to_photos(
            manual_keyword.id,
            &[first.0],
            LibraryKeywordAssignmentOrigin::Manual,
            "",
            None,
            501,
        )
        .expect("assign manual keyword");
    let first_revision = store
        .proposal(first.0, first.1)
        .expect("first proposal")
        .expect("first stored proposal")
        .source_revision;
    let second_revision = store
        .proposal(second.0, second.1)
        .expect("second proposal")
        .expect("second stored proposal")
        .source_revision;
    drop(store);

    let automatic = apply_image_understanding_keywords(
        &fixture.catalog,
        &fixture.cache_root,
        first.0,
        first.1,
        &first_revision,
        ImageUnderstandingKeywordAcceptance::Automatic,
    )
    .expect("apply automatic suggestions");
    assert_eq!(automatic.changed_assignments, 0);
    assert_eq!(
        fixture
            .catalog
            .library_keywords_for_photo(first.0)
            .expect("manual assignments")[0]
            .origin,
        LibraryKeywordAssignmentOrigin::Manual
    );
    let accepted = apply_image_understanding_keywords(
        &fixture.catalog,
        &fixture.cache_root,
        second.0,
        second.1,
        &second_revision,
        ImageUnderstandingKeywordAcceptance::User,
    )
    .expect("accept suggestions");
    assert_eq!(accepted.changed_assignments, 1);
    let second_keywords = fixture
        .catalog
        .library_keywords_for_photo(second.0)
        .expect("AI assignments");
    assert_eq!(second_keywords.len(), 1);
    assert_eq!(second_keywords[0].keyword.name, "山景");
    assert_eq!(
        second_keywords[0].origin,
        LibraryKeywordAssignmentOrigin::AiAccepted
    );
    assert_eq!(
        image_understanding_proposal(&fixture.cache_root, second.0, second.1)
            .expect("public proposal projection")
            .expect("proposal")
            .disposition,
        ImageUnderstandingProposalDisposition::Accepted
    );
    fixture.finish();
}

#[test]
fn enabled_automatic_keywords_are_applied_during_the_background_batch() {
    let fixture = WorkflowFixture::new();
    let (photo_id, representation_id) = fixture.add_photo(1, "/photos/automatic.dng");
    let provider = FakeUnderstandingProvider::default();
    let policy = ImageUnderstandingScanPolicy::new(ImageUnderstandingScanScope::All, None, 4)
        .expect("all policy");

    let batch = process_image_understanding_batch(
        &fixture.catalog,
        &fixture.cache_root,
        &provider,
        policy,
        &ImageUnderstandingRequest {
            generation: None,
            start_new: true,
            auto_apply_keywords: true,
        },
    )
    .expect("automatic keyword batch");

    assert_eq!(batch.snapshot.status, ImageUnderstandingRunStatus::Complete);
    let assignments = fixture
        .catalog
        .library_keywords_for_photo(photo_id)
        .expect("automatic keyword assignments");
    assert_eq!(assignments.len(), 1);
    assert_eq!(assignments[0].keyword.name, "山景");
    assert_eq!(
        assignments[0].origin,
        LibraryKeywordAssignmentOrigin::AiAccepted
    );
    assert_eq!(
        image_understanding_proposal(&fixture.cache_root, photo_id, representation_id)
            .expect("public proposal projection")
            .expect("proposal")
            .disposition,
        ImageUnderstandingProposalDisposition::AutoApplied
    );
    fixture.finish();
}

fn analysis() -> SemanticImageAnalysis {
    SemanticImageAnalysis::new(
        SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION,
        "qwen3-vl-structured-v1",
        "shadow.photo-understanding.zh-cn.v1",
        vec![SemanticKeywordSuggestion {
            kind: SemanticKeywordKind::Scene,
            concept_id: "scene.mountain".into(),
            display_label: "山景".into(),
            evidence: SemanticEvidenceKind::DirectObservation,
        }],
        SemanticShortCaption {
            language_tag: "zh-CN".into(),
            text: "山间的日落。".into(),
        },
    )
    .expect("valid analysis")
}

fn provenance() -> ImageUnderstandingProvenance {
    ImageUnderstandingProvenance {
        job_id: "vision-test".into(),
        provider: "ollama".into(),
        deployment: "local".into(),
        model_profile: "basic".into(),
        model_build: "qwen3-vl-4b-local-v1".into(),
        physical_model: "qwen3-vl:4b".into(),
        runtime: "ollama_native_chat".into(),
        schema_revision: "image-description-v1".into(),
        prompt_revision: "shadow-description-v1".into(),
        total_duration_ms: Some(1_000),
        load_duration_ms: Some(100),
        prompt_eval_count: Some(80),
        eval_count: Some(40),
    }
}

struct WorkflowFixture {
    root: PathBuf,
    actor: CatalogActor,
    catalog: shadow_catalog::CatalogHandle,
    cache_root: PathBuf,
}

impl WorkflowFixture {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!(
            "shadow-image-understanding-workflow-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create workflow fixture");
        let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("start catalog");
        let catalog = actor.handle();
        let cache_root = root.join("cache");
        Self {
            root,
            actor,
            catalog,
            cache_root,
        }
    }

    fn add_photo(&self, ordinal: u8, path: &str) -> (PhotoId, RepresentationId) {
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
        let blob = cache.put(DISPLAY_JPEG_BYTES).expect("store JPEG");
        let artifact = CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: format!("understanding-test-{ordinal}"),
            generator_id: "image-understanding-test".into(),
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
                    artifact,
                })
                .expect("record cached visual"),
            RecordCachedArtifactStatus::Recorded
        );
        (registered.photo_id, registered.representation_id)
    }

    fn finish(self) {
        self.actor.shutdown().expect("stop catalog");
        std::fs::remove_dir_all(self.root).expect("remove workflow fixture");
    }
}
