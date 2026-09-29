//! Versioned, self-identifying Shadow Recipe interchange documents.

use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;
use thiserror::Error;

use super::{RecipeSnapshot, RecipeValidationError, canonical_recipe_snapshot_digest};

pub const SHADOW_RECIPE_FORMAT: &str = "dev.shadow.recipe";
pub const CURRENT_SHADOW_RECIPE_DOCUMENT_VERSION: u32 = 2;
pub const MAX_SHADOW_RECIPE_DOCUMENT_BYTES: usize = 64 * 1024 * 1024;
pub const MAX_SHADOW_RECIPE_LABEL_BYTES: usize = 256;
const MAX_LUT_BYTES: usize = 16 * 1024 * 1024;
const MAX_TOTAL_LUT_BYTES: usize = 32 * 1024 * 1024;

/// Bounded text resource, keyed by the original LUT's SHA-256 identity. Domain
/// verifies the document digest; the image boundary also verifies SHA-256 and
/// the native .cube grammar before admitting it to the destination store.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ShadowRecipeLutResource {
    resource_id: String,
    document: String,
    document_blake3: String,
}

impl ShadowRecipeLutResource {
    pub fn new(resource_id: String, document: String) -> Result<Self, ShadowRecipeDocumentError> {
        let resource = Self {
            document_blake3: blake3::hash(document.as_bytes()).to_hex().to_string(),
            resource_id,
            document,
        };
        resource.validate()?;
        Ok(resource)
    }

    pub fn resource_id(&self) -> &str {
        &self.resource_id
    }

    pub fn document(&self) -> &str {
        &self.document
    }

    fn validate(&self) -> Result<(), ShadowRecipeDocumentError> {
        if self.resource_id.len() != 64
            || !self
                .resource_id
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
            || self.document.is_empty()
            || self.document.len() > MAX_LUT_BYTES
            || self.document_blake3 != blake3::hash(self.document.as_bytes()).to_hex().as_str()
        {
            return Err(ShadowRecipeDocumentError::InvalidLutResources);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ShadowRecipeDocument {
    label: Option<String>,
    snapshot: RecipeSnapshot,
    lut_resources: Vec<ShadowRecipeLutResource>,
}

impl ShadowRecipeDocument {
    pub fn new(
        label: Option<&str>,
        snapshot: RecipeSnapshot,
    ) -> Result<Self, ShadowRecipeDocumentError> {
        snapshot.validate_for_commit()?;
        let label = normalize_label(label)?;
        Ok(Self {
            label,
            snapshot,
            lut_resources: Vec::new(),
        })
    }

    pub fn with_lut_resources(
        mut self,
        resources: Vec<ShadowRecipeLutResource>,
    ) -> Result<Self, ShadowRecipeDocumentError> {
        validate_lut_resources(&resources)?;
        self.lut_resources = resources;
        Ok(self)
    }

    pub fn lut_resources(&self) -> &[ShadowRecipeLutResource] {
        &self.lut_resources
    }

    pub fn label(&self) -> Option<&str> {
        self.label.as_deref()
    }

    pub const fn snapshot(&self) -> &RecipeSnapshot {
        &self.snapshot
    }

    pub fn into_snapshot(self) -> RecipeSnapshot {
        self.snapshot
    }

    pub fn to_pretty_json(&self) -> Result<Vec<u8>, ShadowRecipeDocumentError> {
        let digest = snapshot_digest_hex(&self.snapshot)?;
        let wire = ShadowRecipeWire {
            format: SHADOW_RECIPE_FORMAT.to_owned(),
            document_version: CURRENT_SHADOW_RECIPE_DOCUMENT_VERSION,
            recipe_schema_version: self.snapshot.schema_version(),
            label: self.label.clone(),
            snapshot_blake3: digest,
            snapshot: self.snapshot.clone(),
            lut_resources: self.lut_resources.clone(),
        };
        checked_document_bytes(serde_json::to_vec_pretty(&wire)?)
    }

    pub fn from_json(bytes: &[u8]) -> Result<Self, ShadowRecipeDocumentError> {
        if bytes.len() > MAX_SHADOW_RECIPE_DOCUMENT_BYTES {
            return Err(ShadowRecipeDocumentError::DocumentTooLarge);
        }
        let wire: ShadowRecipeWire = serde_json::from_slice(bytes)?;
        if wire.format != SHADOW_RECIPE_FORMAT {
            return Err(ShadowRecipeDocumentError::UnknownFormat(wire.format));
        }
        if wire.document_version != 1
            && wire.document_version != CURRENT_SHADOW_RECIPE_DOCUMENT_VERSION
        {
            return Err(ShadowRecipeDocumentError::UnsupportedDocumentVersion(
                wire.document_version,
            ));
        }
        if wire.document_version == 1 && !wire.lut_resources.is_empty() {
            return Err(ShadowRecipeDocumentError::InvalidLutResources);
        }
        if wire.recipe_schema_version != wire.snapshot.schema_version() {
            return Err(ShadowRecipeDocumentError::RecipeSchemaMismatch {
                declared: wire.recipe_schema_version,
                actual: wire.snapshot.schema_version(),
            });
        }
        wire.snapshot.validate_for_commit()?;
        let actual_digest = snapshot_digest_hex(&wire.snapshot)?;
        if wire.snapshot_blake3 != actual_digest {
            return Err(ShadowRecipeDocumentError::DigestMismatch);
        }
        Self::new(wire.label.as_deref(), wire.snapshot)?.with_lut_resources(wire.lut_resources)
    }
}

#[derive(Debug, Error)]
pub enum ShadowRecipeDocumentError {
    #[error("Shadow Recipe document exceeds the 64 MiB limit")]
    DocumentTooLarge,
    #[error(
        "Shadow Recipe LUT resources have invalid identities, digests, duplicates, or exceed the 16-resource / 16 MiB each / 32 MiB total limit"
    )]
    InvalidLutResources,
    #[error("Shadow Recipe label must be trimmed, non-empty, and at most 256 UTF-8 bytes")]
    InvalidLabel,
    #[error("unknown Shadow Recipe format {0:?}")]
    UnknownFormat(String),
    #[error("unsupported Shadow Recipe document version {0}")]
    UnsupportedDocumentVersion(u32),
    #[error("declared Recipe schema {declared} does not match snapshot schema {actual}")]
    RecipeSchemaMismatch { declared: u32, actual: u32 },
    #[error("Shadow Recipe snapshot digest does not match its contents")]
    DigestMismatch,
    #[error(transparent)]
    InvalidRecipe(#[from] RecipeValidationError),
    #[error(transparent)]
    Json(#[from] serde_json::Error),
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct ShadowRecipeWire {
    format: String,
    document_version: u32,
    recipe_schema_version: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    label: Option<String>,
    snapshot_blake3: String,
    snapshot: RecipeSnapshot,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    lut_resources: Vec<ShadowRecipeLutResource>,
}

fn validate_lut_resources(
    resources: &[ShadowRecipeLutResource],
) -> Result<(), ShadowRecipeDocumentError> {
    if resources.len() > 16 {
        return Err(ShadowRecipeDocumentError::InvalidLutResources);
    }
    let mut identities = BTreeSet::new();
    let mut total = 0_usize;
    for resource in resources {
        resource.validate()?;
        total += resource.document.len();
        if total > MAX_TOTAL_LUT_BYTES || !identities.insert(resource.resource_id()) {
            return Err(ShadowRecipeDocumentError::InvalidLutResources);
        }
    }
    Ok(())
}

fn normalize_label(label: Option<&str>) -> Result<Option<String>, ShadowRecipeDocumentError> {
    let Some(label) = label else {
        return Ok(None);
    };
    if label.is_empty()
        || label.trim() != label
        || label.len() > MAX_SHADOW_RECIPE_LABEL_BYTES
        || label.chars().any(char::is_control)
    {
        return Err(ShadowRecipeDocumentError::InvalidLabel);
    }
    Ok(Some(label.to_owned()))
}

fn snapshot_digest_hex(snapshot: &RecipeSnapshot) -> Result<String, serde_json::Error> {
    let digest = canonical_recipe_snapshot_digest(snapshot)?;
    let mut encoded = String::with_capacity(digest.len() * 2);
    for byte in digest {
        use std::fmt::Write as _;
        write!(&mut encoded, "{byte:02x}").expect("writing to String cannot fail");
    }
    Ok(encoded)
}

fn checked_document_bytes(encoded: Vec<u8>) -> Result<Vec<u8>, ShadowRecipeDocumentError> {
    if encoded.len() > MAX_SHADOW_RECIPE_DOCUMENT_BYTES {
        return Err(ShadowRecipeDocumentError::DocumentTooLarge);
    }
    Ok(encoded)
}

#[cfg(test)]
mod tests;
