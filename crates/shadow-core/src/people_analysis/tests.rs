use std::{
    path::PathBuf,
    sync::atomic::{AtomicBool, Ordering},
};

use shadow_ai::{
    DetectedFace, DetectedFaceBatch, EmbeddedFace, FaceAnalysisProvider, FaceBoundingBox,
    FaceEmbedding, FaceEmbeddingEligibility, FaceLandmarks, FacePoint, InferRuntimeClient,
    InferRuntimeClientError, ParsedFace, VisionProvenance,
};
use shadow_cache::ContentAddressedStore;
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogActor, CatalogStore, RecordCachedArtifact,
    RecordCachedArtifactStatus, RegisterAsset, RepresentationFingerprint,
};
use shadow_domain::{
    AssetLocation, EntityId, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec,
    RepresentationKind,
};

use super::*;
use crate::display_jpeg_fixture::DISPLAY_JPEG_BYTES;

#[derive(Debug)]
struct FakeFaceProvider;

impl FaceAnalysisProvider for FakeFaceProvider {
    fn detect_faces(
        &self,
        _image: &[u8],
        _media_type: &str,
        source_revision: &str,
    ) -> Result<DetectedFaceBatch, InferRuntimeClientError> {
        let (width, height) = image::load_from_memory(_image)
            .map(|image| (image.width(), image.height()))
            .unwrap();
        Ok(DetectedFaceBatch {
            source_revision: source_revision.into(),
            width,
            height,
            orientation: "input_pixels_no_exif_transform".into(),
            detections: vec![DetectedFace {
                bounding_box: FaceBoundingBox {
                    x: 0.0,
                    y: 0.0,
                    width: width as f32,
                    height: height as f32,
                },
                landmarks: FaceLandmarks {
                    right_eye: FacePoint { x: 0.4, y: 0.5 },
                    left_eye: FacePoint { x: 1.4, y: 0.5 },
                    nose_tip: FacePoint { x: 0.9, y: 0.9 },
                    right_mouth_corner: FacePoint { x: 0.5, y: 1.4 },
                    left_mouth_corner: FacePoint { x: 1.3, y: 1.4 },
                },
                confidence: 0.95,
            }],
            provenance: provenance("yunet-sha256"),
        })
    }

    fn embed_face(
        &self,
        _image: &[u8],
        _media_type: &str,
        source_revision: &str,
        _landmarks: FaceLandmarks,
    ) -> Result<EmbeddedFace, InferRuntimeClientError> {
        let mut values = vec![0.0; shadow_ai::SFACE_EMBEDDING_DIMENSIONS];
        values[0] = 1.0;
        Ok(EmbeddedFace {
            source_revision: source_revision.into(),
            embedding: FaceEmbedding::new(values, "sface-test-space".into())
                .expect("test embedding"),
            eligibility: FaceEmbeddingEligibility {
                eligible: true,
                landmarks_in_image: true,
                inter_eye_distance_pixels: 10.0,
                alignment_rmse_pixels: 0.1,
            },
            provenance: provenance("sface-sha256"),
        })
    }

    fn parse_face(
        &self,
        _image: &[u8],
        _media_type: &str,
        source_revision: &str,
        _face_box: FaceBoundingBox,
    ) -> Result<ParsedFace, InferRuntimeClientError> {
        Ok(ParsedFace {
            source_revision: source_revision.into(),
            width: 2,
            height: 2,
            labels: vec![0; 4],
            provenance: provenance("face-parser-sha256"),
        })
    }
}

fn provenance(artifact_sha256: &str) -> VisionProvenance {
    VisionProvenance {
        job_id: "vision-job".into(),
        provider: "onnx-local".into(),
        deployment: "test".into(),
        model_build: "test-build".into(),
        artifact_sha256: artifact_sha256.into(),
        preprocessing_identity: "test-preprocess".into(),
        postprocessing_identity: "test-postprocess".into(),
        tokenizer: None,
        runtime: "onnxruntime-test".into(),
        requested_execution_provider: "cpu".into(),
        actual_execution_provider: "cpu".into(),
        execution_provider_fallback_reason: None,
        precision: "fp32".into(),
    }
}

#[test]
fn cached_visuals_reach_transient_anonymous_grouping_without_persisting_vectors() {
    let fixture = PeopleFixture::new();
    fixture.add_photo(1, "/photos/one.dng");
    fixture.add_photo(2, "/photos/two.dng");

    let report = analyze_review_people(
        &fixture.catalog,
        &fixture.cache_root,
        &FakeFaceProvider,
        PeopleAnalysisPolicy::default(),
    )
    .expect("people analysis");

    assert_eq!(report.analyzed_photos, 2);
    assert_eq!(report.detected_faces, 2);
    assert_eq!(report.embedded_faces, 2);
    assert_eq!(report.grouping.groups.len(), 1);
    assert_eq!(report.grouping.groups[0].members.len(), 2);
    assert_eq!(report.face_previews.len(), 2);
    let thumbnail = image::load_from_memory(&report.face_previews[0].thumbnail_jpeg)
        .expect("decode representative face thumbnail");
    assert_eq!(thumbnail.width(), 88);
    assert_eq!(thumbnail.height(), 88);
    assert!(!report.truncated);
    fixture.finish();
}

#[derive(Debug)]
struct CancelledControl {
    published: AtomicBool,
}

impl PeopleAnalysisControl for CancelledControl {
    fn cancellation_requested(&self) -> bool {
        true
    }

    fn publish(&self, _progress: PeopleAnalysisProgress) {
        self.published.store(true, Ordering::Release);
    }
}

#[test]
fn cancellation_before_review_avoids_provider_and_progress_work() {
    let fixture = PeopleFixture::new();
    fixture.add_photo(1, "/photos/one.dng");
    let control = CancelledControl {
        published: AtomicBool::new(false),
    };

    let error = analyze_review_people_with_control(
        &fixture.catalog,
        &fixture.cache_root,
        &FakeFaceProvider,
        PeopleAnalysisPolicy::default(),
        &control,
    )
    .expect_err("cancelled analysis must stop");

    assert!(matches!(error, PeopleAnalysisError::Cancelled));
    assert!(!control.published.load(Ordering::Acquire));
    fixture.finish();
}

#[test]
#[ignore = "set SHADOW_TEST_FACE_IMAGE and SHADOW_INFER_TOKEN_FILE to run real local YuNet/SFace HTTP inference"]
fn real_yunet_sface_http_path_forms_one_anonymous_group() {
    let image_path = std::env::var("SHADOW_TEST_FACE_IMAGE").expect("face image path");
    let token_file = std::env::var("SHADOW_INFER_TOKEN_FILE").expect("token file path");
    let image = std::fs::read(&image_path).expect("read face image");
    let fixture = PeopleFixture::new();
    let (width, height) = image::image_dimensions(&image_path).expect("image dimensions");
    fixture.add_photo_with_visual(1, "/photos/real-one.dng", &image, width, height);
    fixture.add_photo_with_visual(2, "/photos/real-two.dng", &image, width, height);
    let provider = InferRuntimeClient::from_credential_file_with_discovery(
        None,
        std::path::Path::new(&token_file),
    )
    .expect("configure infer-runtime client");

    let report = analyze_review_people(
        &fixture.catalog,
        &fixture.cache_root,
        &provider,
        PeopleAnalysisPolicy::default(),
    )
    .expect("real people analysis");

    assert_eq!(report.analyzed_photos, 2);
    assert!(report.detected_faces >= 2);
    assert!(report.embedded_faces >= 2);
    eprintln!(
        "Local face evidence: detected={}, embedded={}, quality_rejected={}, previews={}",
        report.detected_faces,
        report.embedded_faces,
        report.skipped.low_face_quality,
        report.face_previews.len()
    );
    assert!(!report.grouping.groups.is_empty());
    assert!(report.grouping.groups.iter().all(|g| g.members.len() == 2));
    fixture.finish();
}

struct PeopleFixture {
    root: PathBuf,
    actor: CatalogActor,
    catalog: shadow_catalog::CatalogHandle,
    cache_root: PathBuf,
}

impl PeopleFixture {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!(
            "shadow-people-analysis-{}-{}",
            std::process::id(),
            shadow_domain::RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create people fixture");
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

    fn add_photo(&self, ordinal: u8, path: &str) {
        let image = image::RgbImage::from_fn(96, 96, |x, y| {
            let value = if (x / 6 + y / 9) % 2 == 0 { 50 } else { 180 };
            image::Rgb([value, value, value])
        });
        let mut jpeg = Vec::new();
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut jpeg, 90)
            .encode_image(&image)
            .unwrap();
        self.add_photo_with_visual(ordinal, path, &jpeg, 96, 96);
    }

    fn add_photo_with_visual(
        &self,
        ordinal: u8,
        path: &str,
        visual: &[u8],
        width: u32,
        height: u32,
    ) {
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
        let blob = cache.put(visual).expect("store JPEG");
        let artifact = CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: format!("people-test-{ordinal}"),
            generator_id: "people-test".into(),
            generator_version: "1".into(),
            recipe_snapshot_digest: None,
            provider_preview_id: None,
            blob_algorithm: blob.digest.algorithm().into(),
            blob_digest: *blob.digest.as_bytes(),
            blob_byte_len: blob.byte_len,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions { width, height },
            bits_per_channel: 8,
            channels: 1,
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
    }

    fn finish(self) {
        self.actor.shutdown().expect("stop catalog");
        std::fs::remove_dir_all(self.root).expect("remove people fixture");
    }
}

#[test]
fn incremental_people_scan_continues_past_a_batch_and_skips_unchanged_inputs() {
    let fixture = PeopleFixture::new();
    for n in 1..=5 {
        fixture.add_photo(n, &format!("/photos/{n}.dng"));
    }
    let policy = PeopleAnalysisPolicy {
        maximum_photos: 2,
        ..Default::default()
    };
    let mut selection = PeopleAnalysisSelection::default();
    let mut visited = Vec::new();
    for batch in 0..3 {
        let report = analyze_review_people_incremental(
            &fixture.catalog,
            &fixture.cache_root,
            &FakeFaceProvider,
            policy,
            &UnobservedPeopleAnalysis,
            Some(&selection),
        )
        .unwrap();
        assert_eq!(report.analyzed_photos, if batch == 2 { 1 } else { 2 });
        assert_eq!(report.truncated, batch < 2);
        for input in report.completed_inputs {
            assert!(!visited.contains(&input.representation_id));
            visited.push(input.representation_id.clone());
            selection
                .known_inputs
                .insert(input.representation_id, input.source_revision);
        }
    }
    assert_eq!(visited.len(), 5);
    let unchanged = analyze_review_people_incremental(
        &fixture.catalog,
        &fixture.cache_root,
        &FakeFaceProvider,
        policy,
        &UnobservedPeopleAnalysis,
        Some(&selection),
    )
    .unwrap();
    assert_eq!(unchanged.analyzed_photos, 0);
    assert_eq!(unchanged.detected_faces, 0);
    fixture.finish();
}

#[test]
fn incremental_people_scan_reuses_reference_photo_for_cross_batch_grouping() {
    let fixture = PeopleFixture::new();
    fixture.add_photo(1, "/photos/1.dng");
    let policy = PeopleAnalysisPolicy::default();
    let first = analyze_review_people(
        &fixture.catalog,
        &fixture.cache_root,
        &FakeFaceProvider,
        policy,
    )
    .unwrap();
    let input = first.completed_inputs[0].clone();
    let selection = PeopleAnalysisSelection {
        known_inputs: [(input.representation_id, input.source_revision)].into(),
        anchor_photo_ids: [input.photo_id].into(),
    };
    fixture.add_photo(2, "/photos/2.dng");
    let next = analyze_review_people_incremental(
        &fixture.catalog,
        &fixture.cache_root,
        &FakeFaceProvider,
        policy,
        &UnobservedPeopleAnalysis,
        Some(&selection),
    )
    .unwrap();
    assert_eq!(next.grouping.groups.len(), 1);
    assert_eq!(next.grouping.groups[0].members.len(), 2);
    fixture.finish();
}

#[test]
fn people_analysis_obeys_current_library_source_membership() {
    let fixture = PeopleFixture::new();
    fixture.add_photo(1, "/old/portrait.dng");
    let root = AssetLocation::new(Platform::MacOs, b"/current".to_vec(), "/current");
    fixture
        .catalog
        .clone()
        .begin_import_session(&root, 500)
        .unwrap();
    // A current source registry excludes unowned historical locations, even
    // when their old generated proxies remain in the cache.
    assert!(
        people_analysis_library_membership(&fixture.catalog)
            .unwrap()
            .is_empty()
    );
    let report = analyze_review_people(
        &fixture.catalog,
        &fixture.cache_root,
        &FakeFaceProvider,
        PeopleAnalysisPolicy::default(),
    )
    .unwrap();
    assert_eq!(report.analyzed_photos, 0);
    assert_eq!(report.detected_faces, 0);
    fixture.finish();
}

#[test]
fn unusably_small_faces_are_checkpointed_without_embedding_or_person_creation() {
    let fixture = PeopleFixture::new();
    fixture.add_photo_with_visual(1, "/photos/tiny.dng", DISPLAY_JPEG_BYTES, 2, 2);
    let report = analyze_review_people(
        &fixture.catalog,
        &fixture.cache_root,
        &FakeFaceProvider,
        PeopleAnalysisPolicy::default(),
    )
    .unwrap();
    assert_eq!(report.detected_faces, 1);
    assert_eq!(report.embedded_faces, 0);
    assert_eq!(report.skipped.low_face_quality, 1);
    assert_eq!(report.rejected_faces.len(), 1);
    assert_eq!(report.completed_inputs.len(), 1);
    assert!(report.face_previews.is_empty());
    assert!(report.grouping.groups.is_empty() && report.grouping.ungrouped.is_empty());
    fixture.finish();
}

#[test]
fn pre_quality_checkpoints_are_revisited_once_and_new_policy_is_cached() {
    let fixture = PeopleFixture::new();
    fixture.add_photo(1, "/photos/one.dng");
    let current = analyze_review_people(
        &fixture.catalog,
        &fixture.cache_root,
        &FakeFaceProvider,
        PeopleAnalysisPolicy::default(),
    )
    .unwrap();
    let input = &current.completed_inputs[0];
    let old_revision = input.source_revision.replace("/people-quality-v1", "");
    let mut selection = PeopleAnalysisSelection {
        known_inputs: [(input.representation_id.clone(), old_revision)].into(),
        ..Default::default()
    };
    let refresh = analyze_review_people_incremental(
        &fixture.catalog,
        &fixture.cache_root,
        &FakeFaceProvider,
        PeopleAnalysisPolicy::default(),
        &UnobservedPeopleAnalysis,
        Some(&selection),
    )
    .unwrap();
    assert_eq!(refresh.analyzed_photos, 1);
    selection.known_inputs.insert(
        input.representation_id.clone(),
        input.source_revision.clone(),
    );
    let unchanged = analyze_review_people_incremental(
        &fixture.catalog,
        &fixture.cache_root,
        &FakeFaceProvider,
        PeopleAnalysisPolicy::default(),
        &UnobservedPeopleAnalysis,
        Some(&selection),
    )
    .unwrap();
    assert_eq!(unchanged.analyzed_photos, 0);
    fixture.finish();
}
