//! Canonical repository objects and individual edge records.

use std::str::FromStr;

use serde::{Deserialize, Serialize, de::DeserializeOwned};

use super::{
    content_id::{EditObjectId, object_id},
    error::EditRepositoryError,
};

const MAX_EDGE_ROLE_BYTES: usize = 64;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum EditObjectKind {
    LibraryRoot,
    EntityMap,
    PhotoEditState,
    GradeNodeRevision,
    MaskRevision,
    StyleRevision,
    OutputState,
    LegacyRecipe,
}

impl EditObjectKind {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::LibraryRoot => "library_root",
            Self::EntityMap => "entity_map",
            Self::PhotoEditState => "photo_edit_state",
            Self::GradeNodeRevision => "grade_node_revision",
            Self::MaskRevision => "mask_revision",
            Self::StyleRevision => "style_revision",
            Self::OutputState => "output_state",
            Self::LegacyRecipe => "legacy_recipe",
        }
    }
}

impl FromStr for EditObjectKind {
    type Err = EditRepositoryError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "library_root" => Ok(Self::LibraryRoot),
            "entity_map" => Ok(Self::EntityMap),
            "photo_edit_state" => Ok(Self::PhotoEditState),
            "grade_node_revision" => Ok(Self::GradeNodeRevision),
            "mask_revision" => Ok(Self::MaskRevision),
            "style_revision" => Ok(Self::StyleRevision),
            "output_state" => Ok(Self::OutputState),
            "legacy_recipe" => Ok(Self::LegacyRecipe),
            _ => Err(EditRepositoryError::UnknownObjectKind(value.to_owned())),
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditObject {
    id: EditObjectId,
    kind: EditObjectKind,
    format_version: u32,
    canonical_json: Vec<u8>,
}

impl EditObject {
    /// Serializes one supported versioned payload into canonical JSON and
    /// derives its domain-separated content identifier.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for an unsupported version or a payload
    /// that cannot be represented as JSON.
    pub fn from_canonical_json<T: Serialize>(
        kind: EditObjectKind,
        format_version: u32,
        value: &T,
    ) -> Result<Self, EditRepositoryError> {
        if format_version == 0 {
            return Err(EditRepositoryError::InvalidFormatVersion);
        }
        validate_supported_object_version(kind, format_version)?;
        let canonical_json = canonical_json(value)?;
        let id = object_id(kind.as_str(), format_version, &canonical_json);
        Ok(Self {
            id,
            kind,
            format_version,
            canonical_json,
        })
    }

    /// Reconstructs an object only when its bytes are canonical and match the
    /// supplied content identifier.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for unsupported versions, malformed or
    /// non-canonical JSON, or a digest mismatch.
    pub fn from_stored_parts(
        id: EditObjectId,
        kind: EditObjectKind,
        format_version: u32,
        canonical_json: Vec<u8>,
    ) -> Result<Self, EditRepositoryError> {
        if format_version == 0 {
            return Err(EditRepositoryError::InvalidFormatVersion);
        }
        validate_supported_object_version(kind, format_version)?;
        if object_id(kind.as_str(), format_version, &canonical_json) != id {
            return Err(EditRepositoryError::ObjectDigestMismatch { id });
        }
        let value: serde_json::Value = serde_json::from_slice(&canonical_json)?;
        if serde_json::to_vec(&value)? != canonical_json {
            return Err(EditRepositoryError::NonCanonicalObjectPayload);
        }
        Ok(Self {
            id,
            kind,
            format_version,
            canonical_json,
        })
    }

    #[must_use]
    pub const fn id(&self) -> EditObjectId {
        self.id
    }

    #[must_use]
    pub const fn kind(&self) -> EditObjectKind {
        self.kind
    }

    #[must_use]
    pub const fn format_version(&self) -> u32 {
        self.format_version
    }

    #[must_use]
    pub fn canonical_json(&self) -> &[u8] {
        &self.canonical_json
    }

    /// Decodes an integrity-checked object payload into its versioned type.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] when the requested type does not match
    /// the stored JSON payload.
    pub fn decode<T: DeserializeOwned>(&self) -> Result<T, EditRepositoryError> {
        serde_json::from_slice(&self.canonical_json).map_err(Into::into)
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Serialize, Deserialize)]
pub struct EditObjectEdge {
    role: String,
    position: u32,
    target: EditObjectId,
}

impl EditObjectEdge {
    /// Creates one indexed reference from an object payload to another object.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] when the role is empty, too long, or
    /// contains characters outside the stable edge-role alphabet.
    pub fn new(
        role: impl Into<String>,
        position: u32,
        target: EditObjectId,
    ) -> Result<Self, EditRepositoryError> {
        let role = role.into();
        validate_edge_role(&role)?;
        Ok(Self {
            role,
            position,
            target,
        })
    }

    #[must_use]
    pub fn role(&self) -> &str {
        &self.role
    }

    #[must_use]
    pub const fn position(&self) -> u32 {
        self.position
    }

    #[must_use]
    pub const fn target(&self) -> EditObjectId {
        self.target
    }
}

fn validate_edge_role(role: &str) -> Result<(), EditRepositoryError> {
    if role.is_empty()
        || role.len() > MAX_EDGE_ROLE_BYTES
        || !role.bytes().all(|value| {
            value.is_ascii_lowercase() || value.is_ascii_digit() || b"_.-".contains(&value)
        })
    {
        return Err(EditRepositoryError::InvalidEdgeRole(role.to_owned()));
    }
    Ok(())
}

fn validate_supported_object_version(
    kind: EditObjectKind,
    format_version: u32,
) -> Result<(), EditRepositoryError> {
    if format_version != 1 {
        return Err(EditRepositoryError::UnsupportedObjectFormatVersion {
            kind,
            format_version,
        });
    }
    Ok(())
}

pub(super) fn require_object_type(
    object: &EditObject,
    expected_kind: EditObjectKind,
    expected_version: u32,
) -> Result<(), EditRepositoryError> {
    if object.kind != expected_kind || object.format_version != expected_version {
        return Err(EditRepositoryError::ObjectTypeMismatch {
            expected_kind,
            expected_version,
            actual_kind: object.kind,
            actual_version: object.format_version,
        });
    }
    Ok(())
}

fn canonical_json<T: Serialize>(value: &T) -> Result<Vec<u8>, EditRepositoryError> {
    // Serializing through Value sorts object keys (serde_json's default map is
    // a BTreeMap). This makes semantically identical map inputs independent of
    // HashMap iteration order before they enter the content-addressed store.
    let value = serde_json::to_value(value)?;
    serde_json::to_vec(&value).map_err(Into::into)
}

#[cfg(test)]
mod tests;
