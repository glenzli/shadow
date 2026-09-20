//! Provider-neutral Qwen evidence over the official typed vision SDK.

use std::collections::BTreeSet;

use serde::{Deserialize, Serialize};

use crate::{
    SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION, SemanticEvidenceKind, SemanticImageAnalysis,
    SemanticKeywordKind, SemanticKeywordSuggestion, SemanticShortCaption,
};

use super::{
    InferRuntimeClient, InferRuntimeClientError, SemanticRequestPriority, local_metadata, malformed,
};

const NORMALIZED_DISPLAY_ORIENTATION: &str = "display_pixels_orientation_normalized";
const OLLAMA_NATIVE_RUNTIME: &str = "ollama_native_chat";
const MAX_LANGUAGE_BYTES: usize = 35;
const MAX_TAXONOMY_REVISION_BYTES: usize = 256;
const MAX_CATEGORIES: usize = 64;
const MAX_CATEGORY_ID_BYTES: usize = 128;
const MAX_CATEGORY_NAME_BYTES: usize = 256;
const MAX_CATEGORY_DESCRIPTION_BYTES: usize = 1_024;
const MAX_DESCRIPTION_BYTES: usize = 1_024;
const MAX_KEYWORD_SUGGESTIONS: usize = 16;
const MAX_KEYWORD_BYTES: usize = 128;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum ImageUnderstandingQuality {
    Basic,
    General,
}

impl ImageUnderstandingQuality {
    const fn capability_floor(self) -> &'static str {
        match self {
            Self::Basic => "foundational",
            Self::General => "capable",
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
    /// Produces bounded model evidence for one normalized display image.
    ///
    /// # Errors
    ///
    /// Returns SDK transport/contract failures or Shadow evidence rejection.
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
    /// Reviews one image without allowing suggestions outside the taxonomy.
    ///
    /// # Errors
    ///
    /// Returns SDK transport/contract failures or Shadow evidence rejection.
    fn review_classification(
        &self,
        request: &ClassificationReviewRequest<'_>,
    ) -> Result<ClassificationReviewEvidence, InferRuntimeClientError>;
}

impl InferRuntimeClient {
    /// Local, bounded image evidence with prompt-lifecycle cancellation.
    ///
    /// # Errors
    /// Returns SDK transport failures or rejected semantic evidence.
    pub fn describe_image_cancellable(
        &self,
        image: &[u8],
        source_revision: &str,
        cancellation: &crate::CancellationToken,
    ) -> Result<Option<ImageUnderstandingEvidence>, InferRuntimeClientError> {
        if cancellation.is_cancelled() {
            return Ok(None);
        }
        let (staged, media_type) = Self::stage_image(image, "image/jpeg", source_revision)?;
        let metadata = local_metadata("interactive", Some("foundational"));
        let request =
            self.sdk()
                .describe_image(staged.path(), media_type, source_revision, "en", &metadata);
        let response = self.runtime.block_on(async {
            tokio::select! {
                response = request => response.map(Some),
                () = async {
                    while !cancellation.is_cancelled() {
                        tokio::time::sleep(std::time::Duration::from_millis(25)).await;
                    }
                } => Ok(None),
            }
        })?;
        response
            .map(|response| admit_description(response, source_revision, "en"))
            .transpose()
    }
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
        validate_language(language)?;
        let (staged, media_type) = Self::stage_image(image, media_type, source_revision)?;
        let metadata = local_metadata(priority.as_str(), Some(quality.capability_floor()));
        let response = self.block_on(self.sdk().describe_image(
            staged.path(),
            media_type,
            source_revision,
            language,
            &metadata,
        ))?;
        admit_description(response, source_revision, language)
    }
}

impl ClassificationReviewProvider for InferRuntimeClient {
    fn review_classification(
        &self,
        request: &ClassificationReviewRequest<'_>,
    ) -> Result<ClassificationReviewEvidence, InferRuntimeClientError> {
        validate_taxonomy(request.taxonomy_revision, request.categories)?;
        let (staged, media_type) =
            Self::stage_image(request.image, request.media_type, request.source_revision)?;
        let categories = request
            .categories
            .iter()
            .map(|category| infer_runtime_client::ClassificationCategory {
                id: category.id.clone(),
                name: category.name.clone(),
                description: category.description.clone(),
            })
            .collect::<Vec<_>>();
        let metadata = local_metadata(
            request.priority.as_str(),
            Some(request.quality.capability_floor()),
        );
        let response = self.block_on(self.sdk().review_classification(
            staged.path(),
            media_type,
            request.source_revision,
            request.taxonomy_revision,
            &categories,
            &metadata,
        ))?;
        admit_classification(
            response,
            request.source_revision,
            request.taxonomy_revision,
            request.categories,
        )
    }
}

fn admit_description(
    response: infer_runtime_client::ImageDescriptionResponse,
    expected_source_revision: &str,
    expected_language: &str,
) -> Result<ImageUnderstandingEvidence, InferRuntimeClientError> {
    let provenance = image_understanding_provenance(response.provenance);
    if response.object != "vision.image_description"
        || response.status != "completed"
        || response.source_revision != expected_source_revision
        || response.language != expected_language
        || !valid_geometry(
            response.image.width,
            response.image.height,
            &response.image.orientation,
        )
        || !valid_image_understanding_provenance(&provenance)
        || !valid_display_text(&response.result.description, MAX_DESCRIPTION_BYTES)
        || response.result.keyword_suggestions.len() > MAX_KEYWORD_SUGGESTIONS
    {
        return malformed(
            "image description response violated Shadow's semantic evidence contract",
        );
    }
    let mut labels = BTreeSet::new();
    let mut suggestions = Vec::with_capacity(response.result.keyword_suggestions.len());
    for label in response.result.keyword_suggestions {
        if !valid_display_text(&label, MAX_KEYWORD_BYTES) {
            return malformed("image description keyword violated Shadow's semantic contract");
        }
        let folded = label.to_lowercase();
        if !labels.insert(folded.clone()) {
            return malformed("image description keywords were not unique");
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
            &format!("{}\0{}", provenance.model_build, provenance.schema_revision),
        ),
        stable_revision("qwen.prompt", &provenance.prompt_revision),
        suggestions,
        SemanticShortCaption {
            language_tag: response.language,
            text: response.result.description,
        },
    )
    .map_err(|_| {
        InferRuntimeClientError::MalformedResponse(
            "image description analysis violated Shadow's semantic contract".into(),
        )
    })?;
    Ok(ImageUnderstandingEvidence {
        source_revision: response.source_revision,
        width: response.image.width,
        height: response.image.height,
        orientation: response.image.orientation,
        analysis,
        provenance,
    })
}

fn admit_classification(
    response: infer_runtime_client::ClassificationReviewResponse,
    expected_source_revision: &str,
    expected_taxonomy_revision: &str,
    categories: &[ClassificationReviewCategory],
) -> Result<ClassificationReviewEvidence, InferRuntimeClientError> {
    let provenance = image_understanding_provenance(response.provenance);
    if response.object != "vision.classification_review"
        || response.status != "completed"
        || response.source_revision != expected_source_revision
        || response.taxonomy_revision != expected_taxonomy_revision
        || !valid_geometry(
            response.image.width,
            response.image.height,
            &response.image.orientation,
        )
        || !valid_image_understanding_provenance(&provenance)
    {
        return malformed(
            "classification review response violated Shadow's typed evidence contract",
        );
    }
    let (disposition, category_id) = match (
        response.suggestion.disposition,
        response.suggestion.category_id,
    ) {
        (infer_runtime_client::ClassificationDisposition::Matched, Some(category_id))
            if categories.iter().any(|category| category.id == category_id) =>
        {
            (ClassificationReviewDisposition::Matched, Some(category_id))
        }
        (infer_runtime_client::ClassificationDisposition::None, None) => {
            (ClassificationReviewDisposition::None, None)
        }
        (infer_runtime_client::ClassificationDisposition::Uncertain, None) => {
            (ClassificationReviewDisposition::Uncertain, None)
        }
        _ => return malformed("classification review escaped the submitted taxonomy"),
    };
    Ok(ClassificationReviewEvidence {
        source_revision: response.source_revision,
        taxonomy_revision: response.taxonomy_revision,
        width: response.image.width,
        height: response.image.height,
        orientation: response.image.orientation,
        suggestion: ClassificationReviewSuggestion {
            disposition,
            category_id,
        },
        provenance,
    })
}

fn image_understanding_provenance(
    value: infer_runtime_client::ImageUnderstandingProvenance,
) -> ImageUnderstandingProvenance {
    ImageUnderstandingProvenance {
        job_id: value.job_id,
        provider: value.provider,
        deployment: value.deployment,
        model_profile: value.model_profile,
        model_build: value.model_build,
        physical_model: value.physical_model,
        runtime: value.runtime,
        schema_revision: value.schema_revision,
        prompt_revision: value.prompt_revision,
        total_duration_ms: value.total_duration_ms,
        load_duration_ms: value.load_duration_ms,
        prompt_eval_count: value.prompt_eval_count,
        eval_count: value.eval_count,
    }
}

fn validate_language(language: &str) -> Result<(), InferRuntimeClientError> {
    if language.is_empty()
        || language.len() > MAX_LANGUAGE_BYTES
        || !language
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
    {
        return Err(InferRuntimeClientError::Input(
            "image-understanding language is not a bounded BCP-47-shaped tag".into(),
        ));
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
        return Err(InferRuntimeClientError::Input(
            "classification taxonomy or category set is invalid".into(),
        ));
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
        return Err(InferRuntimeClientError::Input(
            "classification taxonomy or category set is invalid".into(),
        ));
    }
    Ok(())
}

fn valid_geometry(width: u32, height: u32, orientation: &str) -> bool {
    width > 0 && height > 0 && orientation == NORMALIZED_DISPLAY_ORIENTATION
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
