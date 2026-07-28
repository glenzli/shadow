//! Cache identities for prepared source, edit, and display preview stages.
//!
//! The native pipeline owns the canonical receipt because only it knows the
//! selected provider, effective RAW plan, developer revision, camera profile
//! catalog, and the effective edit/display route. The desktop persists only
//! bounded digests of those receipts: diagnostics and user-local paths must
//! never become Catalog keys.

use std::{env, ffi::OsStr};

use anyhow::{Result as AnyResult, bail};
use shadow_bridge::{EditPreviewExecutionReceipt, RawPipelineReceipt};

const PREPARED_PIPELINE_CACHE_SCHEMA: &str = "raw-pipeline-v1";
const PREPARED_EDIT_EXECUTION_CACHE_SCHEMA: &str = "edit-execution-v1";
const SOURCE_ENVIRONMENT_CACHE_SCHEMA: &str = "source-environment-v1";
const EDIT_PREVIEW_GENERATOR_SCHEMA: &str = "shadow-edit-preview-v1";
pub(super) const EDIT_PREVIEW_GENERATOR_ID: &str = "shadow-edit-preview";
const RAW_PIPELINE_ENVIRONMENT: &str = "SHADOW_RAW_PIPELINE";
const IMAGE_ACCELERATION_ENVIRONMENT: &str = "SHADOW_IMAGE_ACCELERATION";
const CAMERA_PROFILE_DIRECTORY_ENVIRONMENT: &str = "SHADOW_CAMERA_PROFILE_DIRECTORY";
const DECODE_HELPER_PATH_ENVIRONMENT: &str = "SHADOW_DECODE_HELPER_PATH";
const PRIVATE_DECODER_PLUGIN_PATH_ENVIRONMENT: &str = "SHADOW_PRIVATE_DECODER_PLUGIN_PATH";
const SHORT_DIGEST_BYTES: usize = 16;

#[derive(Debug, Clone, Eq, PartialEq)]
pub(super) struct PreparedRawPipelineCacheIdentity {
    component: String,
}

impl PreparedRawPipelineCacheIdentity {
    pub(super) fn component(&self) -> &str {
        &self.component
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(super) struct PreparedEditExecutionCacheIdentity {
    component: String,
}

impl PreparedEditExecutionCacheIdentity {
    pub(super) fn component(&self) -> &str {
        &self.component
    }
}

/// Identifies the Recipe-preview implementation before a source is decoded.
///
/// The prepared source and edit/display receipts remain part of each artifact's variant key,
/// where they distinguish actual provider and CPU/Metal execution. The generator version uses
/// only the bounded source environment and compiled implementation contract, both available to
/// the Library before any expensive render is prepared.
pub(super) fn edit_preview_generator_version(
    source_environment_identity: &str,
    implementation_identity: &str,
) -> String {
    format!(
        "{EDIT_PREVIEW_GENERATOR_SCHEMA};environment={source_environment_identity};\
         implementation={}",
        short_digest(implementation_identity.as_bytes())
    )
}

/// Compacts the native canonical receipt without exposing any of its diagnostic text.
pub(super) fn prepared_raw_pipeline_cache_identity(
    receipt: &RawPipelineReceipt,
) -> AnyResult<PreparedRawPipelineCacheIdentity> {
    if !receipt.recorded() || !receipt.uses_current_schema() || receipt.cache_identity.is_empty() {
        bail!("prepared source has no current RAW pipeline cache identity");
    }
    Ok(PreparedRawPipelineCacheIdentity {
        component: format!(
            "{PREPARED_PIPELINE_CACHE_SCHEMA}-{}",
            short_digest(receipt.cache_identity.as_bytes())
        ),
    })
}

/// Compacts the post-selection edit/display route. C++ owns the canonical identity; desktop code
/// must never reconstruct it from diagnostic fields or runtime device descriptions.
pub(super) fn prepared_edit_execution_cache_identity(
    receipt: &EditPreviewExecutionReceipt,
) -> AnyResult<PreparedEditExecutionCacheIdentity> {
    if !receipt.uses_current_schema() || receipt.cache_identity.is_empty() {
        bail!("completed edit preview has no current execution cache identity");
    }
    Ok(PreparedEditExecutionCacheIdentity {
        component: format!(
            "{PREPARED_EDIT_EXECUTION_CACHE_SCHEMA}-{}",
            short_digest(receipt.cache_identity.as_bytes())
        ),
    })
}

/// Captures every source-selection input that can change while one desktop
/// process remains alive. Camera-profile contents are intentionally absent:
/// the native catalog is an immutable process-lifetime snapshot, and its exact
/// identity is already present in each prepared receipt.
pub(super) fn current_source_environment_cache_identity(provider_version: &str) -> String {
    let pipeline_policy = env::var_os(RAW_PIPELINE_ENVIRONMENT);
    let image_acceleration = env::var_os(IMAGE_ACCELERATION_ENVIRONMENT);
    let camera_profile_directory = env::var_os(CAMERA_PROFILE_DIRECTORY_ENVIRONMENT);
    let decode_helper_path = env::var_os(DECODE_HELPER_PATH_ENVIRONMENT);
    let private_decoder_plugin_path = env::var_os(PRIVATE_DECODER_PLUGIN_PATH_ENVIRONMENT);
    source_environment_cache_identity(
        provider_version,
        pipeline_policy.as_deref(),
        image_acceleration.as_deref(),
        camera_profile_directory.as_deref(),
        decode_helper_path.as_deref(),
        private_decoder_plugin_path.as_deref(),
    )
}

/// Compares the requested RAW-development identity used by prepared-source
/// caches.
///
/// A provider may adjust request A to effective plan B. The resulting pixels
/// could match a later request B, but the immutable receipt still records A.
/// Reuse therefore requires the same requested identity rather than an
/// effective-plan match.
pub(crate) fn requested_raw_development_plan_cache_matches(
    cached_requested_identity: &str,
    requested_identity: &str,
) -> bool {
    cached_requested_identity == requested_identity
}

fn source_environment_cache_identity(
    provider_version: &str,
    pipeline_policy: Option<&OsStr>,
    image_acceleration: Option<&OsStr>,
    camera_profile_directory: Option<&OsStr>,
    decode_helper_path: Option<&OsStr>,
    private_decoder_plugin_path: Option<&OsStr>,
) -> String {
    let pipeline_policy =
        pipeline_policy.map_or_else(|| "auto".into(), |value| value.to_string_lossy());
    let image_acceleration =
        image_acceleration.map_or_else(|| "auto".into(), |value| value.to_string_lossy());
    let camera_profile_directory = camera_profile_directory
        .map_or_else(|| "<default>".into(), |value| value.to_string_lossy());
    let decode_helper_path =
        decode_helper_path.map_or_else(|| "<none>".into(), |value| value.to_string_lossy());
    let private_decoder_plugin_path = private_decoder_plugin_path
        .map_or_else(|| "<discovered>".into(), |value| value.to_string_lossy());
    let mut hasher = blake3::Hasher::new();
    update_field(&mut hasher, "provider", provider_version);
    update_field(&mut hasher, "policy", &pipeline_policy);
    update_field(&mut hasher, "image-acceleration", &image_acceleration);
    update_field(
        &mut hasher,
        "camera-profile-directory",
        &camera_profile_directory,
    );
    update_field(&mut hasher, "decode-helper", &decode_helper_path);
    update_field(
        &mut hasher,
        "private-decoder-plugin",
        &private_decoder_plugin_path,
    );
    format!(
        "{SOURCE_ENVIRONMENT_CACHE_SCHEMA}-{}",
        short_digest(hasher.finalize().as_bytes())
    )
}

fn update_field(hasher: &mut blake3::Hasher, name: &str, value: &str) {
    hasher.update(&(name.len() as u64).to_le_bytes());
    hasher.update(name.as_bytes());
    hasher.update(&(value.len() as u64).to_le_bytes());
    hasher.update(value.as_bytes());
}

fn short_digest(value: &[u8]) -> String {
    let digest = blake3::hash(value);
    let mut encoded = String::with_capacity(SHORT_DIGEST_BYTES * 2);
    for byte in &digest.as_bytes()[..SHORT_DIGEST_BYTES] {
        use std::fmt::Write as _;
        let _ = write!(encoded, "{byte:02x}");
    }
    encoded
}

#[cfg(test)]
mod tests;
