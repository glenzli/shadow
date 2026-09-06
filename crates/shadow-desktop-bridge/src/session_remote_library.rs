//! Desktop-session delegations for the remote Library client workflow.

use anyhow::Result as AnyResult;
use shadow_library_sharing::RemoteReviewFlag;

use super::{DesktopSession, ffi};
use crate::remote_library_service::{
    RemoteLibraryPhoto, RemoteLibraryServer, RemoteLibrarySnapshot, RemoteLibrarySyncResult,
    RemoteLibrarySyncStart, RemoteLibrarySyncStep, RemoteMaterialization,
};

impl DesktopSession {
    pub(crate) fn remote_library_snapshot(
        &self,
        connection_id: &str,
    ) -> AnyResult<ffi::FfiRemoteLibrarySnapshot> {
        Ok(project_snapshot(
            self.remote_library.snapshot(connection_id)?,
        ))
    }

    pub(crate) fn sync_remote_library(
        &self,
        connection_id: &str,
        server_address: &str,
        authorization: &str,
    ) -> AnyResult<ffi::FfiRemoteLibrarySyncResult> {
        Ok(project_sync_result(self.remote_library.sync(
            connection_id,
            server_address,
            authorization,
        )?))
    }

    pub(crate) fn begin_remote_library_sync(
        &self,
        connection_id: &str,
        server_address: &str,
        authorization: &str,
    ) -> AnyResult<ffi::FfiRemoteLibrarySyncStart> {
        Ok(project_sync_start(self.remote_library.begin_sync(
            connection_id,
            server_address,
            authorization,
        )?))
    }

    pub(crate) fn step_remote_library_sync(
        &self,
        job_id: u64,
    ) -> AnyResult<ffi::FfiRemoteLibrarySyncStep> {
        Ok(project_sync_step(self.remote_library.sync_step(job_id)?))
    }

    pub(crate) fn cancel_remote_library_sync(&self, job_id: u64) -> AnyResult<bool> {
        self.remote_library.cancel_sync(job_id)
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn set_remote_library_review_state(
        &self,
        connection_id: &str,
        remote_photo_id: &str,
        remote_representation_id: &str,
        flag: ffi::FfiDecisionFlag,
        rating: u8,
        liked: bool,
        color_label: &str,
        updated_at_ms: i64,
    ) -> AnyResult<()> {
        self.remote_library.set_review_state(
            connection_id,
            remote_photo_id,
            remote_representation_id,
            remote_flag(flag),
            rating,
            liked,
            color_label,
            updated_at_ms,
        )
    }

    pub(crate) fn materialize_remote_library_photo(
        &self,
        connection_id: &str,
        server_address: &str,
        authorization: &str,
        remote_photo_id: &str,
        remote_representation_id: &str,
    ) -> AnyResult<ffi::FfiRemoteLibraryMaterialization> {
        Ok(project_materialization(self.remote_library.materialize(
            connection_id,
            server_address,
            authorization,
            remote_photo_id,
            remote_representation_id,
        )?))
    }
}

fn project_sync_result(result: RemoteLibrarySyncResult) -> ffi::FfiRemoteLibrarySyncResult {
    ffi::FfiRemoteLibrarySyncResult {
        snapshot: project_snapshot(result.snapshot),
        page_count: result.page_count,
        photo_count: result.photo_count,
        downloaded_previews: result.downloaded_previews,
        preview_failures: result.preview_failures,
        removed: result.removed,
    }
}

fn project_sync_start(result: RemoteLibrarySyncStart) -> ffi::FfiRemoteLibrarySyncStart {
    ffi::FfiRemoteLibrarySyncStart {
        job_id: result.job_id,
        snapshot: project_snapshot(result.snapshot),
    }
}

fn project_sync_step(result: RemoteLibrarySyncStep) -> ffi::FfiRemoteLibrarySyncStep {
    ffi::FfiRemoteLibrarySyncStep {
        job_id: result.job_id,
        snapshot: project_snapshot(result.snapshot),
        stage: result.stage,
        page_count: result.page_count,
        photo_count: result.photo_count,
        preview_completed_count: result.preview_completed_count,
        downloaded_previews: result.downloaded_previews,
        preview_failures: result.preview_failures,
        removed: result.removed,
        manifest_complete: result.manifest_complete,
        complete: result.complete,
        diagnostic: result.diagnostic,
    }
}

fn project_snapshot(snapshot: RemoteLibrarySnapshot) -> ffi::FfiRemoteLibrarySnapshot {
    ffi::FfiRemoteLibrarySnapshot {
        has_server: snapshot.server.is_some(),
        server: snapshot.server.map_or_else(empty_server, project_server),
        photos: snapshot.photos.into_iter().map(project_photo).collect(),
    }
}

fn project_server(server: RemoteLibraryServer) -> ffi::FfiRemoteLibraryServer {
    ffi::FfiRemoteLibraryServer {
        server_id: server.server_id,
        display_name: server.display_name,
        embedded_previews_available: server.capabilities.serves_embedded_previews.is_available(),
        generated_proxies_available: server.capabilities.serves_generated_proxies.is_available(),
        originals_available: server.capabilities.serves_originals.is_available(),
        private_preview_provider_available: server
            .capabilities
            .private_preview_provider
            .is_available(),
    }
}

fn empty_server() -> ffi::FfiRemoteLibraryServer {
    ffi::FfiRemoteLibraryServer {
        server_id: String::new(),
        display_name: String::new(),
        embedded_previews_available: false,
        generated_proxies_available: false,
        originals_available: false,
        private_preview_provider_available: false,
    }
}

fn project_photo(photo: RemoteLibraryPhoto) -> ffi::FfiRemoteLibraryPhoto {
    let local_source = photo.local_source;
    let metadata = photo.metadata;
    let raw_dimensions = metadata.raw_dimensions.unwrap_or_default();
    let image_dimensions = metadata.image_dimensions.unwrap_or_default();
    let gps = metadata.gps;
    ffi::FfiRemoteLibraryPhoto {
        server_id: photo.server_id,
        remote_photo_id: photo.remote_photo_id.to_string(),
        remote_representation_id: photo.remote_representation_id.to_string(),
        title: photo.title,
        source_byte_len: photo.source_byte_len,
        has_source_modified_at: photo.source_modified_at_ms.is_some(),
        source_modified_at_ms: photo.source_modified_at_ms.unwrap_or_default(),
        has_original_identity: photo.original_digest_blake3.is_some(),
        original_digest_hex: photo
            .original_digest_blake3
            .map_or_else(String::new, hex_digest),
        representation_count: photo.representation_count,
        source_location_count: photo.source_location_count,
        has_raw_representation: photo.has_raw_representation,
        has_raster_representation: photo.has_raster_representation,
        has_preview: photo.preview_path.is_some(),
        preview_path: photo
            .preview_path
            .map_or_else(String::new, |path| path.to_string_lossy().into_owned()),
        preview_role: photo.preview_role,
        preview_width: photo.preview_width,
        preview_height: photo.preview_height,
        preview_auto_transform: photo.preview_auto_transform,
        preview_unavailable_reason: photo.preview_unavailable_reason,
        metadata_schema_version: metadata.schema_version,
        has_captured_at: metadata.captured_at_unix_seconds.is_some(),
        captured_at_unix_seconds: metadata.captured_at_unix_seconds.unwrap_or_default(),
        camera_make: metadata.camera_make,
        camera_model: metadata.camera_model,
        lens_make: metadata.lens_make,
        lens_model: metadata.lens_model,
        has_iso_speed: metadata.iso_speed.is_some(),
        iso_speed: metadata.iso_speed.unwrap_or_default(),
        has_exposure_time: metadata.exposure_time_seconds.is_some(),
        exposure_time_seconds: metadata.exposure_time_seconds.unwrap_or_default(),
        has_aperture: metadata.aperture_f_number.is_some(),
        aperture_f_number: metadata.aperture_f_number.unwrap_or_default(),
        has_focal_length: metadata.focal_length_mm.is_some(),
        focal_length_mm: metadata.focal_length_mm.unwrap_or_default(),
        has_focal_length_35mm: metadata.focal_length_35mm.is_some(),
        focal_length_35mm: metadata.focal_length_35mm.unwrap_or_default(),
        has_raw_dimensions: metadata.raw_dimensions.is_some(),
        raw_width: raw_dimensions.width,
        raw_height: raw_dimensions.height,
        has_image_dimensions: metadata.image_dimensions.is_some(),
        image_width: image_dimensions.width,
        image_height: image_dimensions.height,
        has_orientation: metadata.orientation.is_some(),
        orientation: metadata.orientation.unwrap_or_default(),
        has_coordinates: gps.is_some(),
        latitude_degrees: gps
            .as_ref()
            .map_or(0.0, |coordinates| coordinates.latitude_degrees),
        longitude_degrees: gps
            .as_ref()
            .map_or(0.0, |coordinates| coordinates.longitude_degrees),
        has_altitude: gps
            .as_ref()
            .is_some_and(|coordinates| coordinates.altitude_meters.is_some()),
        altitude_meters: gps
            .and_then(|coordinates| coordinates.altitude_meters)
            .unwrap_or_default(),
        decision_flag: ffi_flag(photo.review_state.flag),
        decision_rating: photo.review_state.rating,
        liked: photo.review_state.liked,
        color_label: photo.review_state.color_label,
        review_updated_at_ms: photo.review_state.updated_at_ms,
        has_cached_original: local_source.is_some(),
        local_photo_id: local_source
            .as_ref()
            .map_or_else(String::new, |source| source.photo_id.to_string()),
        local_representation_id: local_source
            .as_ref()
            .map_or_else(String::new, |source| source.representation_id.to_string()),
        local_source_path: local_source.map_or_else(String::new, |source| source.native_path),
    }
}

fn hex_digest(digest: [u8; 32]) -> String {
    use std::fmt::Write as _;

    let mut encoded = String::with_capacity(64);
    for byte in digest {
        write!(&mut encoded, "{byte:02x}").expect("writing into a String cannot fail");
    }
    encoded
}

fn project_materialization(
    materialization: RemoteMaterialization,
) -> ffi::FfiRemoteLibraryMaterialization {
    let metadata = materialization.metadata;
    let raw_dimensions = metadata.raw_dimensions.unwrap_or_default();
    let image_dimensions = metadata.image_dimensions.unwrap_or_default();
    let gps = metadata.gps;
    ffi::FfiRemoteLibraryMaterialization {
        local_photo_id: materialization.local_photo_id.to_string(),
        local_representation_id: materialization.local_representation_id.to_string(),
        local_source_path: materialization.native_path.to_string_lossy().into_owned(),
        title: materialization.title,
        reused_existing: materialization.reused_existing,
        inspection_diagnostic: materialization.inspection_diagnostic,
        metadata_schema_version: metadata.schema_version,
        has_captured_at: metadata.captured_at_unix_seconds.is_some(),
        captured_at_unix_seconds: metadata.captured_at_unix_seconds.unwrap_or_default(),
        camera_make: metadata.camera_make,
        camera_model: metadata.camera_model,
        lens_make: metadata.lens_make,
        lens_model: metadata.lens_model,
        has_iso_speed: metadata.iso_speed.is_some(),
        iso_speed: metadata.iso_speed.unwrap_or_default(),
        has_exposure_time: metadata.exposure_time_seconds.is_some(),
        exposure_time_seconds: metadata.exposure_time_seconds.unwrap_or_default(),
        has_aperture: metadata.aperture_f_number.is_some(),
        aperture_f_number: metadata.aperture_f_number.unwrap_or_default(),
        has_focal_length: metadata.focal_length_mm.is_some(),
        focal_length_mm: metadata.focal_length_mm.unwrap_or_default(),
        has_focal_length_35mm: metadata.focal_length_35mm.is_some(),
        focal_length_35mm: metadata.focal_length_35mm.unwrap_or_default(),
        has_raw_dimensions: metadata.raw_dimensions.is_some(),
        raw_width: raw_dimensions.width,
        raw_height: raw_dimensions.height,
        has_image_dimensions: metadata.image_dimensions.is_some(),
        image_width: image_dimensions.width,
        image_height: image_dimensions.height,
        has_orientation: metadata.orientation.is_some(),
        orientation: metadata.orientation.unwrap_or_default(),
        has_coordinates: gps.is_some(),
        latitude_degrees: gps
            .as_ref()
            .map_or(0.0, |coordinates| coordinates.latitude_degrees),
        longitude_degrees: gps
            .as_ref()
            .map_or(0.0, |coordinates| coordinates.longitude_degrees),
        has_altitude: gps
            .as_ref()
            .is_some_and(|coordinates| coordinates.altitude_meters.is_some()),
        altitude_meters: gps
            .and_then(|coordinates| coordinates.altitude_meters)
            .unwrap_or_default(),
    }
}

const fn remote_flag(flag: ffi::FfiDecisionFlag) -> RemoteReviewFlag {
    match flag {
        ffi::FfiDecisionFlag::Picked => RemoteReviewFlag::Picked,
        ffi::FfiDecisionFlag::Rejected => RemoteReviewFlag::Rejected,
        _ => RemoteReviewFlag::Unflagged,
    }
}

fn ffi_flag(flag: RemoteReviewFlag) -> ffi::FfiDecisionFlag {
    match flag {
        RemoteReviewFlag::Unflagged => ffi::FfiDecisionFlag::Unflagged,
        RemoteReviewFlag::Picked => ffi::FfiDecisionFlag::Picked,
        RemoteReviewFlag::Rejected => ffi::FfiDecisionFlag::Rejected,
    }
}
