//! Host-selected path transport for the CXX image boundary.
//!
//! Unix borrows exact `OsStr` bytes. Other platforms retain the UTF-8 compatibility contract;
//! Windows UTF-16 is deliberately not inferred from `OsStr`'s opaque encoded representation.

use std::path::Path;

use super::{BridgeError, ffi};

#[cfg(unix)]
use ffi::{
    open_libraw_unix_bytes as open_libraw_wire, open_photo_unix_bytes as open_photo_wire,
    prepare_edit_preview_with_staged_raw_development_plan_from_metadata_unix_bytes as prepare_metadata_wire,
    query_libraw_optics_profiles_unix_bytes as query_libraw_wire,
    query_photo_optics_profiles_unix_bytes as query_photo_wire,
    render_photo_reference_proxy_unix_bytes as render_reference_wire,
};
#[cfg(not(unix))]
use ffi::{
    open_libraw_utf8 as open_libraw_wire, open_photo_utf8 as open_photo_wire,
    prepare_edit_preview_with_staged_raw_development_plan_from_metadata as prepare_metadata_wire,
    query_libraw_optics_profiles_utf8 as query_libraw_wire,
    query_photo_optics_profiles_utf8 as query_photo_wire,
    render_photo_reference_proxy as render_reference_wire,
};

#[cfg(unix)]
type BridgePath<'a> = &'a [u8];
#[cfg(not(unix))]
type BridgePath<'a> = &'a str;

fn bridge_path(path: &Path) -> Result<BridgePath<'_>, BridgeError> {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        let bytes = path.as_os_str().as_bytes();
        if bytes.contains(&0) {
            return Err(BridgeError::InvalidPath("path contains an embedded NUL"));
        }
        Ok(bytes)
    }
    #[cfg(not(unix))]
    {
        let text = path
            .to_str()
            .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
        if text.as_bytes().contains(&0) {
            return Err(BridgeError::InvalidPath("path contains an embedded NUL"));
        }
        Ok(text)
    }
}

// Source-backed staged preparation validates the manifest before opening its source.
pub(super) fn validate(path: &Path) -> Result<(), BridgeError> {
    bridge_path(path).map(|_| ())
}

pub(super) fn open_libraw(path: &Path) -> Result<cxx::UniquePtr<ffi::DecodeHandle>, BridgeError> {
    open_libraw_wire(bridge_path(path)?).map_err(Into::into)
}

pub(super) fn open_photo(path: &Path) -> Result<cxx::UniquePtr<ffi::DecodeHandle>, BridgeError> {
    open_photo_wire(bridge_path(path)?).map_err(Into::into)
}

pub(super) fn query_libraw_optics_profiles(
    path: &Path,
) -> Result<Vec<ffi::FfiOpticsProfileCandidate>, BridgeError> {
    query_libraw_wire(bridge_path(path)?).map_err(Into::into)
}

pub(super) fn query_photo_optics_profiles(
    path: &Path,
) -> Result<Vec<ffi::FfiOpticsProfileCandidate>, BridgeError> {
    query_photo_wire(bridge_path(path)?).map_err(Into::into)
}

pub(super) fn render_photo_reference_proxy(
    path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<ffi::FfiEncodedProxy, BridgeError> {
    render_reference_wire(bridge_path(path)?, max_edge, jpeg_quality).map_err(Into::into)
}

pub(super) fn prepare_preview_from_metadata(
    metadata: &ffi::FfiMetadataSnapshot,
    max_edge: u32,
    plan: &ffi::FfiRawDevelopmentPlan,
    staging_manifest: &Path,
    optics: &ffi::FfiOpticsSettings,
) -> Result<cxx::UniquePtr<ffi::EditPreviewHandle>, BridgeError> {
    prepare_metadata_wire(
        metadata,
        max_edge,
        plan,
        bridge_path(staging_manifest)?,
        optics,
    )
    .map_err(Into::into)
}

pub(super) fn prepare_staged_preview(
    handle: &ffi::DecodeHandle,
    max_edge: u32,
    plan: &ffi::FfiRawDevelopmentPlan,
    staging_manifest: &Path,
) -> Result<cxx::UniquePtr<ffi::EditPreviewHandle>, BridgeError> {
    #[cfg(unix)]
    let prepare =
        ffi::DecodeHandle::prepare_edit_preview_with_staged_raw_development_plan_unix_bytes;
    #[cfg(not(unix))]
    let prepare = ffi::DecodeHandle::prepare_edit_preview_with_staged_raw_development_plan;
    prepare(handle, max_edge, plan, bridge_path(staging_manifest)?).map_err(Into::into)
}

pub(super) fn prepare_staged_preview_with_foundation(
    handle: &ffi::DecodeHandle,
    max_edge: u32,
    plan: &ffi::FfiRawDevelopmentPlan,
    foundation: &ffi::FfiRawFoundation,
    staging_manifest: &Path,
) -> Result<cxx::UniquePtr<ffi::EditPreviewHandle>, BridgeError> {
    #[cfg(unix)]
    let prepare = ffi::DecodeHandle::prepare_edit_preview_with_staged_raw_foundation_unix_bytes;
    #[cfg(not(unix))]
    let prepare = ffi::DecodeHandle::prepare_edit_preview_with_staged_raw_foundation;
    prepare(
        handle,
        max_edge,
        plan,
        foundation,
        bridge_path(staging_manifest)?,
    )
    .map_err(Into::into)
}

pub(super) fn prepare_staged_detail(
    handle: &ffi::DecodeHandle,
    plan: &ffi::FfiRawDevelopmentPlan,
    staging_manifest: &Path,
    requirements: ffi::FfiDetailSessionRequirements,
) -> Result<cxx::UniquePtr<ffi::FullEditDetailHandle>, BridgeError> {
    #[cfg(unix)]
    let prepare =
        ffi::DecodeHandle::prepare_edit_detail_with_staged_raw_development_plan_unix_bytes;
    #[cfg(not(unix))]
    let prepare = ffi::DecodeHandle::prepare_edit_detail_with_staged_raw_development_plan;
    prepare(handle, plan, bridge_path(staging_manifest)?, &requirements).map_err(Into::into)
}

pub(super) fn prepare_staged_detail_with_foundation(
    handle: &ffi::DecodeHandle,
    plan: &ffi::FfiRawDevelopmentPlan,
    foundation: &ffi::FfiRawFoundation,
    staging_manifest: &Path,
    requirements: ffi::FfiDetailSessionRequirements,
) -> Result<cxx::UniquePtr<ffi::FullEditDetailHandle>, BridgeError> {
    #[cfg(unix)]
    let prepare = ffi::DecodeHandle::prepare_edit_detail_with_staged_raw_foundation_unix_bytes;
    #[cfg(not(unix))]
    let prepare = ffi::DecodeHandle::prepare_edit_detail_with_staged_raw_foundation;
    prepare(
        handle,
        plan,
        foundation,
        bridge_path(staging_manifest)?,
        &requirements,
    )
    .map_err(Into::into)
}

#[cfg(test)]
mod tests;
