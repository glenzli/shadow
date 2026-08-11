//! Provider-neutral boundary for structured local image understanding.
//!
//! Runtime wire adaptation lives here, while Library admission, checkpointing,
//! keyword acceptance, and stale-result arbitration remain Consumer-owned.

use std::collections::BTreeSet;

use reqwest::blocking::multipart;
use serde::{Deserialize, Serialize};

use crate::{
    SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION, SemanticEvidenceKind, SemanticImageAnalysis,
    SemanticKeywordKind, SemanticKeywordSuggestion, SemanticShortCaption,
};

use super::{
    InferRuntimeClient, InferRuntimeClientError, RawImageGeometry, SemanticRequestPriority,
    discovery::InferRuntimeConsumerVersion, image_form, validate_request,
};

const IMAGE_DESCRIPTION_PATH: &str = "infer/v1/vision/image-descriptions";
const CLASSIFICATION_REVIEW_PATH: &str = "infer/v1/vision/classification-reviews";
const NORMALIZED_DISPLAY_ORIENTATION: &str = "display_pixels_orientation_normalized";
const OLLAMA_NATIVE_RUNTIME: &str = "ollama_native_chat";
const MAX_LANGUAGE_BYTES: usize = 35;
const MAX_TAXONOMY_REVISION_BYTES: usize = 256;
const MAX_CATEGORIES: usize = 64;
const MAX_CATEGORIES_JSON_BYTES: usize = 65_536;
const MAX_CATEGORY_ID_BYTES: usize = 128;
const MAX_CATEGORY_NAME_BYTES: usize = 256;
const MAX_CATEGORY_DESCRIPTION_BYTES: usize = 1_024;
const MAX_DESCRIPTION_BYTES: usize = 1_024;
const MAX_KEYWORD_SUGGESTIONS: usize = 16;
const MAX_KEYWORD_BYTES: usize = 128;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ImageUnderstandingQuality {
    /// Bulk/background path intended for the smaller resident model.
    Basic,
    /// Explicit review path allowed to request the larger local model.
    General,
}

impl ImageUnderstandingQuality {
    const fn capability_floor(self, version: InferRuntimeConsumerVersion) -> &'static str {
        match (version, self) {
            (InferRuntimeConsumerVersion::Candidate2, Self::Basic) => "basic",
            (InferRuntimeConsumerVersion::Candidate2, Self::General) => "general",
            (InferRuntimeConsumerVersion::Candidate3, Self::Basic) => "foundational",
            (InferRuntimeConsumerVersion::Candidate3, Self::General) => "capable",
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ImageUnderstandingEvidence {
    pub source_revision: String,
    pub width: u32,
    pub height: u32,
    pub orientation: String,
    pub analysis: SemanticImageAnalysis,
    pub provenance: ImageUnderstandingProvenance,
}

/// Exact Runtime identity for Qwen-native structured image understanding.
///
/// This is deliberately separate from the ONNX-oriented `VisionProvenance`:
/// the execution provider, graph, and tokenizer fields do not describe an
/// Ollama-native chat invocation.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ImageUnderstandingProvenance {
    pub job_id: String,
    pub provider: String,
    pub deployment: String,
    pub model_profile: String,
    pub model_build: String,
    pub physical_model: String,
    pub runtime: String,
    pub schema_revision: String,
    pub prompt_revision: String,
    pub total_duration_ms: Option<u64>,
    pub load_duration_ms: Option<u64>,
    pub prompt_eval_count: Option<u64>,
    pub eval_count: Option<u64>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ClassificationReviewCategory {
    pub id: String,
    pub name: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub description: Option<String>,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ClassificationReviewDisposition {
    Matched,
    None,
    Uncertain,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ClassificationReviewSuggestion {
    pub disposition: ClassificationReviewDisposition,
    pub category_id: Option<String>,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ClassificationReviewEvidence {
    pub source_revision: String,
    pub taxonomy_revision: String,
    pub width: u32,
    pub height: u32,
    pub orientation: String,
    pub suggestion: ClassificationReviewSuggestion,
    pub provenance: ImageUnderstandingProvenance,
}

#[derive(Debug, Clone, Copy)]
pub struct ClassificationReviewRequest<'a> {
    pub image: &'a [u8],
    pub media_type: &'a str,
    pub source_revision: &'a str,
    pub taxonomy_revision: &'a str,
    pub categories: &'a [ClassificationReviewCategory],
    pub quality: ImageUnderstandingQuality,
    pub priority: SemanticRequestPriority,
}

pub trait ImageUnderstandingProvider {
    /// Produces one bounded structured proposal from orientation-normalized
    /// display JPEG bytes. No result from this method is a user-authored fact.
    ///
    /// # Errors
    ///
    /// Returns a fail-closed local transport or typed-contract error.
    fn describe_image(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        language: &str,
        quality: ImageUnderstandingQuality,
        priority: SemanticRequestPriority,
    ) -> Result<ImageUnderstandingEvidence, InferRuntimeClientError>;
}

pub trait ClassificationReviewProvider {
    /// Reviews one image against only the supplied category set.
    ///
    /// The result remains assistant evidence. A Consumer must not persist it
    /// as user feedback until the user explicitly accepts or corrects it.
    ///
    /// # Errors
    ///
    /// Returns a fail-closed local transport or typed-contract error.
    fn review_classification(
        &self,
        request: &ClassificationReviewRequest<'_>,
    ) -> Result<ClassificationReviewEvidence, InferRuntimeClientError>;
}

impl ImageUnderstandingProvider for InferRuntimeClient {
    fn describe_image(
        &self,
        image: &[u8],
        media_type: &str,
        source_revision: &str,
        language: &str,
        quality: ImageUnderstandingQuality,
        priority: SemanticRequestPriority,
    ) -> Result<ImageUnderstandingEvidence, InferRuntimeClientError> {
        validate_request(image, media_type, source_revision)?;
        validate_language(language)?;
        let response: RawImageDescriptionResponse =
            self.send_json(IMAGE_DESCRIPTION_PATH, |endpoint, consumer_version| {
                let form = understanding_form(
                    "vision.describe_image",
                    image,
                    media_type,
                    source_revision,
                    quality,
                    priority,
                    consumer_version,
                )?
                .text("language", language.to_owned());
                Ok(self
                    .client
                    .post(endpoint)
                    .bearer_auth(self.credential.expose())
                    .multipart(form))
            })?;
        response.validate(source_revision, language)
    }
}

impl ClassificationReviewProvider for InferRuntimeClient {
    fn review_classification(
        &self,
        request: &ClassificationReviewRequest<'_>,
    ) -> Result<ClassificationReviewEvidence, InferRuntimeClientError> {
        validate_request(request.image, request.media_type, request.source_revision)?;
        validate_taxonomy(request.taxonomy_revision, request.categories)?;
        let categories_json = serde_json::to_string(request.categories)
            .map_err(InferRuntimeClientError::SerializeRequest)?;
        if categories_json.len() > MAX_CATEGORIES_JSON_BYTES {
            return Err(InferRuntimeClientError::InvalidClassificationCategories);
        }
        let response: RawClassificationReviewResponse =
            self.send_json(CLASSIFICATION_REVIEW_PATH, |endpoint, consumer_version| {
                let form = understanding_form(
                    classification_intent(consumer_version),
                    request.image,
                    request.media_type,
                    request.source_revision,
                    request.quality,
                    request.priority,
                    consumer_version,
                )?
                .text("taxonomy_revision", request.taxonomy_revision.to_owned())
                .text("categories", categories_json.clone());
                Ok(self
                    .client
                    .post(endpoint)
                    .bearer_auth(self.credential.expose())
                    .multipart(form))
            })?;
        response.validate(
            request.source_revision,
            request.taxonomy_revision,
            request.categories,
        )
    }
}

fn understanding_form(
    model: &'static str,
    image: &[u8],
    media_type: &str,
    source_revision: &str,
    quality: ImageUnderstandingQuality,
    priority: SemanticRequestPriority,
    consumer_version: InferRuntimeConsumerVersion,
) -> Result<multipart::Form, InferRuntimeClientError> {
    let (capability_key, capability_floor) = capability_metadata(consumer_version, quality);
    Ok(image_form(model, image, media_type, source_revision)?
        .text("image_orientation", NORMALIZED_DISPLAY_ORIENTATION)
        .text("infer.priority", priority.as_str())
        .text(capability_key, capability_floor)
        .text("infer.placement", "local_only")
        .text("infer.offline_required", "true")
        .text("infer.fallback", "none"))
}

const fn capability_metadata(
    version: InferRuntimeConsumerVersion,
    quality: ImageUnderstandingQuality,
) -> (&'static str, &'static str) {
    let key = match version {
        InferRuntimeConsumerVersion::Candidate2 => "infer.quality_floor",
        InferRuntimeConsumerVersion::Candidate3 => "infer.capability_floor",
    };
    (key, quality.capability_floor(version))
}

const fn classification_intent(version: InferRuntimeConsumerVersion) -> &'static str {
    match version {
        InferRuntimeConsumerVersion::Candidate2 => "vision.review_classification",
        InferRuntimeConsumerVersion::Candidate3 => "vision.classify_closed_set",
    }
}

#[derive(Deserialize)]
struct RawImageDescriptionResponse {
    object: String,
    status: String,
    source_revision: String,
    language: String,
    image: RawImageGeometry,
    result: RawImageDescriptionResult,
    provenance: ImageUnderstandingProvenance,
}

#[derive(Deserialize)]
struct RawImageDescriptionResult {
    description: String,
    keyword_suggestions: Vec<String>,
}

impl RawImageDescriptionResponse {
    fn validate(
        self,
        expected_source_revision: &str,
        expected_language: &str,
    ) -> Result<ImageUnderstandingEvidence, InferRuntimeClientError> {
        if self.object != "vision.image_description"
            || self.status != "completed"
            || self.source_revision != expected_source_revision
            || self.language != expected_language
            || !valid_geometry(&self.image)
            || !valid_image_understanding_provenance(&self.provenance)
            || !valid_display_text(&self.result.description, MAX_DESCRIPTION_BYTES)
            || self.result.keyword_suggestions.len() > MAX_KEYWORD_SUGGESTIONS
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "image description response violated the typed contract",
            ));
        }
        let mut labels = BTreeSet::new();
        let mut suggestions = Vec::with_capacity(self.result.keyword_suggestions.len());
        for label in self.result.keyword_suggestions {
            if !valid_display_text(&label, MAX_KEYWORD_BYTES) {
                return Err(InferRuntimeClientError::InvalidResponse(
                    "image description keyword violated the typed contract",
                ));
            }
            let folded = label.to_lowercase();
            if !labels.insert(folded.clone()) {
                return Err(InferRuntimeClientError::InvalidResponse(
                    "image description keywords were not unique",
                ));
            }
            suggestions.push(SemanticKeywordSuggestion {
                kind: SemanticKeywordKind::Concept,
                concept_id: stable_revision("qwen.keyword", &folded),
                display_label: label,
                evidence: SemanticEvidenceKind::ModelInterpretation,
            });
        }
        let analysis = SemanticImageAnalysis::new(
            SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION,
            stable_revision(
                "qwen.analyzer",
                &format!(
                    "{}\0{}",
                    self.provenance.model_build, self.provenance.schema_revision
                ),
            ),
            stable_revision("qwen.prompt", &self.provenance.prompt_revision),
            suggestions,
            SemanticShortCaption {
                language_tag: self.language,
                text: self.result.description,
            },
        )
        .map_err(|_| {
            InferRuntimeClientError::InvalidResponse(
                "image description analysis violated the semantic contract",
            )
        })?;
        Ok(ImageUnderstandingEvidence {
            source_revision: self.source_revision,
            width: self.image.width,
            height: self.image.height,
            orientation: self.image.orientation,
            analysis,
            provenance: self.provenance,
        })
    }
}

#[derive(Deserialize)]
struct RawClassificationReviewResponse {
    object: String,
    status: String,
    source_revision: String,
    taxonomy_revision: String,
    image: RawImageGeometry,
    suggestion: RawClassificationReviewSuggestion,
    provenance: ImageUnderstandingProvenance,
}

#[derive(Deserialize)]
struct RawClassificationReviewSuggestion {
    disposition: String,
    category_id: Option<String>,
}

impl RawClassificationReviewResponse {
    fn validate(
        self,
        expected_source_revision: &str,
        expected_taxonomy_revision: &str,
        categories: &[ClassificationReviewCategory],
    ) -> Result<ClassificationReviewEvidence, InferRuntimeClientError> {
        if self.object != "vision.classification_review"
            || self.status != "completed"
            || self.source_revision != expected_source_revision
            || self.taxonomy_revision != expected_taxonomy_revision
            || !valid_geometry(&self.image)
            || !valid_image_understanding_provenance(&self.provenance)
        {
            return Err(InferRuntimeClientError::InvalidResponse(
                "classification review response violated the typed contract",
            ));
        }
        let disposition = match (
            self.suggestion.disposition.as_str(),
            self.suggestion.category_id.as_deref(),
        ) {
            ("matched", Some(category_id))
                if categories.iter().any(|category| category.id == category_id) =>
            {
                ClassificationReviewDisposition::Matched
            }
            ("none", None) => ClassificationReviewDisposition::None,
            ("uncertain", None) => ClassificationReviewDisposition::Uncertain,
            _ => {
                return Err(InferRuntimeClientError::InvalidResponse(
                    "classification review suggestion escaped the submitted taxonomy",
                ));
            }
        };
        Ok(ClassificationReviewEvidence {
            source_revision: self.source_revision,
            taxonomy_revision: self.taxonomy_revision,
            width: self.image.width,
            height: self.image.height,
            orientation: self.image.orientation,
            suggestion: ClassificationReviewSuggestion {
                disposition,
                category_id: self.suggestion.category_id,
            },
            provenance: self.provenance,
        })
    }
}

fn validate_language(language: &str) -> Result<(), InferRuntimeClientError> {
    if language.is_empty()
        || language.len() > MAX_LANGUAGE_BYTES
        || !language
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
    {
        return Err(InferRuntimeClientError::InvalidLanguage);
    }
    Ok(())
}

fn validate_taxonomy(
    taxonomy_revision: &str,
    categories: &[ClassificationReviewCategory],
) -> Result<(), InferRuntimeClientError> {
    if !valid_bounded_text(taxonomy_revision, MAX_TAXONOMY_REVISION_BYTES)
        || !(1..=MAX_CATEGORIES).contains(&categories.len())
    {
        return Err(InferRuntimeClientError::InvalidClassificationCategories);
    }
    let mut ids = BTreeSet::new();
    if categories.iter().any(|category| {
        !valid_bounded_text(&category.id, MAX_CATEGORY_ID_BYTES)
            || !valid_display_text(&category.name, MAX_CATEGORY_NAME_BYTES)
            || category.description.as_ref().is_some_and(|description| {
                !valid_display_text(description, MAX_CATEGORY_DESCRIPTION_BYTES)
            })
            || !ids.insert(category.id.as_str())
    }) {
        return Err(InferRuntimeClientError::InvalidClassificationCategories);
    }
    Ok(())
}

fn valid_geometry(image: &RawImageGeometry) -> bool {
    image.width > 0 && image.height > 0 && image.orientation == NORMALIZED_DISPLAY_ORIENTATION
}

fn valid_image_understanding_provenance(provenance: &ImageUnderstandingProvenance) -> bool {
    [
        provenance.job_id.as_str(),
        provenance.provider.as_str(),
        provenance.deployment.as_str(),
        provenance.model_profile.as_str(),
        provenance.model_build.as_str(),
        provenance.physical_model.as_str(),
        provenance.schema_revision.as_str(),
        provenance.prompt_revision.as_str(),
    ]
    .iter()
    .all(|value| valid_bounded_text(value, 1_024))
        && provenance.runtime == OLLAMA_NATIVE_RUNTIME
}

fn valid_bounded_text(value: &str, maximum_bytes: usize) -> bool {
    !value.is_empty() && value.len() <= maximum_bytes && !value.chars().any(char::is_control)
}

fn valid_display_text(value: &str, maximum_bytes: usize) -> bool {
    valid_bounded_text(value, maximum_bytes) && value == value.trim()
}

fn stable_revision(namespace: &str, value: &str) -> String {
    format!("{namespace}:{}", blake3::hash(value.as_bytes()).to_hex())
}

#[cfg(test)]
mod tests;
