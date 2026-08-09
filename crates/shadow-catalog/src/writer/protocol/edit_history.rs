//! Actor messages for per-photo Recipe and Library-wide edit history.

use std::sync::mpsc::SyncSender;

use shadow_domain::{EditCommitId, EditObjectId, PhotoId, PhotoVariantId, RecipeCommitId};

use crate::{
    ActivatePhotoVariant, CatalogError, CommitEditRepository, CommitRecipe,
    CommitRecipeAndEditRepository, CommitRecipeAndEditRepositoryResult, CreatePhotoVariant,
    EditObjectPackWrite, EditObjectRecord, EditRepositoryCommitRecord, EditRepositoryHistoryCursor,
    EditRepositoryHistoryPage, EditRepositoryRefPage, EditRepositoryRefRecord, PhotoVariantRecord,
    RecipeCommitRecord, RecipeHistoryCursor, RecipeHistoryPage, RecipeRefRecord,
    RemovePhotoVariant, RenamePhotoVariant, SetRecipeRef, StoreEditObjectPackResult,
};

pub(in crate::writer) enum EditHistoryMessage {
    CommitRecipe(
        Box<CommitRecipe>,
        SyncSender<Result<RecipeCommitRecord, CatalogError>>,
    ),
    CommitRecipeForVariant(
        Box<CommitRecipe>,
        PhotoVariantId,
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
    RecipeHistoryPage(
        PhotoId,
        Option<RecipeHistoryCursor>,
        usize,
        SyncSender<Result<RecipeHistoryPage, CatalogError>>,
    ),
    RecipeRef(
        PhotoId,
        String,
        SyncSender<Result<Option<RecipeRefRecord>, CatalogError>>,
    ),
    SetRecipeRef(Box<SetRecipeRef>, SyncSender<Result<(), CatalogError>>),
    PhotoVariants(
        PhotoId,
        SyncSender<Result<Vec<PhotoVariantRecord>, CatalogError>>,
    ),
    CreatePhotoVariant(
        Box<CreatePhotoVariant>,
        SyncSender<Result<PhotoVariantRecord, CatalogError>>,
    ),
    RenamePhotoVariant(
        Box<RenamePhotoVariant>,
        SyncSender<Result<(), CatalogError>>,
    ),
    ActivatePhotoVariant(
        Box<ActivatePhotoVariant>,
        SyncSender<Result<(), CatalogError>>,
    ),
    RemovePhotoVariant(
        Box<RemovePhotoVariant>,
        SyncSender<Result<(), CatalogError>>,
    ),
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
    EditRepositoryHistoryPage(
        Option<EditRepositoryHistoryCursor>,
        usize,
        SyncSender<Result<EditRepositoryHistoryPage, CatalogError>>,
    ),
    EditRepositoryRef(
        String,
        SyncSender<Result<Option<EditRepositoryRefRecord>, CatalogError>>,
    ),
    EditRepositoryRefPage(
        Option<String>,
        usize,
        SyncSender<Result<EditRepositoryRefPage, CatalogError>>,
    ),
}
