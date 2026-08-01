//! Thin `DesktopSession` routing for durable History queries.

use anyhow::Result as AnyResult;

use crate::{DesktopSession, ffi};

impl DesktopSession {
    pub(crate) fn photo_edit_history_page(
        &self,
        photo_id: &str,
        after: &ffi::FfiHistoryCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiPhotoHistoryPage> {
        self.history.photo_page(photo_id, after, limit)
    }

    pub(crate) fn library_edit_history_page(
        &self,
        after: &ffi::FfiHistoryCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryHistoryPage> {
        self.history.library_page(after, limit)
    }

    pub(crate) fn library_edit_history_ref_page(
        &self,
        after_name: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryHistoryRefPage> {
        self.history.library_ref_page(after_name, limit)
    }
}
