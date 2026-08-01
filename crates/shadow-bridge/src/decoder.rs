//! Source-neutral inspection, embedded-preview extraction, and shared decoder wire mappings.

use std::path::Path;

use shadow_domain::{
    DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot,
    FocusObservationSnapshot, FocusObservationSource, GpsMetadataSnapshot, ImageDimensions,
    ImageMargins, PendingCorrectionsSnapshot, PreviewCodec, PreviewDescriptorSnapshot,
    RawDevelopmentCapabilitySnapshot, RawMetadataSnapshot,
};

use super::{BridgeError, ffi};

/// Opens a RAW file through the `LibRaw` provider and copies a stable, owned
/// descriptor snapshot into Rust. Pixel buffers intentionally remain on the
/// C++ side at this stage.
///
/// # Errors
///
/// Returns [`BridgeError::NonUtf8Path`] for paths not yet representable by the
/// Mac-first bridge, or a decoder error when the C++ provider cannot open and
/// identify the file.
pub fn inspect_libraw(path: &Path) -> Result<DecoderSnapshot, BridgeError> {
    let handle = open_libraw(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;

    Ok(snapshot(handle))
}

/// Inspects a supported photo through Shadow's source-neutral decoder router.
///
/// The returned snapshot identifies the router as the cache-facing provider. The exact RAW
/// renderer, when any, remains separately available as a prepared session's immutable
/// [`RawDevelopmentReceipt`].
///
/// # Errors
///
/// Returns a path or source-router error when the photo cannot be opened and identified.
pub fn inspect_photo(path: &Path) -> Result<DecoderSnapshot, BridgeError> {
    let handle = open_photo(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    Ok(snapshot(handle))
}

/// Extracts the largest decodable embedded preview selected by the image
/// kernel. Absence of an embedded preview is a successful `None` result.
///
/// # Errors
///
/// Returns [`BridgeError`] when the path cannot cross the Mac-first bridge or
/// the provider fails while decoding the selected preview.
pub fn extract_best_libraw_preview(
    path: &Path,
) -> Result<Option<shadow_domain::PreviewPayload>, BridgeError> {
    let mut handle = open_libraw(path)?;
    if handle.is_null() {
        return Err(BridgeError::NullHandle);
    }
    let payload = handle.pin_mut().decode_best_preview()?;
    if !payload.present {
        return Ok(None);
    }
    Ok(Some(shadow_domain::PreviewPayload {
        descriptor: preview_descriptor(&payload.descriptor),
        byte_order: preview_byte_order(payload.byte_order),
        bytes: payload.bytes,
    }))
}

/// Extracts the router-selected embedded preview from a supported photo source.
///
/// Absence of an embedded preview is a successful `None` result. A raster source may choose to
/// expose no embedded preview and instead rely on [`render_photo_reference_proxy`].
///
/// # Errors
///
/// Returns a path or source-router error when the source cannot be opened or decoded.
pub fn extract_best_photo_preview(
    path: &Path,
) -> Result<Option<shadow_domain::PreviewPayload>, BridgeError> {
    let mut handle = open_photo(path)?;
    if handle.is_null() {
        return Err(BridgeError::NullHandle);
    }
    let payload = handle.pin_mut().decode_best_preview()?;
    if !payload.present {
        return Ok(None);
    }
    Ok(Some(shadow_domain::PreviewPayload {
        descriptor: preview_descriptor(&payload.descriptor),
        byte_order: preview_byte_order(payload.byte_order),
        bytes: payload.bytes,
    }))
}

pub(super) fn open_libraw(path: &Path) -> Result<cxx::UniquePtr<ffi::DecodeHandle>, BridgeError> {
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    ffi::open_libraw_utf8(utf8_path).map_err(Into::into)
}

pub(super) fn open_photo(path: &Path) -> Result<cxx::UniquePtr<ffi::DecodeHandle>, BridgeError> {
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    ffi::open_photo_utf8(utf8_path).map_err(Into::into)
}

fn snapshot(handle: &ffi::DecodeHandle) -> DecoderSnapshot {
    let provider = handle.provider();
    let metadata = handle.metadata();
    let gps = ffi_gps_metadata(&metadata);
    let focus_observation = ffi_focus_observation(&metadata);
    let capabilities = handle.capabilities();
    let previews = handle.previews();

    DecoderSnapshot {
        provider: DecodeProviderSnapshot {
            id: provider.id,
            version: provider.version,
            dng_sdk: provider.dng_sdk,
            rawspeed: provider.rawspeed,
            jpeg: provider.jpeg,
        },
        metadata: RawMetadataSnapshot {
            make: metadata.make,
            model: metadata.model,
            normalized_make: metadata.normalized_make,
            normalized_model: metadata.normalized_model,
            dng_version: (!metadata.dng_version.is_empty()).then_some(metadata.dng_version),
            raw_count: metadata.raw_count,
            raw_dimensions: dimensions(&metadata.raw_dimensions),
            image_dimensions: dimensions(&metadata.image_dimensions),
            margins: ImageMargins {
                left: metadata.margins.left,
                top: metadata.margins.top,
                right: metadata.margins.right,
                bottom: metadata.margins.bottom,
            },
            orientation: metadata.orientation,
            cfa_pattern: metadata.cfa_pattern,
            sensor_colors: metadata.sensor_colors,
            sensor_bits: metadata.sensor_bits,
            black_level: metadata.black_level,
            white_level: metadata.white_level,
            as_shot_neutral: [
                metadata.as_shot_neutral_r,
                metadata.as_shot_neutral_g1,
                metadata.as_shot_neutral_b,
                metadata.as_shot_neutral_g2,
            ],
            baseline_exposure: metadata.baseline_exposure,
            iso_speed: metadata.iso_speed,
            exposure_time_seconds: metadata.exposure_time_seconds,
            aperture_f_number: metadata.aperture_f_number,
            focal_length_mm: metadata.focal_length_mm,
            focus_observation,
            captured_at_unix_seconds: metadata.captured_at_unix_seconds,
            gps,
            lens_make: metadata.lens_make,
            lens_model: metadata.lens_model,
            focal_length_35mm: metadata.focal_length_35mm,
        },
        capabilities: DecodeCapabilitySnapshot {
            metadata: support(capabilities.metadata),
            embedded_previews: support(capabilities.embedded_previews),
            raw_frame: support(capabilities.raw_frame),
            reference_rgb: support(capabilities.reference_rgb),
            pending_corrections: PendingCorrectionsSnapshot {
                dng_opcode_list_bytes: [
                    capabilities.dng_opcode_list_1_bytes,
                    capabilities.dng_opcode_list_2_bytes,
                    capabilities.dng_opcode_list_3_bytes,
                ],
            },
            raw_development: RawDevelopmentCapabilitySnapshot {
                plan_schema_version: capabilities.raw_development.schema_version,
                available: support(capabilities.raw_development.available),
                raw_frame: support(capabilities.raw_development.raw_frame),
                dng_opcode_execution_receipt: support(
                    capabilities.raw_development.dng_opcode_execution_receipt,
                ),
                supported_intents: capabilities.raw_development.supported_intents,
                supported_qualities: capabilities.raw_development.supported_qualities,
                supported_dng_opcode_policies: capabilities
                    .raw_development
                    .supported_dng_opcode_policies,
                supported_noise_reduction_intents: capabilities
                    .raw_development
                    .supported_noise_reduction_intents,
                supported_highlight_recovery_intents: capabilities
                    .raw_development
                    .supported_highlight_recovery_intents,
            },
        },
        previews: previews.iter().map(preview_descriptor).collect(),
    }
}

fn ffi_focus_observation(metadata: &ffi::FfiMetadataSnapshot) -> Option<FocusObservationSnapshot> {
    if !metadata.has_focus_observation {
        return None;
    }
    let source = match metadata.focus_observation_source {
        1 => FocusObservationSource::CameraFocusArea,
        2 => FocusObservationSource::CameraFocusLocation,
        _ => return None,
    };
    let observation = FocusObservationSnapshot {
        schema_version: metadata.focus_observation_schema_version,
        source,
        center_x: metadata.focus_observation_center_x,
        center_y: metadata.focus_observation_center_y,
        width: metadata.focus_observation_width,
        height: metadata.focus_observation_height,
        focus_confirmed: metadata.focus_observation_confirmed,
        confidence: metadata.focus_observation_confidence,
    };
    observation.is_valid().then_some(observation)
}

fn ffi_gps_metadata(metadata: &ffi::FfiMetadataSnapshot) -> Option<GpsMetadataSnapshot> {
    let latitude = metadata.gps_latitude_degrees;
    let longitude = metadata.gps_longitude_degrees;
    if !metadata.has_gps_coordinates
        || !latitude.is_finite()
        || !longitude.is_finite()
        || !(-90.0..=90.0).contains(&latitude)
        || !(-180.0..=180.0).contains(&longitude)
    {
        return None;
    }
    let altitude_meters = (metadata.has_gps_altitude && metadata.gps_altitude_meters.is_finite())
        .then_some(metadata.gps_altitude_meters);
    Some(GpsMetadataSnapshot {
        latitude_degrees: latitude,
        longitude_degrees: longitude,
        altitude_meters,
    })
}

fn preview_descriptor(preview: &ffi::FfiPreviewSnapshot) -> PreviewDescriptorSnapshot {
    PreviewDescriptorSnapshot {
        provider_id: preview.provider_id,
        codec: preview_codec(preview.format),
        dimensions: dimensions(&preview.dimensions),
        bits_per_channel: preview.bits_per_channel,
        channels: preview.channels,
        encoded_bytes: preview.encoded_bytes,
        decodable: preview.decodable,
    }
}

pub(super) fn dimensions(value: &ffi::FfiDimensions) -> ImageDimensions {
    ImageDimensions {
        width: value.width,
        height: value.height,
    }
}

pub(super) fn preview_codec(value: ffi::FfiPreviewFormat) -> PreviewCodec {
    match value {
        ffi::FfiPreviewFormat::Jpeg => PreviewCodec::Jpeg,
        ffi::FfiPreviewFormat::Bitmap => PreviewCodec::Bitmap,
        ffi::FfiPreviewFormat::JpegXl => PreviewCodec::JpegXl,
        ffi::FfiPreviewFormat::H265 => PreviewCodec::H265,
        _ => PreviewCodec::Unknown,
    }
}

fn preview_byte_order(value: ffi::FfiByteOrder) -> shadow_domain::PreviewByteOrder {
    match value {
        ffi::FfiByteOrder::Native => shadow_domain::PreviewByteOrder::Native,
        ffi::FfiByteOrder::LittleEndian => shadow_domain::PreviewByteOrder::LittleEndian,
        ffi::FfiByteOrder::BigEndian => shadow_domain::PreviewByteOrder::BigEndian,
        _ => shadow_domain::PreviewByteOrder::NotApplicable,
    }
}

const fn support(value: bool) -> DecodeSupport {
    if value {
        DecodeSupport::Available
    } else {
        DecodeSupport::Unavailable
    }
}
