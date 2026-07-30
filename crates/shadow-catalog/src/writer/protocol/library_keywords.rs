//! Actor messages for the Library keyword taxonomy and photo assignments.

use std::sync::mpsc::SyncSender;

use shadow_domain::{KeywordId, PhotoId};

use crate::{
    CatalogError, LibraryKeywordAssignmentOrigin, LibraryKeywordDeletionReceipt,
    LibraryKeywordMutationReceipt, LibraryKeywordRecord, LibraryPhotoKeyword,
};

pub(in crate::writer) enum LibraryKeywordsMessage {
    Create(
        Option<KeywordId>,
        String,
        i64,
        SyncSender<Result<LibraryKeywordRecord, CatalogError>>,
    ),
    Rename(
        KeywordId,
        String,
        i64,
        SyncSender<Result<LibraryKeywordRecord, CatalogError>>,
    ),
    Move(
        KeywordId,
        Option<KeywordId>,
        i64,
        SyncSender<Result<LibraryKeywordRecord, CatalogError>>,
    ),
    DeleteSubtree(
        KeywordId,
        SyncSender<Result<LibraryKeywordDeletionReceipt, CatalogError>>,
    ),
    Tree(SyncSender<Result<Vec<LibraryKeywordRecord>, CatalogError>>),
    ForPhoto(
        PhotoId,
        SyncSender<Result<Vec<LibraryPhotoKeyword>, CatalogError>>,
    ),
    Assign {
        keyword_id: KeywordId,
        photo_ids: Vec<PhotoId>,
        origin: LibraryKeywordAssignmentOrigin,
        source_label: String,
        confidence_milli: Option<u16>,
        now_ms: i64,
        response: SyncSender<Result<LibraryKeywordMutationReceipt, CatalogError>>,
    },
    Remove(
        KeywordId,
        Vec<PhotoId>,
        SyncSender<Result<LibraryKeywordMutationReceipt, CatalogError>>,
    ),
}
