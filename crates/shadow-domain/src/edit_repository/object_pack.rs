//! Typed object-pack encoding and edge-index validation.
//!
//! This is the single contract that binds canonical object bytes to the
//! complete edge index derived from their typed payload. Repository objects and
//! Library state remain independently navigable and depend only inward on
//! content identities; this owner is their one-way composition boundary.

use super::{
    error::EditRepositoryError,
    library_state::{EditEntityMapV1, LibraryRootV1},
    repository_object::{EditObject, EditObjectEdge, EditObjectKind, require_object_type},
};

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct EditObjectPack {
    object: EditObject,
    edges: Vec<EditObjectEdge>,
}

impl EditObjectPack {
    /// Binds one immutable object to the edge index derived from its payload.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for duplicate edges or when the index
    /// disagrees with a typed payload such as a Library root or entity map.
    pub fn new(
        object: EditObject,
        mut edges: Vec<EditObjectEdge>,
    ) -> Result<Self, EditRepositoryError> {
        edges.sort();
        if edges.windows(2).any(|pair| {
            pair[0].role() == pair[1].role() && pair[0].position() == pair[1].position()
        }) {
            return Err(EditRepositoryError::DuplicateObjectEdge);
        }
        let pack = Self { object, edges };
        pack.validate_typed_edges()?;
        Ok(pack)
    }

    #[must_use]
    pub const fn object(&self) -> &EditObject {
        &self.object
    }

    #[must_use]
    pub fn edges(&self) -> &[EditObjectEdge] {
        &self.edges
    }

    fn validate_typed_edges(&self) -> Result<(), EditRepositoryError> {
        let expected = match (self.object.kind(), self.object.format_version()) {
            (EditObjectKind::LibraryRoot, 1) => {
                let root: LibraryRootV1 = serde_json::from_slice(self.object.canonical_json())?;
                root.edges()?
            }
            (EditObjectKind::EntityMap, 1) => {
                let map: EditEntityMapV1 = serde_json::from_slice(self.object.canonical_json())?;
                map.validate()?;
                map.edges()?
            }
            (EditObjectKind::LegacyRecipe, 1) => {
                let recipe: crate::RecipeCommit =
                    serde_json::from_slice(self.object.canonical_json())?;
                recipe
                    .validate()
                    .map_err(|error| EditRepositoryError::InvalidLegacyRecipe(error.to_string()))?;
                Vec::new()
            }
            (EditObjectKind::GradeNodeRevision, 1) => {
                let revision: crate::LayerRevision =
                    serde_json::from_slice(self.object.canonical_json())?;
                revision.validate().map_err(|error| {
                    EditRepositoryError::InvalidGradeNodeRevision(error.to_string())
                })?;
                Vec::new()
            }
            _ => Vec::new(),
        };
        if expected != self.edges {
            return Err(EditRepositoryError::ObjectEdgesDoNotMatchPayload);
        }
        Ok(())
    }
}

impl LibraryRootV1 {
    /// Decodes a v1 Library root from an object of the exact expected type.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for a kind/version mismatch or malformed
    /// payload JSON.
    pub fn from_object(object: &EditObject) -> Result<Self, EditRepositoryError> {
        require_object_type(object, EditObjectKind::LibraryRoot, 1)?;
        object.decode()
    }

    /// Encodes this root and its deterministic role edges as one object pack.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] if an edge role or payload cannot be
    /// encoded under the v1 contract.
    pub fn into_object_pack(self) -> Result<EditObjectPack, EditRepositoryError> {
        let edges = self.edges()?;
        let object = EditObject::from_canonical_json(EditObjectKind::LibraryRoot, 1, &self)?;
        EditObjectPack::new(object, edges)
    }

    fn edges(self) -> Result<Vec<EditObjectEdge>, EditRepositoryError> {
        let mut edges = Vec::new();
        for (role, target) in [
            ("photo_recipes", self.photo_recipes),
            ("shared_grade_heads", self.shared_grade_heads),
            ("masks", self.masks),
            ("styles", self.styles),
            ("output_states", self.output_states),
        ] {
            if let Some(target) = target {
                edges.push(EditObjectEdge::new(role, 0, target)?);
            }
        }
        Ok(edges)
    }
}

impl EditEntityMapV1 {
    /// Decodes and validates a sorted v1 entity map from an exact map object.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] for a kind/version mismatch, malformed
    /// JSON, invalid keys, or non-canonical entry order.
    pub fn from_object(object: &EditObject) -> Result<Self, EditRepositoryError> {
        require_object_type(object, EditObjectKind::EntityMap, 1)?;
        let map: Self = object.decode()?;
        map.validate()?;
        Ok(map)
    }

    /// Encodes this map and its ordered entry edges as one object pack.
    ///
    /// # Errors
    ///
    /// Returns [`EditRepositoryError`] when the map violates the v1 key or
    /// ordering contract, or contains too many entries to index safely.
    pub fn into_object_pack(self) -> Result<EditObjectPack, EditRepositoryError> {
        self.validate()?;
        let edges = self.edges()?;
        let object = EditObject::from_canonical_json(EditObjectKind::EntityMap, 1, &self)?;
        EditObjectPack::new(object, edges)
    }

    fn edges(&self) -> Result<Vec<EditObjectEdge>, EditRepositoryError> {
        self.entries()
            .iter()
            .enumerate()
            .map(|(index, entry)| {
                let position =
                    u32::try_from(index).map_err(|_| EditRepositoryError::TooManyEntityEntries)?;
                EditObjectEdge::new("entry", position, entry.value)
            })
            .collect()
    }
}

#[cfg(test)]
mod tests;
