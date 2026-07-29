use std::collections::{BTreeMap, BTreeSet};

use blake3::Hasher;
use serde::{Deserialize, Deserializer, Serialize, de::Error as _};
use thiserror::Error;

use crate::{AiCapability, BackendKind, NumericPrecision};

mod artifact_path;

pub const MODEL_MANIFEST_SCHEMA_VERSION: u32 = 1;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ModelFormat {
    Onnx,
    Safetensors,
    Gguf,
    /// One or more source `.mlpackage` artifacts.
    ///
    /// Device-specialized `.mlmodelc` output is a rebuildable runtime cache and
    /// never the distributed artifact identity.
    CoreMlPackage,
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(
    rename_all = "snake_case",
    tag = "kind",
    content = "detail",
    deny_unknown_fields
)]
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
#[serde(rename_all = "snake_case", tag = "kind", deny_unknown_fields)]
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
#[serde(deny_unknown_fields)]
pub struct NumericRange {
    pub minimum: f64,
    pub maximum: f64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TensorContract {
    pub name: String,
    pub semantics: TensorSemantics,
    pub element_type: ElementType,
    pub layout: TensorLayout,
    #[serde(deserialize_with = "crate::wire_v1::vec_16")]
    pub shape: Vec<Dimension>,
    /// Explicit names such as `display_srgb`, `scene_linear_rec2020`, or `none`.
    pub color_space: Option<String>,
    pub numeric_range: Option<NumericRange>,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ModelArtifact {
    /// Canonical forward-slash path of one downloaded, hashable blob.
    ///
    /// A directory-style package must first have a reproducible archive or a
    /// canonical file inventory; this field never pretends an arbitrary
    /// directory has stable bytes.
    pub relative_path: String,
    pub role: ModelArtifactRole,
    pub byte_len: u64,
    pub sha256: String,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ModelArtifactRole {
    ModelDefinition,
    CoreMlPackageArchive,
    Weights,
    Tokenizer,
    Configuration,
    Auxiliary,
}

/// Exact downloadable blobs that together form one runnable model revision.
///
/// Multi-stage packages such as SAM keep their encoder, prompt encoder, and
/// decoder under one set identity. The package registry computes
/// `inventory_blake3` from the canonical blob inventory. Extraction verifies
/// its own canonical file inventory before compilation; a runtime may not
/// silently mix files from different sets.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ModelArtifactSet {
    pub set_id: String,
    /// Domain-separated BLAKE3 identity of the sorted artifact inventory.
    pub inventory_blake3: String,
    #[serde(deserialize_with = "crate::wire_v1::vec_128")]
    pub artifacts: Vec<ModelArtifact>,
}

impl ModelArtifactSet {
    /// Derives the exact artifact-set identity from canonical inventory facts.
    ///
    /// Artifact order in the serialized manifest is presentation only. The
    /// identity sorts by canonical relative path, length-prefixes every field,
    /// and hashes the raw per-blob SHA-256 bytes under a dedicated BLAKE3
    /// context.
    ///
    /// # Errors
    ///
    /// Returns an error when the inventory is empty, contains a non-canonical
    /// or duplicate path, an empty blob, or a malformed per-blob SHA-256.
    pub fn computed_inventory_blake3(&self) -> Result<String, ModelManifestError> {
        validate_artifact_inventory(&self.artifacts)?;

        let mut artifacts: Vec<_> = self.artifacts.iter().collect();
        artifacts.sort_by(|left, right| left.relative_path.cmp(&right.relative_path));

        let mut hasher = Hasher::new_derive_key("shadow.ai.model-artifact-set.inventory.v1");
        update_identity_field(&mut hasher, 0, &(artifacts.len() as u64).to_be_bytes());
        for artifact in artifacts {
            update_identity_field(&mut hasher, 1, artifact.relative_path.as_bytes());
            update_identity_field(&mut hasher, 2, artifact.role.identity_tag());
            update_identity_field(&mut hasher, 3, &artifact.byte_len.to_be_bytes());
            update_identity_field(&mut hasher, 4, &decode_sha256(&artifact.sha256)?);
        }
        Ok(hasher.finalize().to_hex().to_string())
    }
}

impl ModelArtifactRole {
    const fn identity_tag(self) -> &'static [u8] {
        match self {
            Self::ModelDefinition => b"model_definition",
            Self::CoreMlPackageArchive => b"core_ml_package_archive",
            Self::Weights => b"weights",
            Self::Tokenizer => b"tokenizer",
            Self::Configuration => b"configuration",
            Self::Auxiliary => b"auxiliary",
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LicenseTerms {
    pub code_license: String,
    pub weight_license: String,
    pub training_data_notes: Option<String>,
    pub redistribution: LicensePermission,
    pub commercial_use: LicensePermission,
    pub access: ModelAccess,
    #[serde(deserialize_with = "crate::wire_v1::vec_64")]
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
#[serde(deny_unknown_fields)]
pub struct DistributionTerms {
    pub bundled_by_default: bool,
    pub automatic_download_allowed: bool,
    pub side_load_allowed: bool,
    pub upstream_url: String,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BackendRequirement {
    pub kind: BackendKind,
    pub minimum_runtime_version: Option<String>,
    /// Ordered best-first. The scheduler chooses the first supported precision.
    #[serde(deserialize_with = "crate::wire_v1::vec_5")]
    pub precisions: Vec<NumericPrecision>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ModelManifest {
    pub schema_version: u32,
    pub model_id: String,
    pub exact_revision: String,
    pub artifact_set: ModelArtifactSet,
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

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ModelManifestWire {
    schema_version: u32,
    model_id: String,
    exact_revision: String,
    artifact_set: ModelArtifactSet,
    #[serde(deserialize_with = "crate::wire_v1::set_16")]
    capabilities: BTreeSet<AiCapability>,
    format: ModelFormat,
    opset: Option<u32>,
    quantization: Quantization,
    preprocessing_version: String,
    #[serde(deserialize_with = "crate::wire_v1::vec_64")]
    inputs: Vec<TensorContract>,
    #[serde(deserialize_with = "crate::wire_v1::vec_64")]
    outputs: Vec<TensorContract>,
    #[serde(deserialize_with = "crate::wire_v1::vec_16")]
    execution_targets: Vec<BackendRequirement>,
    minimum_ram_bytes: u64,
    recommended_ram_bytes: u64,
    minimum_device_memory_bytes: u64,
    recommended_device_memory_bytes: u64,
    licensing: LicenseTerms,
    distribution: DistributionTerms,
}

impl<'de> Deserialize<'de> for ModelManifest {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let wire = ModelManifestWire::deserialize(deserializer)?;
        let manifest = Self {
            schema_version: wire.schema_version,
            model_id: wire.model_id,
            exact_revision: wire.exact_revision,
            artifact_set: wire.artifact_set,
            capabilities: wire.capabilities,
            format: wire.format,
            opset: wire.opset,
            quantization: wire.quantization,
            preprocessing_version: wire.preprocessing_version,
            inputs: wire.inputs,
            outputs: wire.outputs,
            execution_targets: wire.execution_targets,
            minimum_ram_bytes: wire.minimum_ram_bytes,
            recommended_ram_bytes: wire.recommended_ram_bytes,
            minimum_device_memory_bytes: wire.minimum_device_memory_bytes,
            recommended_device_memory_bytes: wire.recommended_device_memory_bytes,
            licensing: wire.licensing,
            distribution: wire.distribution,
        };
        manifest.validate().map_err(D::Error::custom)?;
        Ok(manifest)
    }
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
        require_text(&self.licensing.code_license, "licensing.code_license")?;
        require_text(&self.licensing.weight_license, "licensing.weight_license")?;
        require_text(&self.distribution.upstream_url, "distribution.upstream_url")?;
        validate_attribution_paths(&self.licensing.attribution_files)?;

        if self.schema_version != MODEL_MANIFEST_SCHEMA_VERSION {
            return Err(ModelManifestError::UnsupportedSchemaVersion(
                self.schema_version,
            ));
        }
        validate_artifact_set(&self.artifact_set)?;
        if self.format == ModelFormat::CoreMlPackage
            && !self
                .artifact_set
                .artifacts
                .iter()
                .any(|artifact| artifact.role == ModelArtifactRole::CoreMlPackageArchive)
        {
            return Err(ModelManifestError::MissingCoreMlPackageArchive);
        }
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

fn validate_artifact_set(artifact_set: &ModelArtifactSet) -> Result<(), ModelManifestError> {
    require_text(&artifact_set.set_id, "artifact_set.set_id")?;
    validate_blake3(&artifact_set.inventory_blake3)?;
    let expected = artifact_set.computed_inventory_blake3()?;
    if artifact_set.inventory_blake3 != expected {
        return Err(ModelManifestError::ArtifactSetIdentityMismatch {
            expected,
            actual: artifact_set.inventory_blake3.clone(),
        });
    }
    Ok(())
}

fn validate_artifact_inventory(artifacts: &[ModelArtifact]) -> Result<(), ModelManifestError> {
    if artifacts.is_empty() {
        return Err(ModelManifestError::EmptyArtifactSet);
    }

    let mut paths = BTreeSet::new();
    let mut aliases = BTreeMap::new();
    for artifact in artifacts {
        validate_artifact_path(&artifact.relative_path)?;
        if !paths.insert(artifact.relative_path.as_str()) {
            return Err(ModelManifestError::DuplicateArtifactPath(
                artifact.relative_path.clone(),
            ));
        }
        if let Err(existing) = artifact_path::register_alias(&mut aliases, &artifact.relative_path)
        {
            return Err(ModelManifestError::ArtifactPathAlias {
                existing,
                alias: artifact.relative_path.clone(),
            });
        }
        if artifact.byte_len == 0 {
            return Err(ModelManifestError::EmptyArtifact(
                artifact.relative_path.clone(),
            ));
        }
        validate_sha256(&artifact.sha256)?;
    }
    Ok(())
}

fn validate_blake3(value: &str) -> Result<(), ModelManifestError> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err(ModelManifestError::InvalidBlake3);
    }
    Ok(())
}

fn decode_sha256(value: &str) -> Result<[u8; 32], ModelManifestError> {
    validate_sha256(value)?;
    let mut digest = [0_u8; 32];
    for (index, pair) in value.as_bytes().chunks_exact(2).enumerate() {
        let high = decode_hex_nibble(pair[0]).ok_or(ModelManifestError::InvalidSha256)?;
        let low = decode_hex_nibble(pair[1]).ok_or(ModelManifestError::InvalidSha256)?;
        digest[index] = (high << 4) | low;
    }
    Ok(digest)
}

const fn decode_hex_nibble(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

fn update_identity_field(hasher: &mut Hasher, tag: u8, value: &[u8]) {
    hasher.update(&[tag]);
    hasher.update(&(value.len() as u64).to_be_bytes());
    hasher.update(value);
}

fn validate_artifact_path(path: &str) -> Result<(), ModelManifestError> {
    if !artifact_path::validate(path) {
        return Err(ModelManifestError::InvalidArtifactPath(path.to_owned()));
    }
    Ok(())
}

fn validate_attribution_paths(paths: &[String]) -> Result<(), ModelManifestError> {
    let mut exact = BTreeSet::new();
    let mut aliases = BTreeMap::new();
    for path in paths {
        validate_artifact_path(path)?;
        if !exact.insert(path.as_str()) {
            return Err(ModelManifestError::DuplicateAttributionPath(path.clone()));
        }
        if let Err(existing) = artifact_path::register_alias(&mut aliases, path) {
            return Err(ModelManifestError::ArtifactPathAlias {
                existing,
                alias: path.clone(),
            });
        }
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
    #[error("manifest schema version {0} is unsupported")]
    UnsupportedSchemaVersion(u32),
    #[error("manifest field {0} must not be empty")]
    MissingText(&'static str),
    #[error("model artifact set is empty")]
    EmptyArtifactSet,
    #[error("model artifact `{0}` is empty")]
    EmptyArtifact(String),
    #[error("model artifact path `{0}` is not canonical and relative")]
    InvalidArtifactPath(String),
    #[error("model artifact path `{0}` appears more than once")]
    DuplicateArtifactPath(String),
    #[error("portable package paths `{existing}` and `{alias}` are filesystem aliases")]
    ArtifactPathAlias { existing: String, alias: String },
    #[error("license attribution path `{0}` appears more than once")]
    DuplicateAttributionPath(String),
    #[error("artifact sha256 must contain exactly 64 hexadecimal characters")]
    InvalidSha256,
    #[error("artifact-set BLAKE3 identity must be 64 lowercase hexadecimal characters")]
    InvalidBlake3,
    #[error("artifact-set identity does not match its canonical inventory")]
    ArtifactSetIdentityMismatch { expected: String, actual: String },
    #[error("Core ML artifact sets must include at least one package archive")]
    MissingCoreMlPackageArchive,
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
mod tests;
