//! Real linked source/staging boundaries must preserve native bytes and reject NUL before I/O.

use std::{
    ffi::OsString,
    os::unix::ffi::OsStringExt,
    path::{Path, PathBuf},
};

use shadow_domain::ImageDimensions;

use crate::{
    BridgeError, DetailSessionRequirements, OpticsSettings, PhotoEditDetailSession,
    PhotoEditPreviewSession, RawDevelopmentPlan, RawFoundationArtifactIdentity,
    VerifiedRawFoundation, ffi, inspect_libraw, inspect_photo, native_path,
    query_libraw_optics_profiles, query_photo_optics_profiles,
    raw_development::ffi_raw_development_plan, render_photo_reference_proxy,
};

fn source() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../apps/desktop/assets/lut-preview-reference.jpg")
}

fn native_nul_path() -> PathBuf {
    PathBuf::from(OsString::from_vec(
        b"synthetic-\xff\0-must-not-open".to_vec(),
    ))
}

fn rejects_invalid_path<T>(result: Result<T, BridgeError>) {
    match result {
        Err(BridgeError::InvalidPath(message)) => {
            assert!(message.contains("embedded NUL"), "{message}");
        }
        Err(error) => panic!("safe path admission must reject before decoder I/O: {error}"),
        Ok(_) => panic!("an embedded NUL must never be admitted"),
    }
}

fn rejects_nul<T>(result: Result<T, BridgeError>) {
    match result {
        Err(BridgeError::Decoder(error)) => {
            assert!(error.what().contains("embedded NUL"), "{error}");
        }
        Err(error) => panic!("native NUL must reach the shared fail-closed path codec: {error}"),
        Ok(_) => panic!("an embedded NUL must never reach decoder I/O"),
    }
}

fn foundation() -> VerifiedRawFoundation {
    VerifiedRawFoundation::from_verified_interleaved_camera_rgb(
        ImageDimensions {
            width: 1,
            height: 1,
        },
        0,
        0,
        50,
        RawFoundationArtifactIdentity::from_verified_digests(
            "a".repeat(64),
            "b".repeat(64),
            "c".repeat(64),
        )
        .unwrap(),
        vec![0.25; 3],
    )
    .unwrap()
}

#[test]
fn native_source_entry_points_reject_nul_without_utf8_admission_or_prefix_io() {
    let path = native_nul_path();
    assert!(path.to_str().is_none());
    rejects_invalid_path(inspect_libraw(&path));
    rejects_invalid_path(inspect_photo(&path));
    rejects_invalid_path(query_libraw_optics_profiles(&path));
    rejects_invalid_path(query_photo_optics_profiles(&path));
    rejects_invalid_path(render_photo_reference_proxy(&path, 64, 82));
    rejects_invalid_path(PhotoEditPreviewSession::open(&path, 64));
    rejects_invalid_path(PhotoEditDetailSession::open(&path));
}

#[test]
fn all_native_staging_entry_points_use_the_shared_path_codec() {
    let source = source();
    let metadata = inspect_photo(&source).unwrap().metadata;
    let path = native_nul_path();
    let optics = OpticsSettings::default();
    let foundation = foundation();
    let requirements = DetailSessionRequirements::default();

    rejects_invalid_path(
        PhotoEditPreviewSession::open_with_staged_raw_development_plan_from_metadata(
            &metadata,
            &path,
            64,
            RawDevelopmentPlan::preview(),
            &optics,
        ),
    );
    rejects_invalid_path(
        PhotoEditPreviewSession::open_with_staged_raw_development_plan(
            &source,
            &path,
            64,
            RawDevelopmentPlan::preview(),
            &optics,
        ),
    );
    rejects_invalid_path(PhotoEditPreviewSession::open_with_staged_raw_foundation(
        &source,
        &path,
        64,
        RawDevelopmentPlan::preview(),
        &foundation,
        &optics,
    ));
    rejects_invalid_path(
        PhotoEditDetailSession::open_with_staged_raw_development_plan(
            &source,
            &path,
            RawDevelopmentPlan::detail(),
            &optics,
            requirements,
        ),
    );
    rejects_invalid_path(PhotoEditDetailSession::open_with_staged_raw_foundation(
        &source,
        &path,
        RawDevelopmentPlan::detail(),
        &foundation,
        &optics,
        requirements,
    ));
}

#[test]
fn retained_utf8_wrappers_reject_nul_at_the_same_native_boundary() {
    let path = "synthetic\0-must-not-open";
    rejects_nul(ffi::open_libraw_utf8(path).map_err(Into::into));
    rejects_nul(ffi::open_photo_utf8(path).map_err(Into::into));
    rejects_nul(ffi::query_libraw_optics_profiles_utf8(path).map_err(Into::into));
    rejects_nul(ffi::query_photo_optics_profiles_utf8(path).map_err(Into::into));
    rejects_nul(ffi::render_photo_reference_proxy(path, 64, 82).map_err(Into::into));

    let handle = native_path::open_photo(&source()).unwrap();
    let handle = handle.as_ref().unwrap();
    let metadata = handle.metadata();
    let preview_plan = ffi_raw_development_plan(RawDevelopmentPlan::preview());
    let detail_plan = ffi_raw_development_plan(RawDevelopmentPlan::detail());
    let foundation = foundation();
    let requirements = ffi::FfiDetailSessionRequirements {
        requires_cpu_replay: true,
    };
    let optics = crate::optics::ffi_optics_settings(&OpticsSettings::default());

    rejects_nul(
        ffi::prepare_edit_preview_with_staged_raw_development_plan_from_metadata(
            &metadata,
            64,
            &preview_plan,
            path,
            &optics,
        )
        .map_err(Into::into),
    );
    rejects_nul(
        handle
            .prepare_edit_preview_with_staged_raw_development_plan(64, &preview_plan, path)
            .map_err(Into::into),
    );
    rejects_nul(
        handle
            .prepare_edit_preview_with_staged_raw_foundation(
                64,
                &preview_plan,
                foundation.ffi(),
                path,
            )
            .map_err(Into::into),
    );
    rejects_nul(
        handle
            .prepare_edit_detail_with_staged_raw_development_plan(&detail_plan, path, &requirements)
            .map_err(Into::into),
    );
    rejects_nul(
        handle
            .prepare_edit_detail_with_staged_raw_foundation(
                &detail_plan,
                foundation.ffi(),
                path,
                &requirements,
            )
            .map_err(Into::into),
    );
}

#[test]
fn nonexistent_non_utf8_source_is_a_decoder_error_instead_of_a_text_path_rejection() {
    let path = PathBuf::from(OsString::from_vec(
        b"/shadow-native-path-contract-absent-\xff/source.png".to_vec(),
    ));
    assert!(matches!(inspect_photo(&path), Err(BridgeError::Decoder(_))));
}

#[test]
fn native_cxx_overloads_reject_nul_even_when_called_below_the_safe_adapter() {
    let path = b"synthetic-\xff\0-must-not-open".as_slice();
    rejects_nul(ffi::open_libraw_unix_bytes(path).map_err(Into::into));
    rejects_nul(ffi::open_photo_unix_bytes(path).map_err(Into::into));
    rejects_nul(ffi::query_libraw_optics_profiles_unix_bytes(path).map_err(Into::into));
    rejects_nul(ffi::query_photo_optics_profiles_unix_bytes(path).map_err(Into::into));
    rejects_nul(ffi::render_photo_reference_proxy_unix_bytes(path, 64, 82).map_err(Into::into));

    let handle = native_path::open_photo(&source()).unwrap();
    let handle = handle.as_ref().unwrap();
    let metadata = handle.metadata();
    let preview_plan = ffi_raw_development_plan(RawDevelopmentPlan::preview());
    let detail_plan = ffi_raw_development_plan(RawDevelopmentPlan::detail());
    let foundation = foundation();
    let requirements = ffi::FfiDetailSessionRequirements {
        requires_cpu_replay: true,
    };
    let optics = crate::optics::ffi_optics_settings(&OpticsSettings::default());

    rejects_nul(
        ffi::prepare_edit_preview_with_staged_raw_development_plan_from_metadata_unix_bytes(
            &metadata,
            64,
            &preview_plan,
            path,
            &optics,
        )
        .map_err(Into::into),
    );
    rejects_nul(
        handle
            .prepare_edit_preview_with_staged_raw_development_plan_unix_bytes(
                64,
                &preview_plan,
                path,
            )
            .map_err(Into::into),
    );
    rejects_nul(
        handle
            .prepare_edit_preview_with_staged_raw_foundation_unix_bytes(
                64,
                &preview_plan,
                foundation.ffi(),
                path,
            )
            .map_err(Into::into),
    );
    rejects_nul(
        handle
            .prepare_edit_detail_with_staged_raw_development_plan_unix_bytes(
                &detail_plan,
                path,
                &requirements,
            )
            .map_err(Into::into),
    );
    rejects_nul(
        handle
            .prepare_edit_detail_with_staged_raw_foundation_unix_bytes(
                &detail_plan,
                foundation.ffi(),
                path,
                &requirements,
            )
            .map_err(Into::into),
    );
}

#[test]
fn invalid_staging_path_precedes_source_open_for_source_backed_entry_points() {
    let absent_source = Path::new("/shadow-native-path-contract-source-must-not-open.jpg");
    let invalid_manifest = native_nul_path();
    let optics = OpticsSettings::default();
    let foundation = foundation();
    let requirements = DetailSessionRequirements::default();

    // The source does not exist: reaching it would return Decoder instead of InvalidPath.
    assert!(matches!(
        PhotoEditPreviewSession::open_with_staged_raw_development_plan(
            absent_source,
            &invalid_manifest,
            64,
            RawDevelopmentPlan::preview(),
            &optics,
        ),
        Err(BridgeError::InvalidPath(_))
    ));
    assert!(matches!(
        PhotoEditPreviewSession::open_with_staged_raw_foundation(
            absent_source,
            &invalid_manifest,
            64,
            RawDevelopmentPlan::preview(),
            &foundation,
            &optics,
        ),
        Err(BridgeError::InvalidPath(_))
    ));
    assert!(matches!(
        PhotoEditDetailSession::open_with_staged_raw_development_plan(
            absent_source,
            &invalid_manifest,
            RawDevelopmentPlan::detail(),
            &optics,
            requirements,
        ),
        Err(BridgeError::InvalidPath(_))
    ));
    assert!(matches!(
        PhotoEditDetailSession::open_with_staged_raw_foundation(
            absent_source,
            &invalid_manifest,
            RawDevelopmentPlan::detail(),
            &foundation,
            &optics,
            requirements,
        ),
        Err(BridgeError::InvalidPath(_))
    ));
}
