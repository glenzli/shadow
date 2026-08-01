//! Actor-side execution for Recipe and Library-wide edit-history messages.

use crate::Catalog;

use super::super::protocol::EditHistoryMessage;

pub(super) fn run_edit_history_message(catalog: &mut Catalog, message: EditHistoryMessage) {
    match message {
        EditHistoryMessage::CommitRecipe(request, response) => {
            let _ = response.send(catalog.commit_recipe(request.as_ref()));
        }
        EditHistoryMessage::RecipeCommits(photo_id, response) => {
            let _ = response.send(catalog.recipe_commits(photo_id));
        }
        EditHistoryMessage::RecipeCommit(photo_id, commit_id, response) => {
            let _ = response.send(catalog.recipe_commit(photo_id, commit_id));
        }
        EditHistoryMessage::RecipeHistoryPage(photo_id, after, limit, response) => {
            let _ = response.send(catalog.recipe_history_page(photo_id, after.as_ref(), limit));
        }
        EditHistoryMessage::RecipeRef(photo_id, name, response) => {
            let _ = response.send(catalog.recipe_ref(photo_id, &name));
        }
        EditHistoryMessage::SetRecipeRef(request, response) => {
            let _ = response.send(catalog.set_recipe_ref(request.as_ref()));
        }
        EditHistoryMessage::DiscardRecipeHistory(photo_id, response) => {
            let _ = response.send(catalog.discard_recipe_history(photo_id));
        }
        EditHistoryMessage::StoreEditObjectPack(request, response) => {
            let _ = response.send(catalog.store_edit_object_pack(request.as_ref()));
        }
        EditHistoryMessage::EditObject(id, response) => {
            let _ = response.send(catalog.edit_object(id));
        }
        EditHistoryMessage::CommitEditRepository(request, response) => {
            let _ = response.send(catalog.commit_edit_repository(request.as_ref()));
        }
        EditHistoryMessage::CommitRecipeAndEditRepository(request, response) => {
            let _ = response.send(catalog.commit_recipe_and_edit_repository(request.as_ref()));
        }
        EditHistoryMessage::EditRepositoryCommit(id, response) => {
            let _ = response.send(catalog.edit_repository_commit(id));
        }
        EditHistoryMessage::EditRepositoryHistoryPage(after, limit, response) => {
            let _ = response.send(catalog.edit_repository_history_page(after.as_ref(), limit));
        }
        EditHistoryMessage::EditRepositoryRef(name, response) => {
            let _ = response.send(catalog.edit_repository_ref(&name));
        }
        EditHistoryMessage::EditRepositoryRefPage(after_name, limit, response) => {
            let _ = response.send(catalog.edit_repository_ref_page(after_name.as_deref(), limit));
        }
    }
}
