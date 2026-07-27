//! Actor messages for Library affinity state, albums, and membership.

use std::sync::mpsc::SyncSender;

use shadow_domain::{CollectionId, PhotoId};

use crate::{
    AlbumKind, AlbumRecord, CatalogError, LibraryPhotoCursor, LibraryPhotoFilter, LibraryPhotoPage,
    PhotoLibraryState, SetPhotoLibraryState, SmartAlbumQueryV1,
};

pub(in crate::writer) enum LibraryCollectionsMessage {
    SetPhotoState(
        Box<SetPhotoLibraryState>,
        SyncSender<Result<(), CatalogError>>,
    ),
    PhotoState(PhotoId, SyncSender<Result<PhotoLibraryState, CatalogError>>),
    CreateAlbum(
        AlbumKind,
        String,
        Option<String>,
        i64,
        SyncSender<Result<AlbumRecord, CatalogError>>,
    ),
    CreateSmartAlbum(
        String,
        SmartAlbumQueryV1,
        i64,
        SyncSender<Result<AlbumRecord, CatalogError>>,
    ),
    RenameAlbum(
        CollectionId,
        String,
        i64,
        SyncSender<Result<AlbumRecord, CatalogError>>,
    ),
    ReplaceSmartAlbumQuery(
        CollectionId,
        SmartAlbumQueryV1,
        i64,
        SyncSender<Result<AlbumRecord, CatalogError>>,
    ),
    DeleteAlbum(CollectionId, SyncSender<Result<bool, CatalogError>>),
    Albums(SyncSender<Result<Vec<AlbumRecord>, CatalogError>>),
    AddPhoto(
        CollectionId,
        PhotoId,
        i64,
        i64,
        SyncSender<Result<(), CatalogError>>,
    ),
    RemovePhoto(
        CollectionId,
        PhotoId,
        SyncSender<Result<bool, CatalogError>>,
    ),
    AlbumsForPhoto(PhotoId, SyncSender<Result<Vec<AlbumRecord>, CatalogError>>),
    SmartAlbumFilter(
        CollectionId,
        SyncSender<Result<LibraryPhotoFilter, CatalogError>>,
    ),
    SmartAlbumPhotoPage(
        CollectionId,
        Option<LibraryPhotoCursor>,
        usize,
        SyncSender<Result<LibraryPhotoPage, CatalogError>>,
    ),
    SmartAlbumPhotoCount(CollectionId, SyncSender<Result<u64, CatalogError>>),
}
