//! Hierarchical Library keyword management and committed photo assignment.
//!
//! This owner keeps string/typed identity conversion, bounded batch admission, and provenance
//! policy together. Public CXX projection is additive wiring over these typed operations.

use anyhow::{Context, Result as AnyResult, bail};
use shadow_catalog::{
    LibraryKeywordAssignmentOrigin, LibraryKeywordDeletionReceipt, LibraryKeywordMutationReceipt,
    LibraryKeywordRecord, LibraryPhotoKeyword,
};
use shadow_domain::{KeywordId, PhotoId};

use crate::ffi;

use super::LibraryService;

impl LibraryService {
    pub(crate) fn keyword_tree(&self) -> AnyResult<Vec<LibraryKeywordRecord>> {
        Ok(self.catalog.library_keyword_tree()?)
    }

    pub(crate) fn ffi_keyword_tree(&self) -> AnyResult<Vec<ffi::FfiLibraryKeyword>> {
        Ok(self.keyword_tree()?.into_iter().map(ffi_keyword).collect())
    }

    pub(crate) fn create_keyword(
        &self,
        parent_id: &str,
        name: &str,
        now_ms: i64,
    ) -> AnyResult<LibraryKeywordRecord> {
        Ok(self.catalog.create_library_keyword(
            optional_keyword_id_from_text(parent_id)?,
            name,
            now_ms,
        )?)
    }

    pub(crate) fn create_keyword_ffi(
        &self,
        parent_id: &str,
        name: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryKeyword> {
        self.create_keyword(parent_id, name, now_ms)
            .map(ffi_keyword)
    }

    pub(crate) fn rename_keyword(
        &self,
        keyword_id: &str,
        name: &str,
        now_ms: i64,
    ) -> AnyResult<LibraryKeywordRecord> {
        Ok(self
            .catalog
            .rename_library_keyword(keyword_id_from_text(keyword_id)?, name, now_ms)?)
    }

    pub(crate) fn rename_keyword_ffi(
        &self,
        keyword_id: &str,
        name: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryKeyword> {
        self.rename_keyword(keyword_id, name, now_ms)
            .map(ffi_keyword)
    }

    pub(crate) fn move_keyword(
        &self,
        keyword_id: &str,
        parent_id: &str,
        now_ms: i64,
    ) -> AnyResult<LibraryKeywordRecord> {
        Ok(self.catalog.move_library_keyword(
            keyword_id_from_text(keyword_id)?,
            optional_keyword_id_from_text(parent_id)?,
            now_ms,
        )?)
    }

    pub(crate) fn move_keyword_ffi(
        &self,
        keyword_id: &str,
        parent_id: &str,
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryKeyword> {
        self.move_keyword(keyword_id, parent_id, now_ms)
            .map(ffi_keyword)
    }

    pub(crate) fn delete_keyword_subtree(
        &self,
        keyword_id: &str,
    ) -> AnyResult<LibraryKeywordDeletionReceipt> {
        Ok(self
            .catalog
            .delete_library_keyword_subtree(keyword_id_from_text(keyword_id)?)?)
    }

    pub(crate) fn delete_keyword_subtree_ffi(
        &self,
        keyword_id: &str,
    ) -> AnyResult<ffi::FfiLibraryKeywordDeletionReceipt> {
        self.delete_keyword_subtree(keyword_id)
            .map(ffi_keyword_deletion_receipt)
    }

    pub(crate) fn keywords_for_photo(&self, photo_id: &str) -> AnyResult<Vec<LibraryPhotoKeyword>> {
        Ok(self
            .catalog
            .library_keywords_for_photo(photo_id_from_text(photo_id)?)?)
    }

    pub(crate) fn ffi_keywords_for_photo(
        &self,
        photo_id: &str,
    ) -> AnyResult<Vec<ffi::FfiLibraryPhotoKeyword>> {
        Ok(self
            .keywords_for_photo(photo_id)?
            .into_iter()
            .map(ffi_photo_keyword)
            .collect())
    }

    pub(crate) fn assign_manual_keyword(
        &self,
        keyword_id: &str,
        photo_ids: &[String],
        now_ms: i64,
    ) -> AnyResult<LibraryKeywordMutationReceipt> {
        let photo_ids = photo_ids_from_text(photo_ids)?;
        Ok(self.catalog.assign_library_keyword_to_photos(
            keyword_id_from_text(keyword_id)?,
            &photo_ids,
            LibraryKeywordAssignmentOrigin::Manual,
            "",
            None,
            now_ms,
        )?)
    }

    pub(crate) fn assign_manual_keyword_ffi(
        &self,
        keyword_id: &str,
        photo_ids: &[String],
        now_ms: i64,
    ) -> AnyResult<ffi::FfiLibraryKeywordMutationReceipt> {
        self.assign_manual_keyword(keyword_id, photo_ids, now_ms)
            .map(ffi_keyword_mutation_receipt)
    }

    pub(crate) fn remove_keyword(
        &self,
        keyword_id: &str,
        photo_ids: &[String],
    ) -> AnyResult<LibraryKeywordMutationReceipt> {
        let photo_ids = photo_ids_from_text(photo_ids)?;
        Ok(self
            .catalog
            .remove_library_keyword_from_photos(keyword_id_from_text(keyword_id)?, &photo_ids)?)
    }

    pub(crate) fn remove_keyword_ffi(
        &self,
        keyword_id: &str,
        photo_ids: &[String],
    ) -> AnyResult<ffi::FfiLibraryKeywordMutationReceipt> {
        self.remove_keyword(keyword_id, photo_ids)
            .map(ffi_keyword_mutation_receipt)
    }
}

pub(super) fn keyword_id_from_text(keyword_id: &str) -> AnyResult<KeywordId> {
    let keyword_id = keyword_id.trim();
    if keyword_id.is_empty() {
        bail!("Library keyword id is required");
    }
    keyword_id
        .parse::<KeywordId>()
        .with_context(|| format!("parse Library keyword id {keyword_id}"))
}

fn optional_keyword_id_from_text(keyword_id: &str) -> AnyResult<Option<KeywordId>> {
    let keyword_id = keyword_id.trim();
    if keyword_id.is_empty() {
        Ok(None)
    } else {
        keyword_id_from_text(keyword_id).map(Some)
    }
}

fn photo_id_from_text(photo_id: &str) -> AnyResult<PhotoId> {
    let photo_id = photo_id.trim();
    if photo_id.is_empty() {
        bail!("Library photo id is required");
    }
    photo_id
        .parse::<PhotoId>()
        .with_context(|| format!("parse Library photo id {photo_id}"))
}

fn photo_ids_from_text(photo_ids: &[String]) -> AnyResult<Vec<PhotoId>> {
    photo_ids
        .iter()
        .map(|photo_id| photo_id_from_text(photo_id))
        .collect()
}

pub(super) fn ffi_keyword(keyword: LibraryKeywordRecord) -> ffi::FfiLibraryKeyword {
    ffi::FfiLibraryKeyword {
        id: keyword.id.to_string(),
        parent_id: keyword
            .parent_id
            .map(|value| value.to_string())
            .unwrap_or_default(),
        name: keyword.name,
        depth: keyword.depth,
        subtree_photo_count: keyword.subtree_photo_count,
        created_at_ms: keyword.created_at_ms,
        updated_at_ms: keyword.updated_at_ms,
    }
}

pub(super) fn ffi_keyword_mutation_receipt(
    receipt: LibraryKeywordMutationReceipt,
) -> ffi::FfiLibraryKeywordMutationReceipt {
    ffi::FfiLibraryKeywordMutationReceipt {
        keyword_id: receipt.keyword_id.to_string(),
        requested_photo_count: receipt.requested_photo_count,
        changed_photo_count: receipt.changed_photo_count,
    }
}

pub(super) fn ffi_keyword_deletion_receipt(
    receipt: LibraryKeywordDeletionReceipt,
) -> ffi::FfiLibraryKeywordDeletionReceipt {
    ffi::FfiLibraryKeywordDeletionReceipt {
        deleted_keyword_count: receipt.deleted_keyword_count,
        deleted_assignment_count: receipt.deleted_assignment_count,
    }
}

fn ffi_photo_keyword(keyword: LibraryPhotoKeyword) -> ffi::FfiLibraryPhotoKeyword {
    let origin = match keyword.origin {
        LibraryKeywordAssignmentOrigin::Manual => ffi::FfiLibraryKeywordOrigin::Manual,
        LibraryKeywordAssignmentOrigin::Imported => ffi::FfiLibraryKeywordOrigin::Imported,
        LibraryKeywordAssignmentOrigin::AiAccepted => ffi::FfiLibraryKeywordOrigin::AiAccepted,
    };
    ffi::FfiLibraryPhotoKeyword {
        keyword: ffi_keyword(keyword.keyword),
        origin,
        source_label: keyword.source_label,
        has_confidence: keyword.confidence_milli.is_some(),
        confidence_milli: keyword.confidence_milli.unwrap_or_default(),
        assigned_at_ms: keyword.assigned_at_ms,
    }
}

#[cfg(test)]
mod tests;
