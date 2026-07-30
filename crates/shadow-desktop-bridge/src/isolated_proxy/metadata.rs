//! Child-established metadata snapshot protocol, cache, and workflow.

use std::{
    fs, io,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};
use shadow_domain::{GpsMetadataSnapshot, ImageDimensions, ImageMargins, RawMetadataSnapshot};
use uuid::Uuid;

use super::{
    helper_process::{
        HELPER_TIMEOUT, HelperExecution, HelperProcessOutput, execute_decode_helper,
        helper_exit_looks_like_crash, helper_stderr_suffix,
    },
    helper_wire_fields::{
        decode_printable_identity_field, decode_protocol_hex_text_field, parse_metadata_f64,
        parse_metadata_i32, parse_metadata_i64, parse_metadata_u32, parse_metadata_u64,
    },
    persistent_evidence::{
        DecoderSafetyRegistry, IsolatedDecodeObservation, write_atomic_cache_record,
    },
    route_identity::{DecoderSafetyKey, METADATA_SNAPSHOT_PROTOCOL},
};

const MAX_METADATA_TEXT_BYTES: usize = 4 * 1024;
const METADATA_SNAPSHOT_SCHEMA: u8 = 1;
pub(super) const METADATA_SNAPSHOT_FIELD_COUNT: usize = 43;
pub(super) const RAW_METADATA_SNAPSHOT_FIELD_COUNT: usize = 38;

/// A child-established metadata snapshot. The router identity identifies the
/// configured helper-side provider graph; it does not claim that the desktop
/// process may reopen the source with that provider.
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub(crate) struct IsolatedPhotoMetadataSnapshot {
    pub(crate) router_provider_id: String,
    pub(crate) router_provider_version: String,
    pub(crate) metadata: RawMetadataSnapshot,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
struct PersistedMetadataSnapshot {
    schema: u8,
    key: DecoderSafetyKey,
    snapshot: IsolatedPhotoMetadataSnapshot,
}

/// Opens a source and copies its bounded metadata snapshot from the helper.
///
/// It is intended for Catalog and optics callers that otherwise would reopen a
/// RAW merely because an older catalog row lacks EXIF. It returns no preview
/// bytes and no `RawFrame`, and successful completion still does not certify a
/// later in-process detail or export decode.
pub(crate) fn snapshot_isolated_photo_metadata(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
) -> Result<IsolatedPhotoMetadataSnapshot> {
    let safety_key = DecoderSafetyKey::metadata_snapshot(source_path, helper_path)?;
    if let Some(observation) =
        DecoderSafetyRegistry::shared().observation(runtime_cache_root, &safety_key)?
        && observation.quarantines_main_process_native_decode()
    {
        bail!(
            "isolated RAW metadata snapshot is quarantined for this unchanged source after {}; change the source or helper before retrying",
            observation.diagnostic_label()
        );
    }
    if let Some(snapshot) = read_metadata_snapshot(runtime_cache_root, &safety_key)? {
        return Ok(snapshot);
    }

    let nonce = Uuid::now_v7().simple().to_string();
    let execution = execute_decode_helper(helper_path, |command| {
        command
            .arg("metadata-snapshot")
            .arg(source_path)
            .arg(&nonce);
    });
    let (result, observation) = match execution {
        Ok(HelperExecution::TimedOut(output)) => (
            Err(anyhow::anyhow!(
                "isolated RAW metadata snapshot timed out after {} seconds{}",
                HELPER_TIMEOUT.as_secs(),
                helper_stderr_suffix(&output.stderr)
            )),
            IsolatedDecodeObservation::TimedOut,
        ),
        Ok(HelperExecution::Completed(output)) => {
            let result = decode_metadata_snapshot_output(&output, &nonce);
            let observation = if result.is_ok() {
                IsolatedDecodeObservation::LegacyStageSucceededUnproven
            } else if !output.status.success() && helper_exit_looks_like_crash(output.status) {
                IsolatedDecodeObservation::ChildCrashed
            } else if !output.status.success() {
                IsolatedDecodeObservation::ChildRejected
            } else {
                IsolatedDecodeObservation::InvalidOutput
            };
            (result, observation)
        }
        Err(error) => (Err(error), IsolatedDecodeObservation::LaunchFailed),
    };

    let source_is_current = safety_key.is_current(source_path, helper_path);
    if source_is_current {
        // As with all helper-derived records, cache failure only loses a
        // future reuse opportunity; it never suppresses the child diagnosis.
        let _ =
            DecoderSafetyRegistry::shared().record(runtime_cache_root, &safety_key, observation);
        if let Ok(snapshot) = &result {
            let _ = write_metadata_snapshot(runtime_cache_root, &safety_key, snapshot);
        }
    }
    if result.is_ok() && !source_is_current {
        bail!("source or helper changed while the isolated metadata snapshot was running");
    }
    result
}

fn metadata_snapshot_path(runtime_cache_root: &Path, key: &DecoderSafetyKey) -> Result<PathBuf> {
    let key = serde_json::to_vec(key).context("serialize isolated metadata snapshot key")?;
    let digest = blake3::hash(&key).to_hex();
    Ok(runtime_cache_root
        .join("decode-helper")
        .join("metadata-snapshots")
        .join(format!("{digest}.json")))
}

fn read_metadata_snapshot(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
) -> Result<Option<IsolatedPhotoMetadataSnapshot>> {
    let path = metadata_snapshot_path(runtime_cache_root, key)?;
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        // This is an acceleration cache. A permission, partial-write, or
        // future-schema problem must trigger a fresh child read, not convert
        // into a global metadata failure.
        Err(_) => return Ok(None),
    };
    let record: PersistedMetadataSnapshot = match serde_json::from_slice(&bytes) {
        Ok(record) => record,
        Err(_) => return Ok(None),
    };
    if record.schema != METADATA_SNAPSHOT_SCHEMA || record.key != *key {
        return Ok(None);
    }
    Ok(Some(record.snapshot))
}

fn write_metadata_snapshot(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
    snapshot: &IsolatedPhotoMetadataSnapshot,
) -> Result<()> {
    let record = PersistedMetadataSnapshot {
        schema: METADATA_SNAPSHOT_SCHEMA,
        key: key.clone(),
        snapshot: snapshot.clone(),
    };
    let bytes = serde_json::to_vec(&record).context("serialize isolated metadata snapshot")?;
    let path = metadata_snapshot_path(runtime_cache_root, key)?;
    write_atomic_cache_record(&path, &bytes)
}

fn decode_metadata_snapshot_output(
    output: &HelperProcessOutput,
    nonce: &str,
) -> Result<IsolatedPhotoMetadataSnapshot> {
    if !output.status.success() {
        bail!(
            "isolated RAW metadata snapshot exited with {}{}",
            output.status,
            helper_stderr_suffix(&output.stderr)
        );
    }
    parse_metadata_snapshot_protocol(&output.stdout, nonce)
}

fn parse_metadata_snapshot_protocol(
    stdout: &[u8],
    expected_nonce: &str,
) -> Result<IsolatedPhotoMetadataSnapshot> {
    if expected_nonce.is_empty() {
        bail!("isolated metadata snapshot nonce must be non-empty");
    }
    let response =
        std::str::from_utf8(stdout).context("decode isolated RAW metadata snapshot output")?;
    let fields = response.split_whitespace().collect::<Vec<_>>();
    if fields.len() != METADATA_SNAPSHOT_FIELD_COUNT
        || fields[0] != METADATA_SNAPSHOT_PROTOCOL
        || fields[1] != "metadata-snapshot"
        || fields[2] != expected_nonce
    {
        bail!("isolated RAW metadata snapshot returned an invalid protocol response");
    }

    Ok(IsolatedPhotoMetadataSnapshot {
        router_provider_id: decode_printable_identity_field(fields[3], "router provider id")?,
        router_provider_version: decode_printable_identity_field(
            fields[4],
            "router provider version",
        )?,
        metadata: parse_raw_metadata_snapshot_fields(
            &fields[5..],
            MAX_METADATA_TEXT_BYTES,
            "isolated RAW metadata snapshot",
        )?,
    })
}

pub(super) fn parse_raw_metadata_snapshot_fields(
    fields: &[&str],
    maximum_text_bytes: usize,
    protocol_label: &str,
) -> Result<RawMetadataSnapshot> {
    if fields.len() != RAW_METADATA_SNAPSHOT_FIELD_COUNT {
        bail!("{protocol_label} returned an invalid metadata field count");
    }
    let decode_text = |index: usize, label: &str| {
        decode_protocol_hex_text_field(fields[index], label, maximum_text_bytes, true)
    };
    let dng_version = decode_text(4, "DNG version")?;
    let has_gps_coordinates = parse_metadata_bool(fields[30], "GPS coordinates")?;
    let gps_latitude_degrees = parse_metadata_f64(fields[31], "GPS latitude")?;
    let gps_longitude_degrees = parse_metadata_f64(fields[32], "GPS longitude")?;
    let has_gps_altitude = parse_metadata_bool(fields[33], "GPS altitude")?;
    let gps_altitude_meters = parse_metadata_f64(fields[34], "GPS altitude meters")?;
    let gps = valid_gps_snapshot(
        has_gps_coordinates,
        gps_latitude_degrees,
        gps_longitude_degrees,
        has_gps_altitude,
        gps_altitude_meters,
    );
    Ok(RawMetadataSnapshot {
        make: decode_text(0, "make")?,
        model: decode_text(1, "model")?,
        normalized_make: decode_text(2, "normalized make")?,
        normalized_model: decode_text(3, "normalized model")?,
        dng_version: (!dng_version.is_empty()).then_some(dng_version),
        raw_count: parse_metadata_u32(fields[5], "raw count")?,
        raw_dimensions: ImageDimensions {
            width: parse_metadata_u32(fields[6], "raw width")?,
            height: parse_metadata_u32(fields[7], "raw height")?,
        },
        image_dimensions: ImageDimensions {
            width: parse_metadata_u32(fields[8], "image width")?,
            height: parse_metadata_u32(fields[9], "image height")?,
        },
        margins: ImageMargins {
            left: parse_metadata_u32(fields[10], "left margin")?,
            top: parse_metadata_u32(fields[11], "top margin")?,
            right: parse_metadata_u32(fields[12], "right margin")?,
            bottom: parse_metadata_u32(fields[13], "bottom margin")?,
        },
        orientation: parse_metadata_i32(fields[14], "orientation")?,
        cfa_pattern: decode_text(15, "CFA pattern")?,
        sensor_colors: parse_metadata_u32(fields[16], "sensor colors")?,
        sensor_bits: parse_metadata_u32(fields[17], "sensor bits")?,
        black_level: parse_metadata_u32(fields[18], "black level")?,
        white_level: parse_metadata_u32(fields[19], "white level")?,
        as_shot_neutral: [
            parse_metadata_f64(fields[20], "as-shot neutral red")?,
            parse_metadata_f64(fields[21], "as-shot neutral green 1")?,
            parse_metadata_f64(fields[22], "as-shot neutral blue")?,
            parse_metadata_f64(fields[23], "as-shot neutral green 2")?,
        ],
        baseline_exposure: parse_metadata_f64(fields[24], "baseline exposure")?,
        iso_speed: parse_metadata_f64(fields[25], "ISO speed")?,
        exposure_time_seconds: parse_metadata_f64(fields[26], "exposure time")?,
        aperture_f_number: parse_metadata_f64(fields[27], "aperture")?,
        focal_length_mm: parse_metadata_f64(fields[28], "focal length")?,
        captured_at_unix_seconds: parse_metadata_i64(fields[29], "capture time")?,
        gps,
        lens_make: decode_text(35, "lens make")?,
        lens_model: decode_text(36, "lens model")?,
        focal_length_35mm: parse_metadata_f64(fields[37], "35 mm focal length")?,
    })
}

fn parse_metadata_bool(encoded: &str, label: &str) -> Result<bool> {
    match parse_metadata_u64(encoded, label)? {
        0 => Ok(false),
        1 => Ok(true),
        _ => bail!("isolated metadata {label} flag is invalid"),
    }
}

fn valid_gps_snapshot(
    has_coordinates: bool,
    latitude_degrees: f64,
    longitude_degrees: f64,
    has_altitude: bool,
    altitude_meters: f64,
) -> Option<GpsMetadataSnapshot> {
    if !has_coordinates
        || !latitude_degrees.is_finite()
        || !longitude_degrees.is_finite()
        || !(-90.0..=90.0).contains(&latitude_degrees)
        || !(-180.0..=180.0).contains(&longitude_degrees)
    {
        return None;
    }
    Some(GpsMetadataSnapshot {
        latitude_degrees,
        longitude_degrees,
        altitude_meters: (has_altitude && altitude_meters.is_finite()).then_some(altitude_meters),
    })
}

#[cfg(test)]
mod tests;
