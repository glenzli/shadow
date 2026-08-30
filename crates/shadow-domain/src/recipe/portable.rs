//! Versioned, self-identifying Shadow Recipe interchange documents.

use serde::{Deserialize, Serialize};
use thiserror::Error;

use super::{RecipeSnapshot, RecipeValidationError, canonical_recipe_snapshot_digest};

pub const SHADOW_RECIPE_FORMAT: &str = "dev.shadow.recipe";
pub const CURRENT_SHADOW_RECIPE_DOCUMENT_VERSION: u32 = 1;
pub const MAX_SHADOW_RECIPE_DOCUMENT_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_SHADOW_RECIPE_LABEL_BYTES: usize = 256;

#[derive(Debug, Clone, PartialEq)]
pub struct ShadowRecipeDocument {
    label: Option<String>,
    snapshot: RecipeSnapshot,
}

impl ShadowRecipeDocument {
    pub fn new(
        label: Option<&str>,
        snapshot: RecipeSnapshot,
    ) -> Result<Self, ShadowRecipeDocumentError> {
        snapshot.validate_for_commit()?;
        let label = normalize_label(label)?;
        Ok(Self { label, snapshot })
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
        };
        Ok(serde_json::to_vec_pretty(&wire)?)
    }

    pub fn from_json(bytes: &[u8]) -> Result<Self, ShadowRecipeDocumentError> {
        if bytes.len() > MAX_SHADOW_RECIPE_DOCUMENT_BYTES {
            return Err(ShadowRecipeDocumentError::DocumentTooLarge);
        }
        let wire: ShadowRecipeWire = serde_json::from_slice(bytes)?;
        if wire.format != SHADOW_RECIPE_FORMAT {
            return Err(ShadowRecipeDocumentError::UnknownFormat(wire.format));
        }
        if wire.document_version != CURRENT_SHADOW_RECIPE_DOCUMENT_VERSION {
            return Err(ShadowRecipeDocumentError::UnsupportedDocumentVersion(
                wire.document_version,
            ));
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
        Self::new(wire.label.as_deref(), wire.snapshot)
    }
}

#[derive(Debug, Error)]
pub enum ShadowRecipeDocumentError {
    #[error("Shadow Recipe document exceeds the 16 MiB limit")]
    DocumentTooLarge,
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

#[cfg(test)]
mod tests;
