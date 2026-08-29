use std::path::PathBuf;

use shadow_ai::{
    DetectedFace, DetectedFaceBatch, EmbeddedFace, FaceAnalysisProvider, FaceBoundingBox,
    FaceEmbedding, FaceEmbeddingEligibility, FaceLandmarks, FacePoint, InferRuntimeClient,
    InferRuntimeClientError, ParsedFace, VisionProvenance,
};
use shadow_cache::ContentAddressedStore;
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogActor, RecordCachedArtifact,
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
        Ok(DetectedFaceBatch {
            source_revision: source_revision.into(),
            width: 2,
            height: 2,
            orientation: "input_pixels_no_exif_transform".into(),
            detections: vec![DetectedFace {
                bounding_box: FaceBoundingBox {
                    x: 0.0,
                    y: 0.0,
                    width: 2.0,
                    height: 2.0,
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
    assert_eq!(report.group_previews.len(), 1);
    let thumbnail = image::load_from_memory(&report.group_previews[0].thumbnail_jpeg)
        .expect("decode representative face thumbnail");
    assert_eq!(thumbnail.width(), 88);
    assert_eq!(thumbnail.height(), 88);
    assert!(!report.truncated);
    fixture.finish();
}

#[test]
#[ignore = "set SHADOW_TEST_FACE_IMAGE and SHADOW_INFER_TOKEN_FILE to run real local YuNet/SFace HTTP inference"]
fn real_yunet_sface_http_path_forms_one_anonymous_group() {
    let image_path = std::env::var("SHADOW_TEST_FACE_IMAGE").expect("face image path");
    let token_file = std::env::var("SHADOW_INFER_TOKEN_FILE").expect("token file path");
    let image = std::fs::read(&image_path).expect("read face image");
    let fixture = PeopleFixture::new();
    fixture.add_photo_with_visual(1, "/photos/real-one.dng", &image, 512, 512);
    fixture.add_photo_with_visual(2, "/photos/real-two.dng", &image, 512, 512);
    let provider = InferRuntimeClient::from_credential_file(
        "http://127.0.0.1:8787",
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
    assert_eq!(report.detected_faces, 2);
    assert_eq!(report.embedded_faces, 2);
    assert_eq!(report.grouping.groups.len(), 1);
    assert_eq!(report.grouping.groups[0].members.len(), 2);
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
        self.add_photo_with_visual(ordinal, path, DISPLAY_JPEG_BYTES, 2, 2);
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
