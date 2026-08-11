//! Strict `SigLIP` image/text adapters over infer-runtime's experimental routes.
//!
//! Both results are admitted through the provider-neutral semantic contract.
//! The caller must still bind image evidence to its current Catalog/cache
//! revision and partition every search index by the exact embedding space.

use std::collections::BTreeMap;

use reqwest::blocking::multipart;
use serde::{Deserialize, Serialize};

use crate::{SEMANTIC_EMBEDDING_CONTRACT_VERSION, SemanticEmbedding, SemanticEmbeddingSpace};

use super::{
    InferRuntimeClient, InferRuntimeClientError, RawImageGeometry, VisionProvenance,
    discovery::InferRuntimeConsumerVersion, image_form, valid_provenance, validate_request,
};

const IMAGE_EMBEDDING_PATH: &str = "infer/v1/vision/image-embeddings";
const TEXT_EMBEDDING_PATH: &str = "infer/v1/vision/text-embeddings";
const NORMALIZED_DISPLAY_ORIENTATION: &str = "display_pixels_orientation_normalized";
const SIGLIP_EMBEDDING_DIMENSIONS: usize = 768;
const MAX_TEXT_BYTES: usize = 4_096;
const MAX_REVISION_BYTES: usize = 256;
const MAX_LANGUAGE_BYTES: usize = 35;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum SemanticRequestPriority {
    Interactive,
    Background,
}

impl SemanticRequestPriority {
    pub(super) const fn as_str(self) -> &'static str {
        match self {
            Self::Interactive => "interactive",
            Self::Background => "background",
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ImageEmbeddingEvidence {
    pub source_revision: String,
    pub width: u32,
    pub height: u32,
    pub orientation: String,
    pub embedding: SemanticEmbedding,
    pub provenance: VisionProvenance,
}

#[derive(Debug, Clone, PartialEq)]
pub struct TextEmbeddingEvidence {
    pub query_revision: String,
    pub language: Option<String>,
    pub embedding: SemanticEmbedding,
    pub provenance: VisionProvenance,
}

pub trait SemanticEmbeddingProvider {
    /// Encodes one orientation-normalized display JPEG/PNG.
    ///
    /// # Errors
    ///
    /// Returns a fail-closed local transport or typed-contract error.
    fn embed_image_semantics(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        priority: SemanticRequestPriority,
    ) -> Result<ImageEmbeddingEvidence, InferRuntimeClientError>;

    /// Encodes one bounded text query in the same model-owned space.
    ///
    /// # Errors
    ///
    /// Returns a fail-closed local transport or typed-contract error.
    fn embed_text_semantics(
        &self,
        text: &str,
        query_revision: &str,
        language: Option<&str>,
        priority: SemanticRequestPriority,
    ) -> Result<TextEmbeddingEvidence, InferRuntimeClientError>;
}

impl SemanticEmbeddingProvider for InferRuntimeClient {
    fn embed_image_semantics(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        priority: SemanticRequestPriority,
    ) -> Result<ImageEmbeddingEvidence, InferRuntimeClientError> {
        validate_request(image, media_type, source_revision)?;
        let response: RawImageEmbeddingResponse =
            self.send_json(IMAGE_EMBEDDING_PATH, |endpoint, consumer_version| {
                let form: multipart::Form = image_form(
                    image_embedding_intent(consumer_version),
                    image,
                    media_type,
                    source_revision,
                )?
                .text("image_orientation", NORMALIZED_DISPLAY_ORIENTATION)
                .text("infer.priority", priority.as_str());
                Ok(self
                    .client
                    .post(endpoint)
                    .bearer_auth(self.credential.expose())
                    .multipart(form))
            })?;
        response.validate(source_revision)
    }

    fn embed_text_semantics(
        &self,
        text: &str,
        query_revision: &str,
        language: Option<&str>,
        priority: SemanticRequestPriority,
    ) -> Result<TextEmbeddingEvidence, InferRuntimeClientError> {
        validate_text_request(text, query_revision, language)?;
        let response: RawTextEmbeddingResponse =
            self.send_json(TEXT_EMBEDDING_PATH, |endpoint, consumer_version| {
                let request = TextEmbeddingRequest {
                    model: text_embedding_intent(consumer_version),
                    text,
                    query_revision,
                    language,
                    metadata: BTreeMap::from([("infer.priority", priority.as_str())]),
                };
                Ok(self
                    .client
                    .post(endpoint)
                    .bearer_auth(self.credential.expose())
                    .json(&request))
            })?;
        response.validate(query_revision, language)
    }
}

const fn image_embedding_intent(version: InferRuntimeConsumerVersion) -> &'static str {
    match version {
        InferRuntimeConsumerVersion::Candidate3 => "semantic.embed_image",
    }
}

const fn text_embedding_intent(version: InferRuntimeConsumerVersion) -> &'static str {
    match version {
        InferRuntimeConsumerVersion::Candidate3 => "semantic.embed_text",
    }
}

#[derive(Serialize)]
struct TextEmbeddingRequest<'a> {
    model: &'static str,
    text: &'a str,
    query_revision: &'a str,
    #[serde(skip_serializing_if = "Option::is_none")]
    language: Option<&'a str>,
    metadata: BTreeMap<&'static str, &'static str>,
}

#[derive(Deserialize)]
struct RawImageEmbeddingResponse {
    object: String,
    status: String,
    source_revision: String,
    image: RawImageGeometry,
    embedding: RawSemanticEmbedding,
    provenance: VisionProvenance,
}

impl RawImageEmbeddingResponse {
    fn validate(
        self,
        expected_source_revision: &str,
    ) -> Result<ImageEmbeddingEvidence, InferRuntimeClientError> {
        if self.object != "vision.image_embedding"
            || self.status != "completed"
            || self.source_revision != expected_source_revision
            || self.image.orientation != NORMALIZED_DISPLAY_ORIENTATION
            || self.image.width == 0
            || self.image.height == 0
            || !valid_provenance(&self.provenance)
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "image embedding response violated the typed contract",
            ));
        }
        Ok(ImageEmbeddingEvidence {
            source_revision: self.source_revision,
            width: self.image.width,
            height: self.image.height,
            orientation: self.image.orientation,
            embedding: self.embedding.admit()?,
            provenance: self.provenance,
        })
    }
}

#[derive(Deserialize)]
struct RawTextEmbeddingResponse {
    object: String,
    status: String,
    query_revision: String,
    language: Option<String>,
    embedding: RawSemanticEmbedding,
    provenance: VisionProvenance,
}

impl RawTextEmbeddingResponse {
    fn validate(
        self,
        expected_query_revision: &str,
        expected_language: Option<&str>,
    ) -> Result<TextEmbeddingEvidence, InferRuntimeClientError> {
        if self.object != "vision.text_embedding"
            || self.status != "completed"
            || self.query_revision != expected_query_revision
            || self.language.as_deref() != expected_language
            || !valid_provenance(&self.provenance)
            || self.provenance.tokenizer.as_ref().is_none_or(|tokenizer| {
                tokenizer.identity.trim().is_empty()
                    || tokenizer.artifact_sha256.trim().is_empty()
                    || tokenizer.max_length == 0
            })
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "text embedding response violated the typed contract",
            ));
        }
        Ok(TextEmbeddingEvidence {
            query_revision: self.query_revision,
            language: self.language,
            embedding: self.embedding.admit()?,
            provenance: self.provenance,
        })
    }
}

#[derive(Deserialize)]
struct RawSemanticEmbedding {
    values: Vec<f32>,
    dimensions: usize,
    normalized: bool,
    distance_metric: String,
    space: String,
}

impl RawSemanticEmbedding {
    fn admit(self) -> Result<SemanticEmbedding, InferRuntimeClientError> {
        if self.dimensions != SIGLIP_EMBEDDING_DIMENSIONS
            || !self.normalized
            || self.distance_metric != "cosine"
            || self.values.len() != self.dimensions
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "semantic embedding metadata violated the typed contract",
            ));
        }
        let space = SemanticEmbeddingSpace::new(
            SEMANTIC_EMBEDDING_CONTRACT_VERSION,
            self.space,
            self.dimensions,
        )
        .map_err(|_| {
            InferRuntimeClientError::InvalidResponse(
                "semantic embedding space violated the typed contract",
            )
        })?;
        SemanticEmbedding::new(space, self.values).map_err(|_| {
            InferRuntimeClientError::InvalidResponse(
                "semantic embedding values violated the typed contract",
            )
        })
    }
}

fn validate_text_request(
    text: &str,
    query_revision: &str,
    language: Option<&str>,
) -> Result<(), InferRuntimeClientError> {
    if text.trim().is_empty() || text.len() > MAX_TEXT_BYTES {
        return Err(InferRuntimeClientError::InvalidSemanticTextLength(
            text.len(),
        ));
    }
    if query_revision.trim().is_empty() || query_revision.len() > MAX_REVISION_BYTES {
        return Err(InferRuntimeClientError::InvalidQueryRevision);
    }
    if language.is_some_and(|language| {
        language.is_empty()
            || language.len() > MAX_LANGUAGE_BYTES
            || !language
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
    }) {
        return Err(InferRuntimeClientError::InvalidLanguage);
    }
    Ok(())
}

#[cfg(test)]
mod tests;
