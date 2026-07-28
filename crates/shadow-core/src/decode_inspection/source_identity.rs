//! Filesystem identity guards for queued decode work.

use std::{
    path::Path,
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_catalog::RepresentationFingerprint;

use crate::performance::{DecodePerformance, measure_if};

use super::contract::{DecodeInspectionError, DecodeInspectionRequest};

/// Reads the size and native modification time used to guard an inspection.
///
/// # Errors
///
/// Returns the filesystem metadata error for an absent or inaccessible path.
pub fn fingerprint_source(path: &Path) -> Result<RepresentationFingerprint, std::io::Error> {
    let metadata = path.metadata()?;
    Ok(RepresentationFingerprint {
        byte_len: metadata.len(),
        modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
    })
}

pub(super) fn source_changed_profiled(
    request: &DecodeInspectionRequest,
    performance: &mut DecodePerformance,
) -> bool {
    measure_if(
        performance.profiled,
        &mut performance.source_guard_stat,
        || fingerprint_source(&request.path),
    )
    .map_or(true, |current| current != request.expected_source)
}

pub(super) fn read_source_fingerprint_profiled(
    path: &Path,
    performance: &mut DecodePerformance,
) -> Result<RepresentationFingerprint, DecodeInspectionError> {
    measure_if(
        performance.profiled,
        &mut performance.source_guard_stat,
        || fingerprint_source(path),
    )
    .map_err(|source| DecodeInspectionError::SourceMetadata {
        path: path.to_path_buf(),
        source,
    })
}

fn system_time_ms(time: SystemTime) -> Option<i64> {
    let duration = time.duration_since(UNIX_EPOCH).ok()?;
    i64::try_from(duration.as_millis()).ok()
}

pub(super) fn now_ms() -> i64 {
    system_time_ms(SystemTime::now()).unwrap_or_default()
}
