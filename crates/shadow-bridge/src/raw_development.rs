//! Provider-neutral RAW development plans, negotiation, and durable execution receipts.
//!
//! This protocol owns decode intent and provenance before photographer-authored RGB adjustments.

use serde::{Deserialize, Serialize};
use shadow_domain::{
    ImageDimensions, RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN, RawTemperatureTint,
    RawWhiteBalance,
};

use super::{BridgeError, decoder::dimensions, ffi};

/// The source-raster purpose requested from a RAW provider. This is intentionally separate from
/// photographer-controlled adjustment nodes: it determines decode quality and cache identity
/// before the common RGB graph exists.
#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RawDevelopmentIntent {
    Preview,
    Detail,
    ExportImage,
}

/// Provider-neutral decode-quality preference. A provider can negotiate an exact or adjusted
/// plan, but it must write the requested/effective pair into its development receipt.
#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RawDevelopmentQuality {
    Fast,
    Balanced,
    High,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DngOpcodePolicy {
    ProviderDefault,
    RequireApplied,
    DeferToShadow,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RawNoiseReductionIntent {
    ProviderDefault,
    Disabled,
    Conservative,
    NoiseRobust,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RawHighlightRecoveryIntent {
    ProviderDefault,
    Disabled,
    Conservative,
    Aggressive,
}

/// The immutable, cache-visible RAW source-development request. It is deliberately not stored
/// inside a color node or versioned grade stack: previews, 1:1 detail, and future exports may
/// legitimately need different source rasters while sharing every downstream adjustment.
#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct RawDevelopmentPlan {
    pub schema_version: u32,
    pub intent: RawDevelopmentIntent,
    pub quality: RawDevelopmentQuality,
    pub dng_opcode_policy: DngOpcodePolicy,
    pub noise_reduction: RawNoiseReductionIntent,
    pub highlight_recovery: RawHighlightRecoveryIntent,
    #[serde(default)]
    pub white_balance: RawWhiteBalance,
}

impl Default for RawDevelopmentPlan {
    fn default() -> Self {
        Self::detail()
    }
}

impl RawDevelopmentPlan {
    pub const CURRENT_SCHEMA_VERSION: u32 = 1;

    #[must_use]
    pub const fn preview() -> Self {
        Self {
            schema_version: Self::CURRENT_SCHEMA_VERSION,
            intent: RawDevelopmentIntent::Preview,
            quality: RawDevelopmentQuality::Balanced,
            dng_opcode_policy: DngOpcodePolicy::ProviderDefault,
            noise_reduction: RawNoiseReductionIntent::ProviderDefault,
            highlight_recovery: RawHighlightRecoveryIntent::ProviderDefault,
            white_balance: RawWhiteBalance::AsShot,
        }
    }

    #[must_use]
    pub const fn detail() -> Self {
        Self {
            schema_version: Self::CURRENT_SCHEMA_VERSION,
            intent: RawDevelopmentIntent::Detail,
            quality: RawDevelopmentQuality::Balanced,
            dng_opcode_policy: DngOpcodePolicy::ProviderDefault,
            noise_reduction: RawNoiseReductionIntent::ProviderDefault,
            highlight_recovery: RawHighlightRecoveryIntent::ProviderDefault,
            white_balance: RawWhiteBalance::AsShot,
        }
    }

    #[must_use]
    pub const fn export_image() -> Self {
        Self {
            schema_version: Self::CURRENT_SCHEMA_VERSION,
            intent: RawDevelopmentIntent::ExportImage,
            quality: RawDevelopmentQuality::High,
            dng_opcode_policy: DngOpcodePolicy::ProviderDefault,
            noise_reduction: RawNoiseReductionIntent::ProviderDefault,
            highlight_recovery: RawHighlightRecoveryIntent::ProviderDefault,
            white_balance: RawWhiteBalance::AsShot,
        }
    }

    /// Binds the photo's absolute Foundation white balance to this
    /// intent/quality-specific source-development request.
    #[must_use]
    pub const fn with_white_balance(mut self, white_balance: RawWhiteBalance) -> Self {
        self.white_balance = white_balance;
        self
    }

    pub(super) fn validate(self) -> Result<(), BridgeError> {
        if self.schema_version != Self::CURRENT_SCHEMA_VERSION {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "RAW development plan uses an unsupported schema",
            ));
        }
        Ok(())
    }
}

#[derive(Debug, Default, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RawDevelopmentPlanNegotiationStatus {
    Accepted,
    Adjusted,
    #[default]
    Rejected,
}

#[derive(Debug, Default, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DngOpcodeExecutionStatus {
    #[default]
    NotDeclared,
    ProviderDefault,
    Applied,
    DeferredToShadow,
    SkippedForPreview,
    Unsupported,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[allow(clippy::struct_excessive_bools)]
pub struct RawDevelopmentCapabilities {
    pub schema_version: u32,
    pub available: bool,
    pub raw_frame: bool,
    pub dng_opcode_execution_receipt: bool,
    pub supported_intents: u32,
    pub supported_qualities: u32,
    pub supported_dng_opcode_policies: u32,
    pub supported_noise_reduction_intents: u32,
    pub supported_highlight_recovery_intents: u32,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct RawDevelopmentPlanNegotiation {
    pub requested: RawDevelopmentPlan,
    pub effective: RawDevelopmentPlan,
    pub status: RawDevelopmentPlanNegotiationStatus,
    /// Bitset of `RawDevelopmentPlan` aspects the provider could not honor.
    pub unresolved: u32,
}

impl RawDevelopmentPlanNegotiation {
    #[must_use]
    pub const fn accepted(self) -> bool {
        !matches!(self.status, RawDevelopmentPlanNegotiationStatus::Rejected)
    }

    #[must_use]
    pub const fn exact(self) -> bool {
        matches!(self.status, RawDevelopmentPlanNegotiationStatus::Accepted)
    }
}

/// The exact provider-side RAW-development request that produced an editable source raster.
///
/// This is immutable source provenance, not a photographer-editable recipe. A zero
/// `schema_version` is an explicit absence: it means that the active source provider did not
/// report RAW-development provenance. Callers must not infer sensor-domain behavior from it.
///
/// The serialized shape intentionally mirrors the fixed C++ receipt field-for-field. If the
/// source renderer changes what a recorded field means, it must increment
/// `schema_version`; consumers can then preserve rather than misinterpret an unknown receipt.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[allow(clippy::struct_excessive_bools)]
pub struct RawDevelopmentReceipt {
    pub schema_version: u32,
    pub provider_id: String,
    pub provider_version: String,
    pub library_version: String,
    pub development_settings_signature: String,
    #[serde(default)]
    pub requested_plan_identity: String,
    #[serde(default)]
    pub effective_plan_identity: String,
    #[serde(default)]
    pub requested_plan: RawDevelopmentPlan,
    #[serde(default)]
    pub effective_plan: RawDevelopmentPlan,
    #[serde(default)]
    pub plan_negotiation_status: RawDevelopmentPlanNegotiationStatus,
    pub processed_linear_reference_contract_version: u32,
    pub declared_image_dimensions: ImageDimensions,
    pub rendered_dimensions: ImageDimensions,
    pub orientation: i32,
    pub half_size: bool,
    pub use_camera_white_balance: bool,
    pub use_camera_matrix: bool,
    pub use_auto_brightness: bool,
    pub use_exposure_correction: bool,
    pub brightness: f32,
    pub maximum_adjustment_threshold: f32,
    pub output_bits_per_channel: u16,
    pub demosaic_quality: i32,
    pub output_color: i32,
    pub gamma_inverse_power: f64,
    pub gamma_linear_toe_slope: f64,
    pub declared_dng_opcode_list_bytes: [u32; 3],
    #[serde(default)]
    pub dng_opcode_execution: [DngOpcodeExecutionStatus; 3],
    pub process_warnings: u32,
}

impl RawDevelopmentReceipt {
    /// Matches the currently supported C++ `RawDevelopmentReceipt` schema.
    pub const CURRENT_SCHEMA_VERSION: u32 = 1;

    #[must_use]
    pub const fn recorded(&self) -> bool {
        self.schema_version != 0
    }

    #[must_use]
    pub const fn uses_current_schema(&self) -> bool {
        self.schema_version == Self::CURRENT_SCHEMA_VERSION
    }
}

/// The host-selected source path that produced the common editable raster.
///
/// This is deliberately separate from [`RawDevelopmentReceipt`]: the latter records what a RAW
/// provider did, while this enum records whether Shadow consumed a provider-owned `RawFrame`,
/// accepted provider-processed RGB as a compatibility path, or decoded an ordinary raster.
#[derive(Debug, Clone, Copy, Default, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RawPipelinePath {
    #[default]
    DecodedRaster,
    ShadowRawFrame,
    ProviderProcessedCompatibility,
}

/// Outcome of the host-side DCP lookup and application stage.
///
/// Consumers should branch on this enum and treat `camera_profile_diagnostic` as presentation
/// text only. In particular, `NoMatch` and `MatchedNotApplied` are distinct cache-visible states.
#[derive(Debug, Clone, Copy, Default, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RawCameraProfileStatus {
    #[default]
    NotConsidered,
    NoMatch,
    Applied,
    MatchedNotApplied,
}

/// Immutable provenance for Shadow's host-side source-development route.
///
/// `cache_identity` is constructed canonically by C++ from every cache-relevant field. Consumers
/// should use it directly and inspect [`RawPipelinePath`] for behavior; they must not parse either
/// identity or the human-readable fallback reason. A zero `schema_version` is explicit absence
/// before a decode handle has prepared any render-backed session.
#[derive(Debug, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct RawPipelineReceipt {
    pub schema_version: u32,
    pub path: RawPipelinePath,
    pub cache_identity: String,
    pub pipeline_identity: String,
    pub source_provider_id: String,
    pub source_provider_version: String,
    pub fallback_reason: Option<String>,
    pub raw_frame_schema_version: u32,
    pub raw_developer_version: u32,
    pub requested_plan: RawDevelopmentPlan,
    pub effective_plan: RawDevelopmentPlan,
    pub camera_profile_status: RawCameraProfileStatus,
    pub camera_profile_catalog_identity: String,
    pub camera_profile_identity: String,
    pub camera_profile_name: String,
    pub camera_profile_diagnostic: Option<String>,
    pub camera_profile_developer_version: u32,
}

impl Default for RawPipelineReceipt {
    fn default() -> Self {
        Self {
            schema_version: 0,
            path: RawPipelinePath::DecodedRaster,
            cache_identity: String::new(),
            pipeline_identity: String::new(),
            source_provider_id: String::new(),
            source_provider_version: String::new(),
            fallback_reason: None,
            raw_frame_schema_version: 0,
            raw_developer_version: 0,
            requested_plan: RawDevelopmentPlan::detail(),
            effective_plan: RawDevelopmentPlan::detail(),
            camera_profile_status: RawCameraProfileStatus::NotConsidered,
            camera_profile_catalog_identity: String::new(),
            camera_profile_identity: String::new(),
            camera_profile_name: String::new(),
            camera_profile_diagnostic: None,
            camera_profile_developer_version: 0,
        }
    }
}

impl RawPipelineReceipt {
    pub const CURRENT_SCHEMA_VERSION: u32 = 1;
    // Mirrors `shadow::image::dcp_color_developer_version`. This is part of the
    // cross-language provenance contract, rather than the pipeline receipt schema.
    pub const CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION: u32 = 3;

    #[must_use]
    pub const fn recorded(&self) -> bool {
        self.schema_version != 0
    }

    #[must_use]
    pub const fn uses_current_schema(&self) -> bool {
        self.schema_version == Self::CURRENT_SCHEMA_VERSION
    }

    #[must_use]
    pub const fn used_fallback(&self) -> bool {
        self.fallback_reason.is_some()
    }
}

fn ffi_raw_development_intent(value: RawDevelopmentIntent) -> ffi::FfiRawDevelopmentIntent {
    match value {
        RawDevelopmentIntent::Preview => ffi::FfiRawDevelopmentIntent::Preview,
        RawDevelopmentIntent::Detail => ffi::FfiRawDevelopmentIntent::Detail,
        RawDevelopmentIntent::ExportImage => ffi::FfiRawDevelopmentIntent::ExportImage,
    }
}

fn raw_development_intent(
    value: ffi::FfiRawDevelopmentIntent,
) -> Result<RawDevelopmentIntent, BridgeError> {
    match value {
        ffi::FfiRawDevelopmentIntent::Preview => Ok(RawDevelopmentIntent::Preview),
        ffi::FfiRawDevelopmentIntent::Detail => Ok(RawDevelopmentIntent::Detail),
        ffi::FfiRawDevelopmentIntent::ExportImage => Ok(RawDevelopmentIntent::ExportImage),
        _ => Err(BridgeError::InvalidRawDevelopmentPlan(
            "decoder returned an unsupported RAW development intent",
        )),
    }
}

fn ffi_raw_development_quality(value: RawDevelopmentQuality) -> ffi::FfiRawDevelopmentQuality {
    match value {
        RawDevelopmentQuality::Fast => ffi::FfiRawDevelopmentQuality::Fast,
        RawDevelopmentQuality::Balanced => ffi::FfiRawDevelopmentQuality::Balanced,
        RawDevelopmentQuality::High => ffi::FfiRawDevelopmentQuality::High,
    }
}

fn raw_development_quality(
    value: ffi::FfiRawDevelopmentQuality,
) -> Result<RawDevelopmentQuality, BridgeError> {
    match value {
        ffi::FfiRawDevelopmentQuality::Fast => Ok(RawDevelopmentQuality::Fast),
        ffi::FfiRawDevelopmentQuality::Balanced => Ok(RawDevelopmentQuality::Balanced),
        ffi::FfiRawDevelopmentQuality::High => Ok(RawDevelopmentQuality::High),
        _ => Err(BridgeError::InvalidRawDevelopmentPlan(
            "decoder returned an unsupported RAW development quality",
        )),
    }
}

fn ffi_dng_opcode_policy(value: DngOpcodePolicy) -> ffi::FfiDngOpcodePolicy {
    match value {
        DngOpcodePolicy::ProviderDefault => ffi::FfiDngOpcodePolicy::ProviderDefault,
        DngOpcodePolicy::RequireApplied => ffi::FfiDngOpcodePolicy::RequireApplied,
        DngOpcodePolicy::DeferToShadow => ffi::FfiDngOpcodePolicy::DeferToShadow,
    }
}

fn dng_opcode_policy(value: ffi::FfiDngOpcodePolicy) -> Result<DngOpcodePolicy, BridgeError> {
    match value {
        ffi::FfiDngOpcodePolicy::ProviderDefault => Ok(DngOpcodePolicy::ProviderDefault),
        ffi::FfiDngOpcodePolicy::RequireApplied => Ok(DngOpcodePolicy::RequireApplied),
        ffi::FfiDngOpcodePolicy::DeferToShadow => Ok(DngOpcodePolicy::DeferToShadow),
        _ => Err(BridgeError::InvalidRawDevelopmentPlan(
            "decoder returned an unsupported DNG opcode policy",
        )),
    }
}

fn ffi_raw_noise_reduction_intent(
    value: RawNoiseReductionIntent,
) -> ffi::FfiRawNoiseReductionIntent {
    match value {
        RawNoiseReductionIntent::ProviderDefault => {
            ffi::FfiRawNoiseReductionIntent::ProviderDefault
        }
        RawNoiseReductionIntent::Disabled => ffi::FfiRawNoiseReductionIntent::Disabled,
        RawNoiseReductionIntent::Conservative => ffi::FfiRawNoiseReductionIntent::Conservative,
        RawNoiseReductionIntent::NoiseRobust => ffi::FfiRawNoiseReductionIntent::NoiseRobust,
    }
}

fn raw_noise_reduction_intent(
    value: ffi::FfiRawNoiseReductionIntent,
) -> Result<RawNoiseReductionIntent, BridgeError> {
    match value {
        ffi::FfiRawNoiseReductionIntent::ProviderDefault => {
            Ok(RawNoiseReductionIntent::ProviderDefault)
        }
        ffi::FfiRawNoiseReductionIntent::Disabled => Ok(RawNoiseReductionIntent::Disabled),
        ffi::FfiRawNoiseReductionIntent::Conservative => Ok(RawNoiseReductionIntent::Conservative),
        ffi::FfiRawNoiseReductionIntent::NoiseRobust => Ok(RawNoiseReductionIntent::NoiseRobust),
        _ => Err(BridgeError::InvalidRawDevelopmentPlan(
            "decoder returned an unsupported RAW noise-reduction intent",
        )),
    }
}

fn ffi_raw_highlight_recovery_intent(
    value: RawHighlightRecoveryIntent,
) -> ffi::FfiRawHighlightRecoveryIntent {
    match value {
        RawHighlightRecoveryIntent::ProviderDefault => {
            ffi::FfiRawHighlightRecoveryIntent::ProviderDefault
        }
        RawHighlightRecoveryIntent::Disabled => ffi::FfiRawHighlightRecoveryIntent::Disabled,
        RawHighlightRecoveryIntent::Conservative => {
            ffi::FfiRawHighlightRecoveryIntent::Conservative
        }
        RawHighlightRecoveryIntent::Aggressive => ffi::FfiRawHighlightRecoveryIntent::Aggressive,
    }
}

fn raw_highlight_recovery_intent(
    value: ffi::FfiRawHighlightRecoveryIntent,
) -> Result<RawHighlightRecoveryIntent, BridgeError> {
    match value {
        ffi::FfiRawHighlightRecoveryIntent::ProviderDefault => {
            Ok(RawHighlightRecoveryIntent::ProviderDefault)
        }
        ffi::FfiRawHighlightRecoveryIntent::Disabled => Ok(RawHighlightRecoveryIntent::Disabled),
        ffi::FfiRawHighlightRecoveryIntent::Conservative => {
            Ok(RawHighlightRecoveryIntent::Conservative)
        }
        ffi::FfiRawHighlightRecoveryIntent::Aggressive => {
            Ok(RawHighlightRecoveryIntent::Aggressive)
        }
        _ => Err(BridgeError::InvalidRawDevelopmentPlan(
            "decoder returned an unsupported RAW highlight-recovery intent",
        )),
    }
}

fn ffi_raw_white_balance(value: RawWhiteBalance) -> (ffi::FfiRawWhiteBalanceMode, u32, i16) {
    match value {
        RawWhiteBalance::AsShot => (
            ffi::FfiRawWhiteBalanceMode::AsShot,
            RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN,
            0,
        ),
        RawWhiteBalance::TemperatureTint { value } => (
            ffi::FfiRawWhiteBalanceMode::TemperatureTint,
            value.temperature_kelvin(),
            value.tint(),
        ),
    }
}

fn raw_white_balance(
    mode: ffi::FfiRawWhiteBalanceMode,
    temperature_kelvin: u32,
    tint: i16,
) -> Result<RawWhiteBalance, BridgeError> {
    match mode {
        ffi::FfiRawWhiteBalanceMode::AsShot => {
            if temperature_kelvin != RAW_WHITE_BALANCE_DEFAULT_TEMPERATURE_KELVIN || tint != 0 {
                return Err(BridgeError::InvalidRawDevelopmentPlan(
                    "decoder returned non-canonical AsShot RAW white balance",
                ));
            }
            Ok(RawWhiteBalance::AsShot)
        }
        ffi::FfiRawWhiteBalanceMode::TemperatureTint => {
            let value = RawTemperatureTint::new(temperature_kelvin, tint).map_err(|_| {
                BridgeError::InvalidRawDevelopmentPlan(
                    "decoder returned an invalid RAW temperature/tint white balance",
                )
            })?;
            Ok(RawWhiteBalance::temperature_tint(value))
        }
        _ => Err(BridgeError::InvalidRawDevelopmentPlan(
            "decoder returned an unsupported RAW white-balance mode",
        )),
    }
}

pub(super) fn ffi_raw_development_plan(plan: RawDevelopmentPlan) -> ffi::FfiRawDevelopmentPlan {
    let (white_balance_mode, temperature_kelvin, tint) = ffi_raw_white_balance(plan.white_balance);
    ffi::FfiRawDevelopmentPlan {
        schema_version: plan.schema_version,
        intent: ffi_raw_development_intent(plan.intent),
        quality: ffi_raw_development_quality(plan.quality),
        dng_opcode_policy: ffi_dng_opcode_policy(plan.dng_opcode_policy),
        noise_reduction: ffi_raw_noise_reduction_intent(plan.noise_reduction),
        highlight_recovery: ffi_raw_highlight_recovery_intent(plan.highlight_recovery),
        white_balance_mode,
        temperature_kelvin,
        tint,
    }
}

fn raw_development_plan(
    plan: ffi::FfiRawDevelopmentPlan,
) -> Result<RawDevelopmentPlan, BridgeError> {
    Ok(RawDevelopmentPlan {
        schema_version: plan.schema_version,
        intent: raw_development_intent(plan.intent)?,
        quality: raw_development_quality(plan.quality)?,
        dng_opcode_policy: dng_opcode_policy(plan.dng_opcode_policy)?,
        noise_reduction: raw_noise_reduction_intent(plan.noise_reduction)?,
        highlight_recovery: raw_highlight_recovery_intent(plan.highlight_recovery)?,
        white_balance: raw_white_balance(
            plan.white_balance_mode,
            plan.temperature_kelvin,
            plan.tint,
        )?,
    })
}

pub(super) fn raw_development_capabilities(
    capabilities: ffi::FfiRawDevelopmentCapabilities,
) -> RawDevelopmentCapabilities {
    RawDevelopmentCapabilities {
        schema_version: capabilities.schema_version,
        available: capabilities.available,
        raw_frame: capabilities.raw_frame,
        dng_opcode_execution_receipt: capabilities.dng_opcode_execution_receipt,
        supported_intents: capabilities.supported_intents,
        supported_qualities: capabilities.supported_qualities,
        supported_dng_opcode_policies: capabilities.supported_dng_opcode_policies,
        supported_noise_reduction_intents: capabilities.supported_noise_reduction_intents,
        supported_highlight_recovery_intents: capabilities.supported_highlight_recovery_intents,
    }
}

fn raw_development_plan_negotiation_status(
    status: ffi::FfiRawDevelopmentPlanNegotiationStatus,
) -> Result<RawDevelopmentPlanNegotiationStatus, BridgeError> {
    match status {
        ffi::FfiRawDevelopmentPlanNegotiationStatus::Accepted => {
            Ok(RawDevelopmentPlanNegotiationStatus::Accepted)
        }
        ffi::FfiRawDevelopmentPlanNegotiationStatus::Adjusted => {
            Ok(RawDevelopmentPlanNegotiationStatus::Adjusted)
        }
        ffi::FfiRawDevelopmentPlanNegotiationStatus::Rejected => {
            Ok(RawDevelopmentPlanNegotiationStatus::Rejected)
        }
        _ => Err(BridgeError::InvalidRawDevelopmentPlan(
            "decoder returned an unsupported RAW development-plan negotiation status",
        )),
    }
}

pub(super) fn raw_development_plan_negotiation(
    negotiation: ffi::FfiRawDevelopmentPlanNegotiation,
) -> Result<RawDevelopmentPlanNegotiation, BridgeError> {
    Ok(RawDevelopmentPlanNegotiation {
        requested: raw_development_plan(negotiation.requested)?,
        effective: raw_development_plan(negotiation.effective)?,
        status: raw_development_plan_negotiation_status(negotiation.status)?,
        unresolved: negotiation.unresolved,
    })
}

pub(super) fn preflight_photo_edit_development(
    handle: &ffi::DecodeHandle,
    plan: RawDevelopmentPlan,
) -> Result<(), BridgeError> {
    let capabilities = handle.capabilities();
    // Do not enter a render-preparation task when the active RAW provider has already declared
    // that it cannot produce editable reference RGB. A large embedded preview may still be
    // perfectly usable in the Library, but it must not leave Precision appearing to develop a
    // source that this provider cannot ever finish.
    if !capabilities.reference_rgb {
        return Err(BridgeError::RawDevelopmentUnavailable(
            if capabilities.embedded_previews {
                "the active local decoder can show this RAW's embedded preview, but cannot develop it for editing"
            } else {
                "the active local decoder cannot develop this RAW for editing"
            },
        ));
    }

    // A provider-owned RawFrame crosses the decoder boundary before development. From there,
    // Shadow's generic RAW developer owns plan negotiation, including high-quality demosaic and
    // RAW-domain denoise. Provider RAW-development capabilities describe only its already
    // processed-RGB compatibility route, so consulting them here would incorrectly reject a
    // valid host RawFrame request (for example a private Nikon CFA provider that deliberately
    // exposes only a conservative compatibility RGB plan).
    //
    // Keep the provider negotiation gate for sources without RawFrame: then the provider is the
    // actual developer and must explicitly accept the requested plan before an edit session starts.
    let raw_capabilities = raw_development_capabilities(handle.raw_development_capabilities());
    if raw_capabilities.available && !capabilities.raw_frame {
        let negotiation = raw_development_plan_negotiation(
            handle.negotiate_raw_development_plan(&ffi_raw_development_plan(plan))?,
        )?;
        if !negotiation.accepted() {
            return Err(BridgeError::RawDevelopmentUnavailable(
                "the active local decoder rejected Shadow's RAW development request",
            ));
        }
    }
    Ok(())
}

fn dng_opcode_execution_status(
    status: ffi::FfiDngOpcodeExecutionStatus,
) -> Result<DngOpcodeExecutionStatus, BridgeError> {
    match status {
        ffi::FfiDngOpcodeExecutionStatus::NotDeclared => Ok(DngOpcodeExecutionStatus::NotDeclared),
        ffi::FfiDngOpcodeExecutionStatus::ProviderDefault => {
            Ok(DngOpcodeExecutionStatus::ProviderDefault)
        }
        ffi::FfiDngOpcodeExecutionStatus::Applied => Ok(DngOpcodeExecutionStatus::Applied),
        ffi::FfiDngOpcodeExecutionStatus::DeferredToShadow => {
            Ok(DngOpcodeExecutionStatus::DeferredToShadow)
        }
        ffi::FfiDngOpcodeExecutionStatus::SkippedForPreview => {
            Ok(DngOpcodeExecutionStatus::SkippedForPreview)
        }
        ffi::FfiDngOpcodeExecutionStatus::Unsupported => Ok(DngOpcodeExecutionStatus::Unsupported),
        _ => Err(BridgeError::InvalidRawDevelopmentPlan(
            "decoder returned an unsupported DNG opcode execution status",
        )),
    }
}

pub(super) fn raw_development_receipt(
    receipt: ffi::FfiRawDevelopmentReceipt,
) -> Result<RawDevelopmentReceipt, BridgeError> {
    Ok(RawDevelopmentReceipt {
        schema_version: receipt.schema_version,
        provider_id: receipt.provider_id,
        provider_version: receipt.provider_version,
        library_version: receipt.library_version,
        development_settings_signature: receipt.development_settings_signature,
        requested_plan_identity: receipt.requested_plan_identity,
        effective_plan_identity: receipt.effective_plan_identity,
        requested_plan: raw_development_plan(receipt.requested_plan)?,
        effective_plan: raw_development_plan(receipt.effective_plan)?,
        plan_negotiation_status: raw_development_plan_negotiation_status(
            receipt.plan_negotiation_status,
        )?,
        processed_linear_reference_contract_version: receipt
            .processed_linear_reference_contract_version,
        declared_image_dimensions: dimensions(&receipt.declared_image_dimensions),
        rendered_dimensions: dimensions(&receipt.rendered_dimensions),
        orientation: receipt.orientation,
        half_size: receipt.half_size,
        use_camera_white_balance: receipt.use_camera_white_balance,
        use_camera_matrix: receipt.use_camera_matrix,
        use_auto_brightness: receipt.use_auto_brightness,
        use_exposure_correction: receipt.use_exposure_correction,
        brightness: receipt.brightness,
        maximum_adjustment_threshold: receipt.maximum_adjustment_threshold,
        output_bits_per_channel: receipt.output_bits_per_channel,
        demosaic_quality: receipt.demosaic_quality,
        output_color: receipt.output_color,
        gamma_inverse_power: receipt.gamma_inverse_power,
        gamma_linear_toe_slope: receipt.gamma_linear_toe_slope,
        declared_dng_opcode_list_bytes: [
            receipt.dng_opcode_list_1_bytes,
            receipt.dng_opcode_list_2_bytes,
            receipt.dng_opcode_list_3_bytes,
        ],
        dng_opcode_execution: [
            dng_opcode_execution_status(receipt.dng_opcode_list_1_execution)?,
            dng_opcode_execution_status(receipt.dng_opcode_list_2_execution)?,
            dng_opcode_execution_status(receipt.dng_opcode_list_3_execution)?,
        ],
        process_warnings: receipt.process_warnings,
    })
}

fn raw_pipeline_path(path: ffi::FfiRawPipelinePath) -> Result<RawPipelinePath, BridgeError> {
    match path {
        ffi::FfiRawPipelinePath::DecodedRaster => Ok(RawPipelinePath::DecodedRaster),
        ffi::FfiRawPipelinePath::ShadowRawFrame => Ok(RawPipelinePath::ShadowRawFrame),
        ffi::FfiRawPipelinePath::ProviderProcessedCompatibility => {
            Ok(RawPipelinePath::ProviderProcessedCompatibility)
        }
        _ => Err(BridgeError::InvalidRawPipelineReceipt(
            "decoder returned an unsupported RAW pipeline path",
        )),
    }
}

fn raw_camera_profile_status(
    status: ffi::FfiRawCameraProfileStatus,
) -> Result<RawCameraProfileStatus, BridgeError> {
    match status {
        ffi::FfiRawCameraProfileStatus::NotConsidered => Ok(RawCameraProfileStatus::NotConsidered),
        ffi::FfiRawCameraProfileStatus::NoMatch => Ok(RawCameraProfileStatus::NoMatch),
        ffi::FfiRawCameraProfileStatus::Applied => Ok(RawCameraProfileStatus::Applied),
        ffi::FfiRawCameraProfileStatus::MatchedNotApplied => {
            Ok(RawCameraProfileStatus::MatchedNotApplied)
        }
        _ => Err(BridgeError::InvalidRawPipelineReceipt(
            "decoder returned an unsupported RAW camera-profile status",
        )),
    }
}

// This is one ordered fail-closed validation of a versioned FFI receipt. Keeping the route,
// profile-status, and field-coherence checks together makes the accepted wire states auditable.
#[allow(clippy::too_many_lines)]
pub(super) fn raw_pipeline_receipt(
    receipt: ffi::FfiRawPipelineReceipt,
) -> Result<RawPipelineReceipt, BridgeError> {
    if receipt.schema_version == 0 {
        return Ok(RawPipelineReceipt::default());
    }

    let path = raw_pipeline_path(receipt.path)?;
    let requested_plan = raw_development_plan(receipt.requested_plan)?;
    let effective_plan = raw_development_plan(receipt.effective_plan)?;
    let fallback_reason = (!receipt.fallback_reason.is_empty()).then_some(receipt.fallback_reason);
    let camera_profile_status = raw_camera_profile_status(receipt.camera_profile_status)?;
    let camera_profile_diagnostic = (!receipt.camera_profile_diagnostic.is_empty())
        .then_some(receipt.camera_profile_diagnostic);

    if receipt.cache_identity.is_empty() || receipt.pipeline_identity.is_empty() {
        return Err(BridgeError::InvalidRawPipelineReceipt(
            "recorded RAW pipeline identities must not be empty",
        ));
    }
    if requested_plan.schema_version != RawDevelopmentPlan::CURRENT_SCHEMA_VERSION
        || effective_plan.schema_version != RawDevelopmentPlan::CURRENT_SCHEMA_VERSION
    {
        return Err(BridgeError::InvalidRawPipelineReceipt(
            "recorded RAW pipeline plans use an unsupported schema",
        ));
    }
    if receipt.schema_version == RawPipelineReceipt::CURRENT_SCHEMA_VERSION {
        match path {
            RawPipelinePath::ShadowRawFrame => {
                if receipt.raw_frame_schema_version == 0
                    || receipt.raw_developer_version == 0
                    || fallback_reason.is_some()
                {
                    return Err(BridgeError::InvalidRawPipelineReceipt(
                        "Shadow RawFrame route provenance is incomplete",
                    ));
                }
            }
            RawPipelinePath::DecodedRaster => {
                if receipt.raw_frame_schema_version != 0
                    || receipt.raw_developer_version != 0
                    || fallback_reason.is_some()
                {
                    return Err(BridgeError::InvalidRawPipelineReceipt(
                        "decoded raster route contains RAW-only provenance",
                    ));
                }
            }
            RawPipelinePath::ProviderProcessedCompatibility => {
                if receipt.raw_frame_schema_version != 0 || receipt.raw_developer_version != 0 {
                    return Err(BridgeError::InvalidRawPipelineReceipt(
                        "provider compatibility route contains Shadow RawFrame provenance",
                    ));
                }
            }
        }

        let camera_profile_route_is_valid = match path {
            RawPipelinePath::ShadowRawFrame => {
                camera_profile_status != RawCameraProfileStatus::NotConsidered
            }
            RawPipelinePath::DecodedRaster | RawPipelinePath::ProviderProcessedCompatibility => {
                camera_profile_status == RawCameraProfileStatus::NotConsidered
            }
        };
        if !camera_profile_route_is_valid {
            return Err(BridgeError::InvalidRawPipelineReceipt(
                "RAW camera-profile status does not match its source route",
            ));
        }

        let camera_profile_fields_are_valid = match camera_profile_status {
            RawCameraProfileStatus::NotConsidered => {
                receipt.camera_profile_catalog_identity.is_empty()
                    && receipt.camera_profile_identity.is_empty()
                    && receipt.camera_profile_name.is_empty()
                    && camera_profile_diagnostic.is_none()
                    && receipt.camera_profile_developer_version == 0
            }
            RawCameraProfileStatus::NoMatch => {
                !receipt.camera_profile_catalog_identity.is_empty()
                    && receipt.camera_profile_identity.is_empty()
                    && receipt.camera_profile_name.is_empty()
                    && camera_profile_diagnostic.is_none()
                    && receipt.camera_profile_developer_version
                        == RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION
            }
            RawCameraProfileStatus::Applied => {
                !receipt.camera_profile_catalog_identity.is_empty()
                    && !receipt.camera_profile_identity.is_empty()
                    && !receipt.camera_profile_name.is_empty()
                    && camera_profile_diagnostic.is_none()
                    && receipt.camera_profile_developer_version
                        == RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION
            }
            RawCameraProfileStatus::MatchedNotApplied => {
                !receipt.camera_profile_catalog_identity.is_empty()
                    && !receipt.camera_profile_identity.is_empty()
                    && !receipt.camera_profile_name.is_empty()
                    && camera_profile_diagnostic.is_some()
                    && receipt.camera_profile_developer_version
                        == RawPipelineReceipt::CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION
            }
        };
        if !camera_profile_fields_are_valid {
            return Err(BridgeError::InvalidRawPipelineReceipt(
                "RAW camera-profile provenance does not match its status",
            ));
        }
    }

    Ok(RawPipelineReceipt {
        schema_version: receipt.schema_version,
        path,
        cache_identity: receipt.cache_identity,
        pipeline_identity: receipt.pipeline_identity,
        source_provider_id: receipt.source_provider_id,
        source_provider_version: receipt.source_provider_version,
        fallback_reason,
        raw_frame_schema_version: receipt.raw_frame_schema_version,
        raw_developer_version: receipt.raw_developer_version,
        requested_plan,
        effective_plan,
        camera_profile_status,
        camera_profile_catalog_identity: receipt.camera_profile_catalog_identity,
        camera_profile_identity: receipt.camera_profile_identity,
        camera_profile_name: receipt.camera_profile_name,
        camera_profile_diagnostic,
        camera_profile_developer_version: receipt.camera_profile_developer_version,
    })
}
