use std::collections::BTreeSet;

use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::{AiCapability, BackendKind, NumericPrecision};

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ModelFormat {
    Onnx,
    Safetensors,
    Gguf,
    ExternalProvider,
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind", content = "detail")]
pub enum Quantization {
    None,
    Float16,
    Int8,
    Int4,
    Mixed,
    Other(String),
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ElementType {
    Float32,
    Float16,
    Uint8,
    Int8,
    Int32,
    Int64,
    Bool,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TensorLayout {
    Nchw,
    Nhwc,
    Nc,
    Hw,
    Chw,
    Custom,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TensorSemantics {
    Image,
    ImageEmbedding,
    TextEmbedding,
    QualitySignals,
    RecipeParameters,
    Mask,
    Depth,
    InpaintPatch,
    TokenIds,
    AttentionMask,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case", tag = "kind")]
pub enum Dimension {
    Fixed {
        value: u32,
    },
    Dynamic {
        name: String,
        minimum: u32,
        maximum: Option<u32>,
    },
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct NumericRange {
    pub minimum: f64,
    pub maximum: f64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TensorContract {
    pub name: String,
    pub semantics: TensorSemantics,
    pub element_type: ElementType,
    pub layout: TensorLayout,
    pub shape: Vec<Dimension>,
    /// Explicit names such as `display_srgb`, `scene_linear_rec2020`, or `none`.
    pub color_space: Option<String>,
    pub numeric_range: Option<NumericRange>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct ModelArtifact {
    pub filename: String,
    pub byte_len: u64,
    pub sha256: String,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct LicenseTerms {
    pub code_license: String,
    pub weight_license: String,
    pub training_data_notes: Option<String>,
    pub redistribution: LicensePermission,
    pub commercial_use: LicensePermission,
    pub access: ModelAccess,
    pub attribution_files: Vec<String>,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LicensePermission {
    Allowed,
    Prohibited,
    Unclear,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ModelAccess {
    Open,
    Gated,
    GatedWithAcceptance,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct DistributionTerms {
    pub bundled_by_default: bool,
    pub automatic_download_allowed: bool,
    pub side_load_allowed: bool,
    pub upstream_url: String,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct BackendRequirement {
    pub kind: BackendKind,
    pub minimum_runtime_version: Option<String>,
    /// Ordered best-first. The scheduler chooses the first supported precision.
    pub precisions: Vec<NumericPrecision>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ModelManifest {
    pub schema_version: u32,
    pub model_id: String,
    pub exact_revision: String,
    pub artifact: ModelArtifact,
    pub capabilities: BTreeSet<AiCapability>,
    pub format: ModelFormat,
    pub opset: Option<u32>,
    pub quantization: Quantization,
    pub preprocessing_version: String,
    pub inputs: Vec<TensorContract>,
    pub outputs: Vec<TensorContract>,
    /// Ordered best-first and intentionally supplied by data, not hard-coded by GPU name.
    pub execution_targets: Vec<BackendRequirement>,
    pub minimum_ram_bytes: u64,
    pub recommended_ram_bytes: u64,
    pub minimum_device_memory_bytes: u64,
    pub recommended_device_memory_bytes: u64,
    pub licensing: LicenseTerms,
    pub distribution: DistributionTerms,
}

impl ModelManifest {
    /// Validates the persisted, security-relevant parts of a model declaration.
    ///
    /// # Errors
    ///
    /// Returns the first structural or provenance error. It does not claim that
    /// the model is accurate or that a license interpretation is legally correct.
    pub fn validate(&self) -> Result<(), ModelManifestError> {
        require_text(&self.model_id, "model_id")?;
        require_text(&self.exact_revision, "exact_revision")?;
        require_text(&self.preprocessing_version, "preprocessing_version")?;
        require_text(&self.artifact.filename, "artifact.filename")?;
        require_text(&self.licensing.code_license, "licensing.code_license")?;
        require_text(&self.licensing.weight_license, "licensing.weight_license")?;
        require_text(&self.distribution.upstream_url, "distribution.upstream_url")?;

        if self.schema_version == 0 {
            return Err(ModelManifestError::InvalidSchemaVersion);
        }
        if self.artifact.byte_len == 0 && self.format != ModelFormat::ExternalProvider {
            return Err(ModelManifestError::EmptyArtifact);
        }
        validate_sha256(&self.artifact.sha256)?;
        if self.capabilities.is_empty() {
            return Err(ModelManifestError::MissingCapabilities);
        }
        if self.inputs.is_empty() {
            return Err(ModelManifestError::MissingInputs);
        }
        if self.outputs.is_empty() {
            return Err(ModelManifestError::MissingOutputs);
        }
        validate_tensors(&self.inputs, "input")?;
        validate_tensors(&self.outputs, "output")?;
        if self.execution_targets.is_empty() {
            return Err(ModelManifestError::MissingExecutionTargets);
        }
        for target in &self.execution_targets {
            if target.precisions.is_empty() {
                return Err(ModelManifestError::MissingBackendPrecision(target.kind));
            }
        }
        if self.format == ModelFormat::Onnx && self.opset.is_none() {
            return Err(ModelManifestError::MissingOnnxOpset);
        }
        if self.minimum_ram_bytes > self.recommended_ram_bytes {
            return Err(ModelManifestError::InvalidRamRange);
        }
        if self.minimum_device_memory_bytes > self.recommended_device_memory_bytes {
            return Err(ModelManifestError::InvalidDeviceMemoryRange);
        }
        if self.distribution.bundled_by_default
            && self.licensing.redistribution != LicensePermission::Allowed
        {
            return Err(ModelManifestError::BundledWithoutRedistributionPermission);
        }
        if self.distribution.automatic_download_allowed
            && self.licensing.access == ModelAccess::GatedWithAcceptance
        {
            return Err(ModelManifestError::AutomaticDownloadBypassesAcceptance);
        }
        Ok(())
    }
}

fn require_text(value: &str, field: &'static str) -> Result<(), ModelManifestError> {
    if value.trim().is_empty() {
        Err(ModelManifestError::MissingText(field))
    } else {
        Ok(())
    }
}

fn validate_sha256(value: &str) -> Result<(), ModelManifestError> {
    if value.len() != 64 || !value.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err(ModelManifestError::InvalidSha256);
    }
    Ok(())
}

fn validate_tensors(
    tensors: &[TensorContract],
    direction: &'static str,
) -> Result<(), ModelManifestError> {
    let mut names = BTreeSet::new();
    for tensor in tensors {
        require_text(&tensor.name, "tensor.name")?;
        if !names.insert(tensor.name.as_str()) {
            return Err(ModelManifestError::DuplicateTensorName {
                direction,
                name: tensor.name.clone(),
            });
        }
        if tensor.shape.is_empty() {
            return Err(ModelManifestError::EmptyTensorShape(tensor.name.clone()));
        }
        for dimension in &tensor.shape {
            match dimension {
                Dimension::Fixed { value: 0 } => {
                    return Err(ModelManifestError::InvalidTensorDimension(
                        tensor.name.clone(),
                    ));
                }
                Dimension::Dynamic {
                    name,
                    minimum,
                    maximum,
                } => {
                    if name.trim().is_empty()
                        || *minimum == 0
                        || maximum.is_some_and(|maximum| maximum < *minimum)
                    {
                        return Err(ModelManifestError::InvalidTensorDimension(
                            tensor.name.clone(),
                        ));
                    }
                }
                Dimension::Fixed { .. } => {}
            }
        }
        if let Some(range) = tensor.numeric_range
            && (!range.minimum.is_finite()
                || !range.maximum.is_finite()
                || range.minimum >= range.maximum)
        {
            return Err(ModelManifestError::InvalidNumericRange(tensor.name.clone()));
        }
    }
    Ok(())
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum ModelManifestError {
    #[error("manifest schema_version must be greater than zero")]
    InvalidSchemaVersion,
    #[error("manifest field {0} must not be empty")]
    MissingText(&'static str),
    #[error("non-provider model artifact must have a non-zero byte length")]
    EmptyArtifact,
    #[error("artifact sha256 must contain exactly 64 hexadecimal characters")]
    InvalidSha256,
    #[error("model manifest must declare at least one capability")]
    MissingCapabilities,
    #[error("model manifest must declare at least one input")]
    MissingInputs,
    #[error("model manifest must declare at least one output")]
    MissingOutputs,
    #[error("duplicate {direction} tensor name: {name}")]
    DuplicateTensorName {
        direction: &'static str,
        name: String,
    },
    #[error("tensor {0} must have at least one dimension")]
    EmptyTensorShape(String),
    #[error("tensor {0} has an invalid fixed or dynamic dimension")]
    InvalidTensorDimension(String),
    #[error("tensor {0} has a non-finite or inverted numeric range")]
    InvalidNumericRange(String),
    #[error("model manifest must declare at least one execution target")]
    MissingExecutionTargets,
    #[error("execution target {0:?} has no supported precision")]
    MissingBackendPrecision(BackendKind),
    #[error("ONNX manifests must pin an opset")]
    MissingOnnxOpset,
    #[error("minimum RAM exceeds recommended RAM")]
    InvalidRamRange,
    #[error("minimum device memory exceeds recommended device memory")]
    InvalidDeviceMemoryRange,
    #[error("model cannot be bundled when weight redistribution is not allowed")]
    BundledWithoutRedistributionPermission,
    #[error("automatic download cannot bypass acceptance for a gated model")]
    AutomaticDownloadBypassesAcceptance,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid_manifest() -> ModelManifest {
        ModelManifest {
            schema_version: 1,
            model_id: "example.embedding.small".into(),
            exact_revision: "r1".into(),
            artifact: ModelArtifact {
                filename: "model.onnx".into(),
                byte_len: 42,
                sha256: "a".repeat(64),
            },
            capabilities: BTreeSet::from([AiCapability::SimilarityEmbedding]),
            format: ModelFormat::Onnx,
            opset: Some(18),
            quantization: Quantization::Float16,
            preprocessing_version: "rgb-224-v1".into(),
            inputs: vec![TensorContract {
                name: "image".into(),
                semantics: TensorSemantics::Image,
                element_type: ElementType::Float32,
                layout: TensorLayout::Nchw,
                shape: vec![
                    Dimension::Fixed { value: 1 },
                    Dimension::Fixed { value: 3 },
                    Dimension::Fixed { value: 224 },
                    Dimension::Fixed { value: 224 },
                ],
                color_space: Some("display_srgb".into()),
                numeric_range: Some(NumericRange {
                    minimum: 0.0,
                    maximum: 1.0,
                }),
            }],
            outputs: vec![TensorContract {
                name: "embedding".into(),
                semantics: TensorSemantics::ImageEmbedding,
                element_type: ElementType::Float32,
                layout: TensorLayout::Nc,
                shape: vec![
                    Dimension::Fixed { value: 1 },
                    Dimension::Fixed { value: 384 },
                ],
                color_space: None,
                numeric_range: None,
            }],
            execution_targets: vec![BackendRequirement {
                kind: BackendKind::Cpu,
                minimum_runtime_version: None,
                precisions: vec![NumericPrecision::Float32],
            }],
            minimum_ram_bytes: 256,
            recommended_ram_bytes: 512,
            minimum_device_memory_bytes: 0,
            recommended_device_memory_bytes: 0,
            licensing: LicenseTerms {
                code_license: "Apache-2.0".into(),
                weight_license: "Apache-2.0".into(),
                training_data_notes: Some("must be reviewed".into()),
                redistribution: LicensePermission::Allowed,
                commercial_use: LicensePermission::Allowed,
                access: ModelAccess::Open,
                attribution_files: vec!["NOTICE".into()],
            },
            distribution: DistributionTerms {
                bundled_by_default: false,
                automatic_download_allowed: true,
                side_load_allowed: true,
                upstream_url: "https://example.invalid/model".into(),
            },
        }
    }

    #[test]
    fn valid_manifest_round_trips_as_data() {
        let manifest = valid_manifest();
        manifest.validate().expect("valid manifest");
        let json = serde_json::to_string(&manifest).expect("serialize model manifest");
        let decoded: ModelManifest =
            serde_json::from_str(&json).expect("deserialize model manifest");
        assert_eq!(manifest, decoded);
    }

    #[test]
    fn manifest_refuses_to_bundle_non_redistributable_weights() {
        let mut manifest = valid_manifest();
        manifest.licensing.redistribution = LicensePermission::Prohibited;
        manifest.distribution.bundled_by_default = true;
        assert_eq!(
            manifest.validate(),
            Err(ModelManifestError::BundledWithoutRedistributionPermission)
        );
    }

    #[test]
    fn manifest_rejects_unpinned_or_malformed_artifacts() {
        let mut manifest = valid_manifest();
        manifest.artifact.sha256 = "latest".into();
        assert_eq!(manifest.validate(), Err(ModelManifestError::InvalidSha256));
    }
}
