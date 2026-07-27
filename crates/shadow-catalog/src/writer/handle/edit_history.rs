//! Client adapters for per-photo Recipe and Library-wide edit history.

use shadow_domain::{EditCommitId, EditObjectId, PhotoId, RecipeCommitId};

use crate::{
    CatalogError, CommitEditRepository, CommitRecipe, CommitRecipeAndEditRepository,
    CommitRecipeAndEditRepositoryResult, EditObjectPackWrite, EditObjectRecord,
    EditRepositoryCommitRecord, EditRepositoryRefRecord, RecipeCommitRecord, RecipeRefRecord,
    SetRecipeRef, StoreEditObjectPackResult,
};

use super::super::{
    CatalogHandle,
    protocol::{EditHistoryMessage, Message},
};

impl CatalogHandle {
    /// Persists an immutable Recipe commit and optional ref move through the
    /// single Catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the commit is invalid, a guarded ref has
    /// changed since it was read, or persistence fails.
    pub fn commit_recipe(
        &self,
        request: &CommitRecipe,
    ) -> Result<RecipeCommitRecord, CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::CommitRecipe(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Lists every immutable Recipe commit owned by a photo.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor or persisted data is invalid.
    pub fn recipe_commits(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<RecipeCommitRecord>, CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::RecipeCommits(photo_id, response))
        })
    }

    /// Resolves one immutable Recipe commit by owner and id.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor or persisted data is invalid.
    pub fn recipe_commit(
        &self,
        photo_id: PhotoId,
        commit_id: RecipeCommitId,
    ) -> Result<Option<RecipeCommitRecord>, CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::RecipeCommit(
                photo_id, commit_id, response,
            ))
        })
    }

    /// Resolves a named Recipe ref without changing it.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid name or unavailable actor.
    pub fn recipe_ref(
        &self,
        photo_id: PhotoId,
        name: &str,
    ) -> Result<Option<RecipeRefRecord>, CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::RecipeRef(
                photo_id,
                name.to_owned(),
                response,
            ))
        })
    }

    /// Moves a Recipe ref through the single Catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the target does not belong to the photo or
    /// the write fails.
    pub fn set_recipe_ref(&self, request: &SetRecipeRef) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::SetRecipeRef(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Discards all editable Recipe history for a single photo.
    ///
    /// This is intentionally not part of normal editing. The desktop uses it
    /// only after a user confirms that an obsolete development Recipe may be
    /// reset to the current fixed contract.
    pub fn discard_recipe_history(&self, photo_id: PhotoId) -> Result<usize, CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::DiscardRecipeHistory(photo_id, response))
        })
    }

    /// Stores a topologically unordered pack of immutable edit objects through
    /// the Catalog's single writer transaction.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if an object is invalid, an edge target is
    /// absent, persisted content conflicts, or the actor is unavailable.
    pub fn store_edit_object_pack(
        &self,
        request: &EditObjectPackWrite,
    ) -> Result<StoreEditObjectPackResult, CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::StoreEditObjectPack(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Reads and integrity-checks one content-addressed edit object.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if persisted bytes or indexed edges are corrupt
    /// or the actor is unavailable.
    pub fn edit_object(&self, id: EditObjectId) -> Result<Option<EditObjectRecord>, CatalogError> {
        self.request(|response| Message::EditHistory(EditHistoryMessage::EditObject(id, response)))
    }

    /// Writes one Library-wide immutable edit commit and advances guarded refs
    /// in the same Catalog transaction.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for missing roots/parents, stale ref heads,
    /// content collisions, or an unavailable actor.
    pub fn commit_edit_repository(
        &self,
        request: &CommitEditRepository,
    ) -> Result<EditRepositoryCommitRecord, CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::CommitEditRepository(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Atomically publishes one per-photo compatibility commit and its
    /// Library-wide repository commit through the single writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if either commit/ref set is invalid or stale,
    /// persistence fails, or the actor is unavailable.
    pub fn commit_recipe_and_edit_repository(
        &self,
        request: &CommitRecipeAndEditRepository,
    ) -> Result<CommitRecipeAndEditRepositoryResult, CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::CommitRecipeAndEditRepository(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Reads and integrity-checks one Library-wide edit commit.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when persisted data is invalid or the actor is
    /// unavailable.
    pub fn edit_repository_commit(
        &self,
        id: EditCommitId,
    ) -> Result<Option<EditRepositoryCommitRecord>, CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::EditRepositoryCommit(id, response))
        })
    }

    /// Resolves one guarded Library-wide branch, named version, or tag.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid name, invalid persisted data, or
    /// an unavailable actor.
    pub fn edit_repository_ref(
        &self,
        name: &str,
    ) -> Result<Option<EditRepositoryRefRecord>, CatalogError> {
        self.request(|response| {
            Message::EditHistory(EditHistoryMessage::EditRepositoryRef(
                name.to_owned(),
                response,
            ))
        })
    }
}
