//! Client adapters for Library keyword management.

use shadow_domain::{KeywordId, PhotoId};

use crate::{
    CatalogError, LibraryKeywordAssignmentOrigin, LibraryKeywordDeletionReceipt,
    LibraryKeywordMutationReceipt, LibraryKeywordRecord, LibraryPhotoKeyword,
};

use super::super::{
    CatalogHandle,
    protocol::{LibraryKeywordsMessage, Message},
};

impl CatalogHandle {
    pub fn create_library_keyword(
        &self,
        parent_id: Option<KeywordId>,
        name: &str,
        now_ms: i64,
    ) -> Result<LibraryKeywordRecord, CatalogError> {
        self.request(|response| {
            Message::LibraryKeywords(LibraryKeywordsMessage::Create(
                parent_id,
                name.to_owned(),
                now_ms,
                response,
            ))
        })
    }

    pub fn rename_library_keyword(
        &self,
        keyword_id: KeywordId,
        name: &str,
        now_ms: i64,
    ) -> Result<LibraryKeywordRecord, CatalogError> {
        self.request(|response| {
            Message::LibraryKeywords(LibraryKeywordsMessage::Rename(
                keyword_id,
                name.to_owned(),
                now_ms,
                response,
            ))
        })
    }

    pub fn move_library_keyword(
        &self,
        keyword_id: KeywordId,
        parent_id: Option<KeywordId>,
        now_ms: i64,
    ) -> Result<LibraryKeywordRecord, CatalogError> {
        self.request(|response| {
            Message::LibraryKeywords(LibraryKeywordsMessage::Move(
                keyword_id, parent_id, now_ms, response,
            ))
        })
    }

    pub fn delete_library_keyword_subtree(
        &self,
        keyword_id: KeywordId,
    ) -> Result<LibraryKeywordDeletionReceipt, CatalogError> {
        self.request(|response| {
            Message::LibraryKeywords(LibraryKeywordsMessage::DeleteSubtree(keyword_id, response))
        })
    }

    pub fn library_keyword_tree(&self) -> Result<Vec<LibraryKeywordRecord>, CatalogError> {
        self.request(|response| Message::LibraryKeywords(LibraryKeywordsMessage::Tree(response)))
    }

    pub fn library_keywords_for_photo(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<LibraryPhotoKeyword>, CatalogError> {
        self.request(|response| {
            Message::LibraryKeywords(LibraryKeywordsMessage::ForPhoto(photo_id, response))
        })
    }

    pub fn assign_library_keyword_to_photos(
        &self,
        keyword_id: KeywordId,
        photo_ids: &[PhotoId],
        origin: LibraryKeywordAssignmentOrigin,
        source_label: &str,
        confidence_milli: Option<u16>,
        now_ms: i64,
    ) -> Result<LibraryKeywordMutationReceipt, CatalogError> {
        self.request(|response| {
            Message::LibraryKeywords(LibraryKeywordsMessage::Assign {
                keyword_id,
                photo_ids: photo_ids.to_vec(),
                origin,
                source_label: source_label.to_owned(),
                confidence_milli,
                now_ms,
                response,
            })
        })
    }

    pub fn remove_library_keyword_from_photos(
        &self,
        keyword_id: KeywordId,
        photo_ids: &[PhotoId],
    ) -> Result<LibraryKeywordMutationReceipt, CatalogError> {
        self.request(|response| {
            Message::LibraryKeywords(LibraryKeywordsMessage::Remove(
                keyword_id,
                photo_ids.to_vec(),
                response,
            ))
        })
    }
}
