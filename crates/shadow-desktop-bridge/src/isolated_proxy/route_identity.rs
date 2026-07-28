//! Stable source, helper, environment, protocol, and operation identities.
//!
//! These values key both positive helper snapshots and durable negative safety
//! evidence. Their serialized field order and string values are persistence
//! contracts, not implementation details.

use std::{
    env, fs,
    io::{self, Read, Seek, SeekFrom},
    path::{Path, PathBuf},
};

use anyhow::{Context, Result};
use shadow_core::fingerprint_source;

const HELPER_PATH_ENVIRONMENT: &str = "SHADOW_DECODE_HELPER_PATH";
const PRIVATE_DECODER_PLUGIN_PATH_ENVIRONMENT: &str = "SHADOW_PRIVATE_DECODER_PLUGIN_PATH";
const RAW_PIPELINE_ENVIRONMENT: &str = "SHADOW_RAW_PIPELINE";
const IMAGE_ACCELERATION_ENVIRONMENT: &str = "SHADOW_IMAGE_ACCELERATION";
const CAMERA_PROFILE_DIRECTORY_ENVIRONMENT: &str = "SHADOW_CAMERA_PROFILE_DIRECTORY";
const SOURCE_SIGNATURE_SAMPLE_BYTES: usize = 64 * 1024;

pub(super) const REFERENCE_PROXY_PROTOCOL: &str = "shadow-proxy-v1";
pub(super) const METADATA_SNAPSHOT_PROTOCOL: &str = "shadow-metadata-v2";
pub(super) const DECODER_SNAPSHOT_PROTOCOL: &str = "shadow-inspect-v3";

const REFERENCE_PROXY_OPERATION: &str = "reference-proxy";
const METADATA_SNAPSHOT_OPERATION: &str = "metadata-snapshot";
const DECODER_SNAPSHOT_OPERATION: &str = "decoder-snapshot";

// Compatibility identity for persisted negative evidence written by the
// retired metadata-probe executor. Native admission must continue consulting
// this exact protocol/operation pair so an existing crash or timeout cannot be
// silently forgotten after the unreachable executor is removed.
const LEGACY_METADATA_PROBE_PROTOCOL: &str = "shadow-probe-v1";
const LEGACY_METADATA_PROBE_OPERATION: &str = "metadata-probe";

#[derive(Debug, Clone, Eq, PartialEq, Hash, serde::Serialize, serde::Deserialize)]
pub(super) struct DecoderSafetyKey {
    source_path: String,
    source_byte_len: u64,
    source_modified_at_ms: Option<i64>,
    /// A bounded content sample prevents a same-length, timestamp-preserving
    /// replacement from reusing a stale safety decision or decoder snapshot.
    /// This is deliberately not a catalog-wide full-file hash: it is only the
    /// helper's local admission/cache identity, and costs a small fixed read.
    source_content_sample: String,
    helper_path: String,
    helper_byte_len: Option<u64>,
    helper_modified_at_ms: Option<i64>,
    /// Helper executables are normally small. Hash their full contents rather
    /// than trusting a size/mtime pair, because this identity controls which
    /// private provider graph produced an isolated observation.
    helper_content_digest: Option<String>,
    /// A digest of the decoder-relevant environment as observed by the child.
    /// It includes the explicit private-plugin override and that file's weak
    /// fingerprint when present, but never persists the private path itself.
    provider_environment_identity: String,
    helper_protocol: String,
    operation: String,
}

impl DecoderSafetyKey {
    pub(super) fn reference_proxy(source_path: &Path, helper_path: &Path) -> Result<Self> {
        Self::for_operation(
            source_path,
            helper_path,
            REFERENCE_PROXY_PROTOCOL,
            REFERENCE_PROXY_OPERATION,
        )
    }

    pub(super) fn legacy_metadata_probe(source_path: &Path, helper_path: &Path) -> Result<Self> {
        Self::for_operation(
            source_path,
            helper_path,
            LEGACY_METADATA_PROBE_PROTOCOL,
            LEGACY_METADATA_PROBE_OPERATION,
        )
    }

    pub(super) fn metadata_snapshot(source_path: &Path, helper_path: &Path) -> Result<Self> {
        Self::for_operation(
            source_path,
            helper_path,
            METADATA_SNAPSHOT_PROTOCOL,
            METADATA_SNAPSHOT_OPERATION,
        )
    }

    pub(super) fn decoder_snapshot(source_path: &Path, helper_path: &Path) -> Result<Self> {
        Self::for_operation(
            source_path,
            helper_path,
            DECODER_SNAPSHOT_PROTOCOL,
            DECODER_SNAPSHOT_OPERATION,
        )
    }

    pub(super) fn is_current(&self, source_path: &Path, helper_path: &Path) -> bool {
        let Ok(after) = Self::for_operation(
            source_path,
            helper_path,
            &self.helper_protocol,
            &self.operation,
        ) else {
            return false;
        };
        after == *self
    }

    fn for_operation(
        source_path: &Path,
        helper_path: &Path,
        helper_protocol: &str,
        operation: &str,
    ) -> Result<Self> {
        let source = fingerprint_source(source_path).with_context(|| {
            format!(
                "fingerprint isolated decoder source {}",
                source_path.display()
            )
        })?;
        // Failure to stat a helper still belongs to the normal launch-error
        // path. The location remains part of the safety key so a replacement
        // executable receives a separate observation even before it can be
        // fingerprinted successfully.
        let helper = fingerprint_source(helper_path).ok();
        Ok(Self {
            source_path: source_path.to_string_lossy().into_owned(),
            source_byte_len: source.byte_len,
            source_modified_at_ms: source.modified_at_ms,
            source_content_sample: sampled_file_content_identity(source_path).with_context(
                || {
                    format!(
                        "sample isolated decoder source contents {}",
                        source_path.display()
                    )
                },
            )?,
            helper_path: helper_path.to_string_lossy().into_owned(),
            helper_byte_len: helper.as_ref().map(|fingerprint| fingerprint.byte_len),
            helper_modified_at_ms: helper.and_then(|fingerprint| fingerprint.modified_at_ms),
            helper_content_digest: full_file_content_digest(helper_path).ok(),
            provider_environment_identity: isolated_provider_environment_identity(),
            helper_protocol: helper_protocol.to_owned(),
            operation: operation.to_owned(),
        })
    }
}

/// Identifies values that can change which provider/pipeline the child opens.
///
/// This is intentionally a local safety-cache key, rather than catalog
/// provenance. A replacement helper, an explicit private-plugin replacement,
/// or a changed RAW/GPU policy must get a fresh safety observation instead of
/// inheriting a stale crash quarantine. The plugin's *path* is hashed so it is
/// not copied into the persisted record. An explicitly configured plugin also
/// contributes a full content digest. Discovery-mode private plugins remain a
/// limitation until the helper can report the selected module artifact itself.
fn isolated_provider_environment_identity() -> String {
    let mut hasher = blake3::Hasher::new();
    hash_environment_field(
        &mut hasher,
        "raw-pipeline",
        env::var_os(RAW_PIPELINE_ENVIRONMENT),
    );
    hash_environment_field(
        &mut hasher,
        "image-acceleration",
        env::var_os(IMAGE_ACCELERATION_ENVIRONMENT),
    );
    hash_environment_field(
        &mut hasher,
        "camera-profile-directory",
        env::var_os(CAMERA_PROFILE_DIRECTORY_ENVIRONMENT),
    );

    match env::var_os(PRIVATE_DECODER_PLUGIN_PATH_ENVIRONMENT) {
        Some(plugin_path) => {
            let plugin_path = PathBuf::from(plugin_path);
            hash_text_field(
                &mut hasher,
                "private-decoder-plugin-path",
                &plugin_path.to_string_lossy(),
            );
            match fingerprint_source(&plugin_path) {
                Ok(fingerprint) => {
                    hash_text_field(
                        &mut hasher,
                        "private-decoder-plugin-byte-len",
                        &fingerprint.byte_len.to_string(),
                    );
                    hash_text_field(
                        &mut hasher,
                        "private-decoder-plugin-modified-at-ms",
                        &fingerprint
                            .modified_at_ms
                            .map_or_else(|| "<unknown>".to_owned(), |value| value.to_string()),
                    );
                    match full_file_content_digest(&plugin_path) {
                        Ok(digest) => hash_text_field(
                            &mut hasher,
                            "private-decoder-plugin-content-digest",
                            &digest,
                        ),
                        Err(_) => hash_text_field(
                            &mut hasher,
                            "private-decoder-plugin-content-digest",
                            "<unavailable>",
                        ),
                    }
                }
                Err(_) => hash_text_field(
                    &mut hasher,
                    "private-decoder-plugin-fingerprint",
                    "<unavailable>",
                ),
            }
        }
        None => hash_text_field(&mut hasher, "private-decoder-plugin-path", "<discovered>"),
    }
    hasher.finalize().to_hex().to_string()
}

/// Stable implementation identity used by the outer desktop inspector when
/// the helper is enabled. The catalog may use this in a provider/preview
/// version because it reveals only a digest—not a private plugin path—while a
/// replacement helper or explicit private plugin cannot silently reuse an old
/// inspection/proxy cache entry after application restart.
// Keep the fallible facade contract used by callers even though every
// identity input currently degrades to an explicit unavailable marker.
#[allow(clippy::unnecessary_wraps)]
pub(crate) fn isolated_helper_implementation_identity(
    helper_path: Option<&Path>,
) -> Result<String> {
    let mut hasher = blake3::Hasher::new();
    hash_text_field(
        &mut hasher,
        "isolated-provider-environment",
        &isolated_provider_environment_identity(),
    );
    match helper_path {
        Some(path) => {
            hash_text_field(&mut hasher, "helper-path", &path.to_string_lossy());
            match full_file_content_digest(path) {
                Ok(digest) => hash_text_field(&mut hasher, "helper-content-digest", &digest),
                Err(error) => {
                    hash_text_field(&mut hasher, "helper-content-digest", "<unavailable>");
                    hash_text_field(&mut hasher, "helper-open-error", &error.kind().to_string());
                }
            }
        }
        None => hash_text_field(&mut hasher, "helper-path", "<unconfigured>"),
    }
    Ok(hasher.finalize().to_hex().to_string())
}

/// Returns the helper explicitly selected by the desktop shell, if present.
/// Keeping lookup outside the renderer makes tests and non-desktop clients
/// retain their direct public-provider behavior.
pub(crate) fn configured_helper_path() -> Option<PathBuf> {
    env::var_os(HELPER_PATH_ENVIRONMENT).map(PathBuf::from)
}

fn full_file_content_digest(path: &Path) -> io::Result<String> {
    let mut file = fs::File::open(path)?;
    let mut hasher = blake3::Hasher::new();
    let mut buffer = vec![0_u8; 64 * 1024];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(hasher.finalize().to_hex().to_string())
}

fn sampled_file_content_identity(path: &Path) -> io::Result<String> {
    let byte_len = fs::metadata(path)?.len();
    let mut file = fs::File::open(path)?;
    let sample_len = u64::try_from(SOURCE_SIGNATURE_SAMPLE_BYTES).expect("sample length fits u64");
    let middle = byte_len.saturating_sub(sample_len) / 2;
    let tail = byte_len.saturating_sub(sample_len);
    let mut offsets = [0_u64, middle, tail];
    offsets.sort_unstable();
    let mut hasher = blake3::Hasher::new();
    hasher.update(b"shadow-source-sample-v1");
    hasher.update(&byte_len.to_le_bytes());
    let mut previous = None;
    let mut buffer = vec![0_u8; SOURCE_SIGNATURE_SAMPLE_BYTES];
    for offset in offsets {
        if previous == Some(offset) {
            continue;
        }
        previous = Some(offset);
        file.seek(SeekFrom::Start(offset))?;
        let mut remaining = usize::try_from((byte_len - offset).min(sample_len))
            .expect("bounded sample length fits usize");
        hasher.update(&offset.to_le_bytes());
        while remaining > 0 {
            let chunk_len = remaining.min(buffer.len());
            let count = file.read(&mut buffer[..chunk_len])?;
            if count == 0 {
                break;
            }
            hasher.update(&buffer[..count]);
            remaining -= count;
        }
    }
    Ok(hasher.finalize().to_hex().to_string())
}

fn hash_environment_field(
    hasher: &mut blake3::Hasher,
    name: &str,
    value: Option<std::ffi::OsString>,
) {
    let value = value.map_or_else(
        || "<unset>".to_owned(),
        |value| value.to_string_lossy().into_owned(),
    );
    hash_text_field(hasher, name, &value);
}

fn hash_text_field(hasher: &mut blake3::Hasher, name: &str, value: &str) {
    hasher.update(&(name.len() as u64).to_le_bytes());
    hasher.update(name.as_bytes());
    hasher.update(&(value.len() as u64).to_le_bytes());
    hasher.update(value.as_bytes());
}

#[cfg(test)]
mod tests;
