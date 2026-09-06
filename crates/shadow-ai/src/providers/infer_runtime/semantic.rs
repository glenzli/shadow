//! Strict `SigLIP` product evidence over the official typed vision SDK.

use crate::{SEMANTIC_EMBEDDING_CONTRACT_VERSION, SemanticEmbedding, SemanticEmbeddingSpace};

use super::{
    InferRuntimeClient, InferRuntimeClientError, VisionProvenance, admit_vision_provenance,
    local_metadata, malformed,
};

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
    /// Returns SDK transport/contract failures or Shadow evidence rejection.
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
    /// Returns SDK transport/contract failures or Shadow evidence rejection.
    fn embed_text_semantics(
        &self,
        text: &str,
        query_revision: &str,
        language: Option<&str>,
        priority: SemanticRequestPriority,
    ) -> Result<TextEmbeddingEvidence, InferRuntimeClientError>;
    /// Encodes an image with cancellation.
    ///
    /// # Errors
    ///
    /// Provider errors are preserved.
    fn embed_image_semantics_cancellable(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        priority: SemanticRequestPriority,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<ImageEmbeddingEvidence>, InferRuntimeClientError> {
        if cancelled() {
            return Ok(None);
        }
        let value = self.embed_image_semantics(image, media_type, source_revision, priority)?;
        Ok((!cancelled()).then_some(value))
    }
    /// Encodes text with cancellation.
    ///
    /// # Errors
    ///
    /// Provider errors are preserved.
    fn embed_text_semantics_cancellable(
        &self,
        text: &str,
        query_revision: &str,
        language: Option<&str>,
        priority: SemanticRequestPriority,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<TextEmbeddingEvidence>, InferRuntimeClientError> {
        if cancelled() {
            return Ok(None);
        }
        let value = self.embed_text_semantics(text, query_revision, language, priority)?;
        Ok((!cancelled()).then_some(value))
    }
}

impl SemanticEmbeddingProvider for InferRuntimeClient {
    fn embed_image_semantics(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        priority: SemanticRequestPriority,
    ) -> Result<ImageEmbeddingEvidence, InferRuntimeClientError> {
        self.embed_image_semantics_cancellable(
            image,
            media_type,
            source_revision,
            priority,
            &|| false,
        )?
        .ok_or_else(|| InferRuntimeClientError::Input("semantic image cancelled".into()))
    }
    fn embed_text_semantics(
        &self,
        text: &str,
        query_revision: &str,
        language: Option<&str>,
        priority: SemanticRequestPriority,
    ) -> Result<TextEmbeddingEvidence, InferRuntimeClientError> {
        self.embed_text_semantics_cancellable(text, query_revision, language, priority, &|| false)?
            .ok_or_else(|| InferRuntimeClientError::Input("semantic text cancelled".into()))
    }

    fn embed_image_semantics_cancellable(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        priority: SemanticRequestPriority,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<ImageEmbeddingEvidence>, InferRuntimeClientError> {
        let (staged, media_type) = Self::stage_image(image, media_type, source_revision)?;
        let metadata = local_metadata(priority.as_str(), None);
        let Some(response) = self.block_on_cancellable(
            self.sdk()
                .embed_image(staged.path(), media_type, source_revision, &metadata),
            cancelled,
        )?
        else {
            return Ok(None);
        };
        if response.object != "vision.image_embedding"
            || response.status != "completed"
            || response.source_revision != source_revision
            || response.image.orientation != NORMALIZED_DISPLAY_ORIENTATION
            || response.image.width == 0
            || response.image.height == 0
        {
            return malformed("image embedding response violated Shadow's typed evidence contract");
        }
        Ok(Some(ImageEmbeddingEvidence {
            source_revision: response.source_revision,
            width: response.image.width,
            height: response.image.height,
            orientation: response.image.orientation,
            embedding: admit_embedding(response.embedding)?,
            provenance: admit_vision_provenance(response.provenance)?,
        }))
    }

    fn embed_text_semantics_cancellable(
        &self,
        text: &str,
        query_revision: &str,
        language: Option<&str>,
        priority: SemanticRequestPriority,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<TextEmbeddingEvidence>, InferRuntimeClientError> {
        validate_text_request(text, query_revision, language)?;
        let request = infer_runtime_client::TextEmbeddingRequest {
            model: "semantic.embed_text".into(),
            text: text.into(),
            query_revision: query_revision.into(),
            language: language.map(str::to_owned),
            metadata: local_metadata(priority.as_str(), None),
        };
        let Some(response) =
            self.block_on_cancellable(self.sdk().embed_text(&request), cancelled)?
        else {
            return Ok(None);
        };
        if response.object != "vision.text_embedding"
            || response.status != "completed"
            || response.query_revision != query_revision
            || response.language.as_deref() != language
        {
            return malformed("text embedding response violated Shadow's typed evidence contract");
        }
        let provenance = admit_vision_provenance(response.provenance)?;
        if provenance.tokenizer.as_ref().is_none_or(|tokenizer| {
            tokenizer.identity.trim().is_empty()
                || tokenizer.artifact_sha256.trim().is_empty()
                || tokenizer.max_length == 0
        }) {
            return malformed("text embedding tokenizer provenance is incomplete");
        }
        Ok(Some(TextEmbeddingEvidence {
            query_revision: response.query_revision,
            language: response.language,
            embedding: admit_embedding(response.embedding)?,
            provenance,
        }))
    }
}

fn admit_embedding(
    embedding: infer_runtime_client::SemanticEmbeddingVector,
) -> Result<SemanticEmbedding, InferRuntimeClientError> {
    if embedding.dimensions != SIGLIP_EMBEDDING_DIMENSIONS
        || !embedding.normalized
        || embedding.distance_metric != "cosine"
        || embedding.values.len() != embedding.dimensions
    {
        return malformed("semantic embedding metadata violated Shadow's typed contract");
    }
    let space = SemanticEmbeddingSpace::new(
        SEMANTIC_EMBEDDING_CONTRACT_VERSION,
        embedding.space,
        embedding.dimensions,
    )
    .map_err(|_| {
        InferRuntimeClientError::MalformedResponse(
            "semantic embedding space violated Shadow's typed contract".into(),
        )
    })?;
    SemanticEmbedding::new(space, embedding.values).map_err(|_| {
        InferRuntimeClientError::MalformedResponse(
            "semantic embedding values violated Shadow's typed contract".into(),
        )
    })
}

fn validate_text_request(
    text: &str,
    query_revision: &str,
    language: Option<&str>,
) -> Result<(), InferRuntimeClientError> {
    if text.trim().is_empty() || text.len() > MAX_TEXT_BYTES {
        return Err(InferRuntimeClientError::Input(
            "semantic text must be non-empty and at most 4096 bytes".into(),
        ));
    }
    if query_revision.trim().is_empty() || query_revision.len() > MAX_REVISION_BYTES {
        return Err(InferRuntimeClientError::Input(
            "semantic query revision must be non-empty and at most 256 bytes".into(),
        ));
    }
    if language.is_some_and(|language| {
        language.is_empty()
            || language.len() > MAX_LANGUAGE_BYTES
            || !language
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
    }) {
        return Err(InferRuntimeClientError::Input(
            "semantic language is not a bounded BCP-47-shaped tag".into(),
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests;
