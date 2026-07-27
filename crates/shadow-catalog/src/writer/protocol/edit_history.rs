//! Actor messages for per-photo Recipe and Library-wide edit history.

use std::sync::mpsc::SyncSender;

use shadow_domain::{EditCommitId, EditObjectId, PhotoId, RecipeCommitId};

use crate::{
    CatalogError, CommitEditRepository, CommitRecipe, CommitRecipeAndEditRepository,
    CommitRecipeAndEditRepositoryResult, EditObjectPackWrite, EditObjectRecord,
    EditRepositoryCommitRecord, EditRepositoryRefRecord, RecipeCommitRecord, RecipeRefRecord,
    SetRecipeRef, StoreEditObjectPackResult,
};

pub(in crate::writer) enum EditHistoryMessage {
    CommitRecipe(
        Box<CommitRecipe>,
        SyncSender<Result<RecipeCommitRecord, CatalogError>>,
    ),
    RecipeCommits(
        PhotoId,
        SyncSender<Result<Vec<RecipeCommitRecord>, CatalogError>>,
    ),
    RecipeCommit(
        PhotoId,
        RecipeCommitId,
        SyncSender<Result<Option<RecipeCommitRecord>, CatalogError>>,
    ),
    RecipeRef(
        PhotoId,
        String,
        SyncSender<Result<Option<RecipeRefRecord>, CatalogError>>,
    ),
    SetRecipeRef(Box<SetRecipeRef>, SyncSender<Result<(), CatalogError>>),
    DiscardRecipeHistory(PhotoId, SyncSender<Result<usize, CatalogError>>),
    StoreEditObjectPack(
        Box<EditObjectPackWrite>,
        SyncSender<Result<StoreEditObjectPackResult, CatalogError>>,
    ),
    EditObject(
        EditObjectId,
        SyncSender<Result<Option<EditObjectRecord>, CatalogError>>,
    ),
    CommitEditRepository(
        Box<CommitEditRepository>,
        SyncSender<Result<EditRepositoryCommitRecord, CatalogError>>,
    ),
    CommitRecipeAndEditRepository(
        Box<CommitRecipeAndEditRepository>,
        SyncSender<Result<CommitRecipeAndEditRepositoryResult, CatalogError>>,
    ),
    EditRepositoryCommit(
        EditCommitId,
        SyncSender<Result<Option<EditRepositoryCommitRecord>, CatalogError>>,
    ),
    EditRepositoryRef(
        String,
        SyncSender<Result<Option<EditRepositoryRefRecord>, CatalogError>>,
    ),
}
