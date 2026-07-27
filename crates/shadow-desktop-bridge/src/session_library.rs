//! Desktop-session CXX delegations for Library browsing, albums, and source relinking.

use anyhow::Result as AnyResult;

use super::{
    DesktopSession, ffi, relink_service::VerifiedSourceRelinkReceipt, wall_clock::current_time_ms,
};

fn ffi_verified_source_relink_receipt(
    source: VerifiedSourceRelinkReceipt,
) -> ffi::FfiVerifiedSourceRelinkReceipt {
    ffi::FfiVerifiedSourceRelinkReceipt {
        photo_id: source.photo_id,
        representation_id: source.representation_id,
        location_id: source.location_id,
        display_path: source.display_path,
    }
}

impl DesktopSession {
    pub(crate) fn library_photo_page(
        &self,
        filter: &ffi::FfiLibraryPhotoFilter,
        cursor: &ffi::FfiLibraryPhotoCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryPhotoPage> {
        self.library.photo_page(&self.review, filter, cursor, limit)
    }

    pub(crate) fn library_photo_count(
        &self,
        filter: &ffi::FfiLibraryPhotoFilter,
    ) -> AnyResult<u64> {
        self.library.photo_count(filter)
    }

    pub(crate) fn library_facet_page(
        &self,
        filter: &ffi::FfiLibraryPhotoFilter,
        kind: ffi::FfiLibraryFacetKind,
        cursor: &ffi::FfiLibraryFacetCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryFacetPage> {
        self.library.facet_page(filter, kind, cursor, limit)
    }

    pub(crate) fn library_albums(&self) -> AnyResult<Vec<ffi::FfiLibraryAlbum>> {
        self.library.ffi_albums()
    }

    pub(crate) fn library_source_health(&self) -> AnyResult<Vec<ffi::FfiLibrarySourceHealth>> {
        self.library.ffi_source_health()
    }

    pub(crate) fn missing_source_location_page(
        &self,
        scan_session_id: &str,
        after_location_id: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiMissingSourceLocationPage> {
        self.library
            .ffi_missing_source_location_page(scan_session_id, after_location_id, limit)
    }

    pub(crate) fn relink_missing_source_location(
        &self,
        scan_session_id: &str,
        location_id: &str,
        candidate_path: &str,
    ) -> AnyResult<ffi::FfiVerifiedSourceRelinkReceipt> {
        let receipt = self.relink.relink_missing_source_location(
            scan_session_id,
            location_id,
            candidate_path,
        )?;
        Ok(ffi_verified_source_relink_receipt(receipt))
    }

    pub(crate) fn create_manual_library_album(
        &self,
        name: &str,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        self.library
            .create_manual_album_ffi(name, current_time_ms()?)
    }

    pub(crate) fn create_smart_library_album(
        &self,
        name: &str,
        query_filter: &ffi::FfiLibraryPhotoFilter,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        self.library
            .create_smart_album_ffi(name, query_filter, current_time_ms()?)
    }

    pub(crate) fn rename_library_album(
        &self,
        album_id: &str,
        name: &str,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        self.library
            .rename_album_ffi(album_id, name, current_time_ms()?)
    }

    pub(crate) fn replace_smart_library_album_filter(
        &self,
        album_id: &str,
        query_filter: &ffi::FfiLibraryPhotoFilter,
    ) -> AnyResult<ffi::FfiLibraryAlbum> {
        self.library
            .replace_smart_album_query_ffi(album_id, query_filter, current_time_ms()?)
    }

    pub(crate) fn delete_library_album(&self, album_id: &str) -> AnyResult<bool> {
        self.library.delete_album(album_id)
    }

    pub(crate) fn add_photo_to_manual_library_album(
        &self,
        album_id: &str,
        photo_id: &str,
    ) -> AnyResult<()> {
        let now_ms = current_time_ms()?;
        self.library
            .add_photo_to_manual_album(album_id, photo_id, now_ms, now_ms)
    }

    pub(crate) fn remove_photo_from_manual_library_album(
        &self,
        album_id: &str,
        photo_id: &str,
    ) -> AnyResult<bool> {
        self.library
            .remove_photo_from_manual_album(album_id, photo_id)
    }

    pub(crate) fn library_albums_for_photo(
        &self,
        photo_id: &str,
    ) -> AnyResult<Vec<ffi::FfiLibraryAlbum>> {
        self.library.ffi_albums_for_photo(photo_id)
    }

    pub(crate) fn smart_library_photo_page(
        &self,
        album_id: &str,
        cursor: &ffi::FfiLibraryPhotoCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryPhotoPage> {
        self.library
            .smart_album_photo_page(&self.review, album_id, cursor, limit)
    }

    pub(crate) fn smart_library_photo_count(&self, album_id: &str) -> AnyResult<u64> {
        self.library.smart_album_photo_count(album_id)
    }

    pub(crate) fn set_photo_library_state(
        &self,
        photo_id: &str,
        liked: bool,
        color_label: &str,
    ) -> AnyResult<ffi::FfiPhotoLibraryState> {
        self.library
            .set_photo_library_state(photo_id, liked, color_label, current_time_ms()?)
    }
}
