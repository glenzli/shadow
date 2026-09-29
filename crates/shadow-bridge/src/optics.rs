//! Optical correction settings, profile discovery, and native execution receipts.

use std::path::Path;

use shadow_domain::RawMetadataSnapshot;

use super::{BridgeError, ffi};

/// Persisted input-transform contract understood by the C++ optics provider.
pub const OPTICS_SETTINGS_SCHEMA_VERSION: u32 = 1;

#[derive(Debug, Clone, Eq, PartialEq, Hash)]
#[allow(clippy::struct_excessive_bools)] // Mirrors independent persisted correction switches.
pub struct OpticsSettings {
    pub enabled: bool,
    pub correct_distortion: bool,
    pub correct_tca: bool,
    pub correct_vignetting: bool,
    pub automatic_scale: bool,
    pub manual_distortion: i16,
    pub manual_tca_red_cyan: i16,
    pub manual_tca_blue_yellow: i16,
    pub manual_vignetting_amount: i16,
    pub manual_vignetting_midpoint: u8,
    pub camera_profile_maker: String,
    pub camera_profile_model: String,
    pub lens_profile_maker: String,
    pub lens_profile_model: String,
}

impl Default for OpticsSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            correct_distortion: true,
            correct_tca: true,
            correct_vignetting: true,
            automatic_scale: true,
            manual_distortion: 0,
            manual_tca_red_cyan: 0,
            manual_tca_blue_yellow: 0,
            manual_vignetting_amount: 0,
            manual_vignetting_midpoint: 50,
            camera_profile_maker: String::new(),
            camera_profile_model: String::new(),
            lens_profile_maker: String::new(),
            lens_profile_model: String::new(),
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
#[allow(clippy::struct_excessive_bools)] // Availability and application are distinct receipt facts.
pub struct OpticsReceipt {
    pub status: String,
    pub provider_id: String,
    pub provider_version: String,
    pub camera_profile: String,
    pub lens_profile: String,
    pub distortion_available: bool,
    pub tca_available: bool,
    pub vignetting_available: bool,
    pub applied_distortion: bool,
    pub applied_tca: bool,
    pub applied_vignetting: bool,
    pub vignetting_used_distance_fallback: bool,
    pub applied_scaling: bool,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct OpticsProfileCandidate {
    pub camera_maker: String,
    pub camera_model: String,
    pub lens_maker: String,
    pub lens_model: String,
}

/// Calibrated photographer-facing presentation of the source `CameraNeutral`.
///
/// Absence means that the persisted metadata cannot be mapped without
/// guessing, normally because no exact DCP camera profile is installed.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct RawWhiteBalancePresentation {
    pub temperature_kelvin: u32,
    pub tint: i16,
}

/// Enumerates Lensfun lenses compatible with the camera identified by a RAW.
/// The result is sorted and deduplicated by stable maker/model identity.
///
/// # Errors
///
/// Returns a path or decoder error when the RAW cannot be opened and inspected.
pub fn query_libraw_optics_profiles(
    path: &Path,
) -> Result<Vec<OpticsProfileCandidate>, BridgeError> {
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    Ok(ffi::query_libraw_optics_profiles_utf8(utf8_path)?
        .into_iter()
        .map(|candidate| OpticsProfileCandidate {
            camera_maker: candidate.camera_maker,
            camera_model: candidate.camera_model,
            lens_maker: candidate.lens_maker,
            lens_model: candidate.lens_model,
        })
        .collect())
}

/// Enumerates optical profiles for a supported photo source selected by Shadow's decoder router.
///
/// RAW sources may return Lensfun candidates. Raster sources return an empty list unless a future
/// source provider can establish safe camera/lens metadata without risking a second correction of
/// already-developed pixels.
///
/// # Errors
///
/// Returns a path or source-router error when the photo cannot be opened and inspected.
pub fn query_photo_optics_profiles(
    path: &Path,
) -> Result<Vec<OpticsProfileCandidate>, BridgeError> {
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    Ok(ffi::query_photo_optics_profiles_utf8(utf8_path)?
        .into_iter()
        .map(|candidate| OpticsProfileCandidate {
            camera_maker: candidate.camera_maker,
            camera_model: candidate.camera_model,
            lens_maker: candidate.lens_maker,
            lens_model: candidate.lens_model,
        })
        .collect())
}

/// Enumerates Lensfun candidates from a previously inspected RAW metadata snapshot.
///
/// Unlike [`query_photo_optics_profiles`], this does not open or decode the source file. It is
/// therefore the preferred path for Library photos and remains usable when the current pixel
/// provider cannot unpack a proprietary compression such as Nikon HE/HE*.
#[must_use]
pub fn query_optics_profiles_from_metadata(
    metadata: &RawMetadataSnapshot,
) -> Vec<OpticsProfileCandidate> {
    ffi::query_optics_profiles_for_metadata(&ffi_metadata_snapshot(metadata))
        .into_iter()
        .map(|candidate| OpticsProfileCandidate {
            camera_maker: candidate.camera_maker,
            camera_model: candidate.camera_model,
            lens_maker: candidate.lens_maker,
            lens_model: candidate.lens_model,
        })
        .collect()
}

#[must_use]
pub fn query_raw_white_balance_presentation_from_metadata(
    metadata: &RawMetadataSnapshot,
) -> Option<RawWhiteBalancePresentation> {
    let presentation =
        ffi::query_raw_white_balance_presentation_for_metadata(&ffi_metadata_snapshot(metadata));
    presentation
        .available
        .then_some(RawWhiteBalancePresentation {
            temperature_kelvin: presentation.temperature_kelvin,
            tint: presentation.tint,
        })
}

pub(crate) fn ffi_metadata_snapshot(metadata: &RawMetadataSnapshot) -> ffi::FfiMetadataSnapshot {
    ffi::FfiMetadataSnapshot {
        make: metadata.make.clone(),
        model: metadata.model.clone(),
        normalized_make: metadata.normalized_make.clone(),
        normalized_model: metadata.normalized_model.clone(),
        dng_version: metadata.dng_version.clone().unwrap_or_default(),
        raw_count: metadata.raw_count,
        raw_dimensions: ffi::FfiDimensions {
            width: metadata.raw_dimensions.width,
            height: metadata.raw_dimensions.height,
        },
        image_dimensions: ffi::FfiDimensions {
            width: metadata.image_dimensions.width,
            height: metadata.image_dimensions.height,
        },
        margins: ffi::FfiMargins {
            left: metadata.margins.left,
            top: metadata.margins.top,
            right: metadata.margins.right,
            bottom: metadata.margins.bottom,
        },
        orientation: metadata.orientation,
        cfa_pattern: metadata.cfa_pattern.clone(),
        sensor_colors: metadata.sensor_colors,
        sensor_bits: metadata.sensor_bits,
        black_level: metadata.black_level,
        white_level: metadata.white_level,
        as_shot_neutral_r: metadata.as_shot_neutral[0],
        as_shot_neutral_g1: metadata.as_shot_neutral[1],
        as_shot_neutral_b: metadata.as_shot_neutral[2],
        as_shot_neutral_g2: metadata.as_shot_neutral[3],
        baseline_exposure: metadata.baseline_exposure,
        iso_speed: metadata.iso_speed,
        exposure_time_seconds: metadata.exposure_time_seconds,
        aperture_f_number: metadata.aperture_f_number,
        focal_length_mm: metadata.focal_length_mm,
        has_focus_observation: metadata.focus_observation.is_some(),
        focus_observation_schema_version: metadata
            .focus_observation
            .as_ref()
            .map_or(0, |observation| observation.schema_version),
        focus_observation_source: metadata
            .focus_observation
            .as_ref()
            .map_or(0, |observation| match observation.source {
                shadow_domain::FocusObservationSource::Unknown => 0,
                shadow_domain::FocusObservationSource::CameraFocusArea => 1,
                shadow_domain::FocusObservationSource::CameraFocusLocation => 2,
            }),
        focus_observation_center_x: metadata
            .focus_observation
            .as_ref()
            .map_or(0.0, |observation| observation.center_x),
        focus_observation_center_y: metadata
            .focus_observation
            .as_ref()
            .map_or(0.0, |observation| observation.center_y),
        focus_observation_width: metadata
            .focus_observation
            .as_ref()
            .map_or(0.0, |observation| observation.width),
        focus_observation_height: metadata
            .focus_observation
            .as_ref()
            .map_or(0.0, |observation| observation.height),
        focus_observation_confirmed: metadata
            .focus_observation
            .as_ref()
            .is_some_and(|observation| observation.focus_confirmed),
        focus_observation_confidence: metadata
            .focus_observation
            .as_ref()
            .map_or(0.0, |observation| observation.confidence),
        captured_at_unix_seconds: metadata.captured_at_unix_seconds,
        has_gps_coordinates: metadata.gps.is_some(),
        gps_latitude_degrees: metadata
            .gps
            .as_ref()
            .map_or(0.0, |gps| gps.latitude_degrees),
        gps_longitude_degrees: metadata
            .gps
            .as_ref()
            .map_or(0.0, |gps| gps.longitude_degrees),
        has_gps_altitude: metadata
            .gps
            .as_ref()
            .is_some_and(|gps| gps.altitude_meters.is_some()),
        gps_altitude_meters: metadata
            .gps
            .as_ref()
            .and_then(|gps| gps.altitude_meters)
            .unwrap_or_default(),
        lens_make: metadata.lens_make.clone(),
        lens_model: metadata.lens_model.clone(),
        focal_length_35mm: metadata.focal_length_35mm,
    }
}

pub(super) fn ffi_optics_settings(settings: &OpticsSettings) -> ffi::FfiOpticsSettings {
    ffi::FfiOpticsSettings {
        schema_version: OPTICS_SETTINGS_SCHEMA_VERSION,
        enabled: settings.enabled,
        correct_distortion: settings.correct_distortion,
        correct_tca: settings.correct_tca,
        correct_vignetting: settings.correct_vignetting,
        automatic_scale: settings.automatic_scale,
        manual_distortion: settings.manual_distortion,
        manual_tca_red_cyan: settings.manual_tca_red_cyan,
        manual_tca_blue_yellow: settings.manual_tca_blue_yellow,
        manual_vignetting_amount: settings.manual_vignetting_amount,
        manual_vignetting_midpoint: settings.manual_vignetting_midpoint,
        camera_profile_maker: settings.camera_profile_maker.clone(),
        camera_profile_model: settings.camera_profile_model.clone(),
        lens_profile_maker: settings.lens_profile_maker.clone(),
        lens_profile_model: settings.lens_profile_model.clone(),
    }
}

pub(super) fn optics_receipt(receipt: ffi::FfiOpticsReceipt) -> OpticsReceipt {
    OpticsReceipt {
        status: receipt.status,
        provider_id: receipt.provider_id,
        provider_version: receipt.provider_version,
        camera_profile: receipt.camera_profile,
        lens_profile: receipt.lens_profile,
        distortion_available: receipt.distortion_available,
        tca_available: receipt.tca_available,
        vignetting_available: receipt.vignetting_available,
        applied_distortion: receipt.applied_distortion,
        applied_tca: receipt.applied_tca,
        applied_vignetting: receipt.applied_vignetting,
        vignetting_used_distance_fallback: receipt.vignetting_used_distance_fallback,
        applied_scaling: receipt.applied_scaling,
    }
}
