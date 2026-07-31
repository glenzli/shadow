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
        order: ffi::FfiLibraryPhotoOrder,
        cursor: &ffi::FfiLibraryPhotoCursor,
        limit: u32,
    ) -> AnyResult<ffi::FfiLibraryPhotoPage> {
        self.library
            .photo_page(&self.review, filter, order, cursor, limit)
    }

    pub(crate) fn library_photo_count(
        &self,
        filter: &ffi::FfiLibraryPhotoFilter,
    ) -> AnyResult<u64> {
        self.library.photo_count(filter)
    }

    pub(crate) fn archive_library_photo(&self, photo_id: &str) -> AnyResult<bool> {
        self.library.archive_photo(photo_id)
    }

    // CXX exposes viewport bounds as scalar ABI fields; the service immediately
    // groups them into the two domain value objects below.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn library_map_snapshot(
        &self,
        filter: &ffi::FfiLibraryPhotoFilter,
        south_latitude_e7: i32,
        west_longitude_e7: i32,
        north_latitude_e7: i32,
        east_longitude_e7: i32,
        columns: u16,
        rows: u16,
    ) -> AnyResult<ffi::FfiLibraryMapSnapshot> {
        self.library.map_snapshot(
            filter,
            shadow_catalog::LibraryMapViewport {
                south_latitude_e7,
                west_longitude_e7,
                north_latitude_e7,
                east_longitude_e7,
            },
            shadow_catalog::LibraryMapGrid { columns, rows },
        )
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

    pub(crate) fn remove_library_source(&self, source_id: &str) -> AnyResult<bool> {
        self.library.remove_source(source_id)
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

    pub(crate) fn relink_library_source_location(
        &self,
        location_id: &str,
        candidate_path: &str,
    ) -> AnyResult<ffi::FfiVerifiedSourceRelinkReceipt> {
        Ok(ffi_verified_source_relink_receipt(
            self.relink
                .relink_library_source_location(location_id, candidate_path)?,
        ))
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

    pub(crate) fn library_keywords(&self) -> AnyResult<Vec<ffi::FfiLibraryKeyword>> {
        self.library.ffi_keyword_tree()
    }

    pub(crate) fn library_keywords_for_photo(
        &self,
        photo_id: &str,
    ) -> AnyResult<Vec<ffi::FfiLibraryPhotoKeyword>> {
        self.library.ffi_keywords_for_photo(photo_id)
    }

    pub(crate) fn create_library_keyword(
        &self,
        parent_id: &str,
        name: &str,
    ) -> AnyResult<ffi::FfiLibraryKeyword> {
        self.library
            .create_keyword_ffi(parent_id, name, current_time_ms()?)
    }

    pub(crate) fn rename_library_keyword(
        &self,
        keyword_id: &str,
        name: &str,
    ) -> AnyResult<ffi::FfiLibraryKeyword> {
        self.library
            .rename_keyword_ffi(keyword_id, name, current_time_ms()?)
    }

    pub(crate) fn move_library_keyword(
        &self,
        keyword_id: &str,
        parent_id: &str,
    ) -> AnyResult<ffi::FfiLibraryKeyword> {
        self.library
            .move_keyword_ffi(keyword_id, parent_id, current_time_ms()?)
    }

    pub(crate) fn delete_library_keyword_subtree(
        &self,
        keyword_id: &str,
    ) -> AnyResult<ffi::FfiLibraryKeywordDeletionReceipt> {
        self.library.delete_keyword_subtree_ffi(keyword_id)
    }

    // CXX transfers QStringList-equivalent values as an owned Rust vector.
    #[allow(clippy::needless_pass_by_value)]
    pub(crate) fn assign_library_keyword(
        &self,
        keyword_id: &str,
        photo_ids: Vec<String>,
    ) -> AnyResult<ffi::FfiLibraryKeywordMutationReceipt> {
        self.library
            .assign_manual_keyword_ffi(keyword_id, &photo_ids, current_time_ms()?)
    }

    // CXX transfers QStringList-equivalent values as an owned Rust vector.
    #[allow(clippy::needless_pass_by_value)]
    pub(crate) fn remove_library_keyword(
        &self,
        keyword_id: &str,
        photo_ids: Vec<String>,
    ) -> AnyResult<ffi::FfiLibraryKeywordMutationReceipt> {
        self.library.remove_keyword_ffi(keyword_id, &photo_ids)
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

    pub(crate) fn library_metadata_state(
        &self,
        photo_id: &str,
    ) -> AnyResult<ffi::FfiLibraryMetadataState> {
        self.library.metadata_state(photo_id)
    }

    pub(crate) fn set_library_capture_time_override(
        &self,
        photo_id: &str,
        mode: &str,
        captured_at_unix_seconds: i64,
    ) -> AnyResult<ffi::FfiLibraryMetadataState> {
        self.library.set_capture_time_override(
            photo_id,
            mode,
            captured_at_unix_seconds,
            current_time_ms()?,
        )
    }

    pub(crate) fn set_library_coordinates_override(
        &self,
        photo_id: &str,
        mode: &str,
        latitude_degrees: f64,
        longitude_degrees: f64,
        place_name: &str,
    ) -> AnyResult<ffi::FfiLibraryMetadataState> {
        self.library.set_coordinates_override(
            photo_id,
            mode,
            latitude_degrees,
            longitude_degrees,
            place_name,
            current_time_ms()?,
        )
    }

    pub(crate) fn preview_library_capture_time_batch(
        &self,
        targets: Vec<ffi::FfiBatchPhotoTarget>,
        mode: &str,
        offset_seconds: i64,
    ) -> AnyResult<ffi::FfiCaptureTimeBatchPreview> {
        self.library
            .preview_capture_time_batch(targets, mode, offset_seconds)
    }

    pub(crate) fn apply_library_capture_time_batch(
        &self,
        preview_id: &str,
    ) -> AnyResult<ffi::FfiLibraryMetadataBatchReceipt> {
        self.library
            .apply_capture_time_batch(preview_id, current_time_ms()?)
    }

    pub(crate) fn preview_library_gpx_import(
        &self,
        gpx_path: &str,
        targets: Vec<ffi::FfiBatchPhotoTarget>,
        camera_clock_offset_seconds: i64,
        maximum_gap_seconds: u32,
    ) -> AnyResult<ffi::FfiGpxImportPreview> {
        self.library.preview_gpx_import(
            gpx_path,
            targets,
            shadow_core::GpsMatchSettings {
                camera_clock_offset_seconds,
                maximum_gap_seconds,
            },
        )
    }

    pub(crate) fn apply_library_gpx_import(
        &self,
        preview_id: &str,
    ) -> AnyResult<ffi::FfiLibraryMetadataBatchReceipt> {
        self.library
            .apply_gpx_import(preview_id, current_time_ms()?)
    }
}
