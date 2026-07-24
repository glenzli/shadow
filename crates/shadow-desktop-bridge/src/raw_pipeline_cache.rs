//! Cache identities for prepared source-development sessions.
//!
//! The native pipeline owns the canonical receipt because only it knows the
//! selected provider, effective RAW plan, developer revision, and camera
//! profile catalog.  The desktop persists only a bounded digest of that
//! receipt: diagnostics and user-local paths must never become Catalog keys.

use std::{env, ffi::OsStr};

use anyhow::{Result as AnyResult, bail};
use shadow_bridge::RawPipelineReceipt;

const PREPARED_PIPELINE_CACHE_SCHEMA: &str = "raw-pipeline-v1";
const SOURCE_ENVIRONMENT_CACHE_SCHEMA: &str = "source-environment-v1";
const EDIT_PREVIEW_GENERATOR_SCHEMA: &str = "shadow-edit-preview-v1";
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

    pub(super) fn edit_preview_generator_version(
        &self,
        source_environment_identity: &str,
    ) -> String {
        format!(
            "{EDIT_PREVIEW_GENERATOR_SCHEMA};environment={source_environment_identity};pipeline={}",
            self.component,
        )
    }
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
mod tests {
    use super::*;
    use shadow_bridge::{
        DngOpcodePolicy, RawCameraProfileStatus, RawDevelopmentIntent, RawDevelopmentPlan,
        RawDevelopmentQuality, RawHighlightRecoveryIntent, RawNoiseReductionIntent,
        RawPipelinePath,
    };

    fn receipt(canonical_identity: &str) -> RawPipelineReceipt {
        let plan = RawDevelopmentPlan {
            schema_version: RawDevelopmentPlan::CURRENT_SCHEMA_VERSION,
            intent: RawDevelopmentIntent::Preview,
            quality: RawDevelopmentQuality::Balanced,
            dng_opcode_policy: DngOpcodePolicy::ProviderDefault,
            noise_reduction: RawNoiseReductionIntent::ProviderDefault,
            highlight_recovery: RawHighlightRecoveryIntent::ProviderDefault,
        };
        RawPipelineReceipt {
            schema_version: RawPipelineReceipt::CURRENT_SCHEMA_VERSION,
            path: RawPipelinePath::ShadowRawFrame,
            cache_identity: canonical_identity.to_owned(),
            pipeline_identity: "fixture-pipeline".to_owned(),
            source_provider_id: "fixture-provider".to_owned(),
            source_provider_version: "1".to_owned(),
            fallback_reason: None,
            raw_frame_schema_version: 1,
            raw_developer_version: 1,
            requested_plan: plan,
            effective_plan: plan,
            camera_profile_status: RawCameraProfileStatus::NoMatch,
            camera_profile_catalog_identity: "fixture-catalog".to_owned(),
            camera_profile_identity: String::new(),
            camera_profile_name: String::new(),
            camera_profile_diagnostic: None,
            camera_profile_developer_version:
                RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION,
        }
    }

    #[test]
    fn prepared_identity_is_bounded_and_hides_native_diagnostics() {
        let diagnostic = "private path /Users/example/profile.dcp; fallback details";
        let identity =
            prepared_raw_pipeline_cache_identity(&receipt(diagnostic)).expect("compact receipt");

        assert!(identity.component().starts_with("raw-pipeline-v1-"));
        assert_eq!(
            identity.component().len(),
            "raw-pipeline-v1-".len() + SHORT_DIGEST_BYTES * 2
        );
        assert!(!identity.component().contains(diagnostic));
        assert_ne!(
            identity,
            prepared_raw_pipeline_cache_identity(&receipt("different canonical receipt"))
                .expect("compact changed receipt")
        );
        assert_ne!(
            identity.edit_preview_generator_version("source-environment-v1-router"),
            prepared_raw_pipeline_cache_identity(&receipt("different canonical receipt"))
                .expect("compact changed receipt")
                .edit_preview_generator_version("source-environment-v1-router")
        );
        assert_ne!(
            identity.edit_preview_generator_version("source-environment-v1-router"),
            identity.edit_preview_generator_version("source-environment-v1-other-router")
        );
    }

    #[test]
    fn absent_or_stale_receipts_cannot_enter_a_durable_cache_key() {
        assert!(prepared_raw_pipeline_cache_identity(&RawPipelineReceipt::default()).is_err());
        let mut stale = receipt("canonical");
        stale.schema_version = RawPipelineReceipt::CURRENT_SCHEMA_VERSION + 1;
        assert!(prepared_raw_pipeline_cache_identity(&stale).is_err());
    }

    #[test]
    fn effective_backend_changes_the_bounded_durable_identity() {
        let cpu = prepared_raw_pipeline_cache_identity(&receipt(
            "raw-pipeline-receipt-v1;pipeline=shadow-raw-frame-v1;\
             backend=shadow-fused-raw-cpu-v1",
        ))
        .expect("compact CPU receipt");
        let metal = prepared_raw_pipeline_cache_identity(&receipt(
            "raw-pipeline-receipt-v1;pipeline=shadow-raw-frame-v1;\
             backend=shadow-fused-raw-metal-full-v1;math=f32-precise",
        ))
        .expect("compact Metal receipt");

        assert_ne!(cpu, metal);
        assert_ne!(
            cpu.edit_preview_generator_version("source-environment-v1-auto"),
            metal.edit_preview_generator_version("source-environment-v1-auto")
        );
    }

    #[test]
    fn source_environment_distinguishes_router_policy_and_profile_root() {
        let automatic =
            source_environment_cache_identity("router-v1", None, None, None, None, None);
        let raw_frame = source_environment_cache_identity(
            "router-v1",
            Some(OsStr::new("raw-frame")),
            None,
            None,
            None,
            None,
        );
        let forced_cpu = source_environment_cache_identity(
            "router-v1",
            None,
            Some(OsStr::new("cpu")),
            None,
            None,
            None,
        );
        let other_router =
            source_environment_cache_identity("router-v2", None, None, None, None, None);
        let other_profile_root = source_environment_cache_identity(
            "router-v1",
            None,
            None,
            Some(OsStr::new("/profiles/alternate")),
            None,
            None,
        );
        let other_helper = source_environment_cache_identity(
            "router-v1",
            None,
            None,
            None,
            Some(OsStr::new("/helpers/alternate")),
            None,
        );

        assert_ne!(automatic, raw_frame);
        assert_ne!(automatic, forced_cpu);
        assert_ne!(automatic, other_router);
        assert_ne!(automatic, other_profile_root);
        assert_ne!(automatic, other_helper);
        assert_eq!(
            automatic,
            source_environment_cache_identity("router-v1", None, None, None, None, None)
        );
    }
}
