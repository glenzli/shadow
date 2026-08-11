use super::*;
use crate::VisionTokenizerProvenance;

const SPACE: &str = "siglip2_base_patch16_224@75de2d55:fixres224:lowercase64:l2_768_fp32:v1";

fn values() -> Vec<f32> {
    let mut values = vec![0.0; SIGLIP_EMBEDDING_DIMENSIONS];
    values[0] = 1.0;
    values
}

fn provenance(tokenizer: bool) -> VisionProvenance {
    VisionProvenance {
        job_id: "vision-job".into(),
        provider: "onnx-local".into(),
        deployment: "siglip".into(),
        model_build: "siglip-build".into(),
        artifact_sha256: "artifact-sha256".into(),
        preprocessing_identity: "siglip-preprocess".into(),
        postprocessing_identity: "l2".into(),
        tokenizer: tokenizer.then(|| VisionTokenizerProvenance {
            identity: "siglip-tokenizer".into(),
            artifact_sha256: "tokenizer-sha256".into(),
            max_length: 64,
            lowercase: true,
        }),
        runtime: "onnxruntime".into(),
        requested_execution_provider: "cpu".into(),
        actual_execution_provider: "cpu".into(),
        execution_provider_fallback_reason: None,
        precision: "fp32".into(),
    }
}

fn raw_embedding() -> RawSemanticEmbedding {
    RawSemanticEmbedding {
        values: values(),
        dimensions: SIGLIP_EMBEDDING_DIMENSIONS,
        normalized: true,
        distance_metric: "cosine".into(),
        space: SPACE.into(),
    }
}

#[test]
fn image_and_text_responses_admit_the_same_exact_space() {
    let image = RawImageEmbeddingResponse {
        object: "vision.image_embedding".into(),
        status: "completed".into(),
        source_revision: "shadow:photo/artifact:abc".into(),
        image: RawImageGeometry {
            width: 224,
            height: 224,
            orientation: NORMALIZED_DISPLAY_ORIENTATION.into(),
        },
        embedding: raw_embedding(),
        provenance: provenance(false),
    }
    .validate("shadow:photo/artifact:abc")
    .expect("image response");
    let text = RawTextEmbeddingResponse {
        object: "vision.text_embedding".into(),
        status: "completed".into(),
        query_revision: "shadow:semantic-query:v1".into(),
        language: Some("zh-CN".into()),
        embedding: raw_embedding(),
        provenance: provenance(true),
    }
    .validate("shadow:semantic-query:v1", Some("zh-CN"))
    .expect("text response");

    assert_eq!(image.embedding.space(), text.embedding.space());
    assert_eq!(
        image.embedding.cosine_similarity(&text.embedding),
        Some(1.0)
    );
}

#[test]
fn response_validation_rejects_wrong_orientation_space_and_tokenizer() {
    let wrong_orientation = RawImageEmbeddingResponse {
        object: "vision.image_embedding".into(),
        status: "completed".into(),
        source_revision: "source".into(),
        image: RawImageGeometry {
            width: 224,
            height: 224,
            orientation: "input_pixels_no_exif_transform".into(),
        },
        embedding: raw_embedding(),
        provenance: provenance(false),
    };
    assert!(wrong_orientation.validate("source").is_err());

    let no_tokenizer = RawTextEmbeddingResponse {
        object: "vision.text_embedding".into(),
        status: "completed".into(),
        query_revision: "query".into(),
        language: None,
        embedding: raw_embedding(),
        provenance: provenance(false),
    };
    assert!(no_tokenizer.validate("query", None).is_err());

    let mut wrong_dimensions = raw_embedding();
    wrong_dimensions.dimensions -= 1;
    assert!(wrong_dimensions.admit().is_err());
}

#[test]
fn text_request_is_bounded_before_transport() {
    assert!(validate_text_request("海边日落", "shadow:query:v1", Some("zh-CN")).is_ok());
    assert!(validate_text_request(" ", "shadow:query:v1", None).is_err());
    assert!(validate_text_request("photo", "", None).is_err());
    assert!(validate_text_request("photo", "query", Some("zh_CN")).is_err());
}

#[test]
fn semantic_intents_follow_the_selected_consumer_contract() {
    assert_eq!(
        image_embedding_intent(InferRuntimeConsumerVersion::Candidate2),
        "vision.embed_image"
    );
    assert_eq!(
        text_embedding_intent(InferRuntimeConsumerVersion::Candidate2),
        "vision.embed_text"
    );
    assert_eq!(
        image_embedding_intent(InferRuntimeConsumerVersion::Candidate3),
        "semantic.embed_image"
    );
    assert_eq!(
        text_embedding_intent(InferRuntimeConsumerVersion::Candidate3),
        "semantic.embed_text"
    );
}

#[test]
#[ignore = "set SHADOW_TEST_SEMANTIC_IMAGE and SHADOW_INFER_TOKEN_FILE to run real local SigLIP HTTP inference"]
fn real_siglip_image_and_chinese_text_share_one_space() {
    let image_path = std::env::var("SHADOW_TEST_SEMANTIC_IMAGE").expect("semantic image path");
    let token_file = std::env::var("SHADOW_INFER_TOKEN_FILE").expect("token file path");
    let image = std::fs::read(&image_path).expect("read semantic image");
    let media_type = if image_path.to_ascii_lowercase().ends_with(".png") {
        "image/png"
    } else {
        "image/jpeg"
    };
    let source_revision = format!("shadow:test:artifact:{}", blake3::hash(&image).to_hex());
    let client = InferRuntimeClient::from_credential_file_with_discovery(
        None,
        std::path::Path::new(&token_file),
    )
    .expect("discover infer-runtime client");

    let image = client
        .embed_image_semantics(
            &image,
            media_type,
            &source_revision,
            SemanticRequestPriority::Background,
        )
        .expect("embed image");
    let text = client
        .embed_text_semantics(
            "摄影应用图标",
            "shadow:semantic-query:v1",
            Some("zh-CN"),
            SemanticRequestPriority::Interactive,
        )
        .expect("embed Chinese text");

    assert_eq!(image.orientation, NORMALIZED_DISPLAY_ORIENTATION);
    assert_eq!(image.embedding.space(), text.embedding.space());
    assert!(image.embedding.cosine_similarity(&text.embedding).is_some());
}
