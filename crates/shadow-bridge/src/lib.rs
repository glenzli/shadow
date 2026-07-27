//! Safe, coarse-grained Rust access to Shadow's C++ image decoder providers.
//!
//! Navigation follows the public contract families in this file: `ffi` defines the generated CXX
//! wire shape; `RawDevelopmentPlan` and receipt types own source-development provenance;
//! `DecodedDisplayLuma` owns bounded proxy analysis; `AdjustmentRenderPlan` owns validated edit
//! execution; preview/detail session types own reusable native buffers; and the final public
//! functions expose inspection and one-shot render entry points. Responsibility-indexed private
//! bridge tests live in `tests`.
//!
//! The generated CXX wire declaration remains centralized and auditable. Safe Rust contract
//! families move into responsibility-named modules when they own independent invariants and
//! failure policy; sharing `ffi` does not require sharing one production file.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
#[cfg(test)]
use shadow_domain::PreviewCodec;
use shadow_domain::{ImageDimensions, RawMetadataSnapshot};
use thiserror::Error;

mod adjustment;
mod decoder;
mod detail_session;
mod display_luma;
mod optics;
mod preview_analysis;
mod preview_session;
mod raw_development;
mod render_wire;

#[cfg(test)]
use adjustment::validate_render_operation;
pub use adjustment::*;
use adjustment::{validate_jpeg_quality, validate_proxy_max_edge};
use decoder::{dimensions, open_libraw, open_photo, preview_codec};
pub use decoder::{
    extract_best_libraw_preview, extract_best_photo_preview, inspect_libraw, inspect_photo,
};
pub use detail_session::*;
pub use display_luma::{
    DecodedDisplayLuma, JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX, MAX_JPEG_DISPLAY_LUMA_EDGE,
    decode_jpeg_display_luma,
};
pub use optics::*;
pub use preview_analysis::*;
#[cfg(test)]
use preview_analysis::{edit_preview_execution_receipt, validate_edit_preview_analysis};
pub use preview_session::*;
pub use raw_development::*;
use raw_development::{
    ffi_raw_development_plan, raw_development_capabilities, raw_development_plan_negotiation,
};
#[cfg(test)]
use raw_development::{raw_development_receipt, raw_pipeline_receipt};
#[cfg(test)]
use render_wire::ffi_render_node;
use render_wire::{ffi_render_request, proxy_payload};

#[cxx::bridge(namespace = "shadow::bridge")]
mod ffi {
    #[derive(Debug)]
    enum FfiPreviewFormat {
        Unknown,
        Jpeg,
        Bitmap,
        JpegXl,
        H265,
    }

    #[derive(Debug)]
    enum FfiByteOrder {
        NotApplicable,
        Native,
        LittleEndian,
        BigEndian,
    }

    #[derive(Debug)]
    struct FfiDimensions {
        width: u32,
        height: u32,
    }

    #[derive(Debug)]
    struct FfiMargins {
        left: u32,
        top: u32,
        right: u32,
        bottom: u32,
    }

    #[derive(Debug, Clone)]
    struct FfiOpticsSettings {
        schema_version: u32,
        enabled: bool,
        correct_distortion: bool,
        correct_tca: bool,
        correct_vignetting: bool,
        automatic_scale: bool,
        manual_distortion: i16,
        manual_tca_red_cyan: i16,
        manual_tca_blue_yellow: i16,
        manual_vignetting_amount: i16,
        manual_vignetting_midpoint: u8,
        camera_profile_maker: String,
        camera_profile_model: String,
        lens_profile_maker: String,
        lens_profile_model: String,
    }

    #[derive(Debug)]
    struct FfiOpticsReceipt {
        status: String,
        provider_id: String,
        provider_version: String,
        camera_profile: String,
        lens_profile: String,
        distortion_available: bool,
        tca_available: bool,
        vignetting_available: bool,
        applied_distortion: bool,
        applied_tca: bool,
        applied_vignetting: bool,
        vignetting_used_distance_fallback: bool,
        applied_scaling: bool,
    }

    #[derive(Debug)]
    enum FfiRawDevelopmentIntent {
        Preview,
        Detail,
        ExportImage,
    }

    #[derive(Debug)]
    enum FfiRawDevelopmentQuality {
        Fast,
        Balanced,
        High,
    }

    #[derive(Debug)]
    enum FfiDngOpcodePolicy {
        ProviderDefault,
        RequireApplied,
        DeferToShadow,
    }

    #[derive(Debug)]
    enum FfiRawNoiseReductionIntent {
        ProviderDefault,
        Disabled,
        Conservative,
        NoiseRobust,
    }

    #[derive(Debug)]
    enum FfiRawHighlightRecoveryIntent {
        ProviderDefault,
        Disabled,
        Conservative,
        Aggressive,
    }

    #[derive(Debug)]
    enum FfiRawDevelopmentPlanNegotiationStatus {
        Accepted,
        Adjusted,
        Rejected,
    }

    #[derive(Debug)]
    enum FfiDngOpcodeExecutionStatus {
        NotDeclared,
        ProviderDefault,
        Applied,
        DeferredToShadow,
        SkippedForPreview,
        Unsupported,
    }

    #[derive(Debug, Clone, Copy)]
    struct FfiRawDevelopmentPlan {
        schema_version: u32,
        intent: FfiRawDevelopmentIntent,
        quality: FfiRawDevelopmentQuality,
        dng_opcode_policy: FfiDngOpcodePolicy,
        noise_reduction: FfiRawNoiseReductionIntent,
        highlight_recovery: FfiRawHighlightRecoveryIntent,
    }

    #[derive(Debug, Clone, Copy)]
    #[allow(clippy::struct_excessive_bools)]
    struct FfiRawDevelopmentCapabilities {
        schema_version: u32,
        available: bool,
        raw_frame: bool,
        dng_opcode_execution_receipt: bool,
        supported_intents: u32,
        supported_qualities: u32,
        supported_dng_opcode_policies: u32,
        supported_noise_reduction_intents: u32,
        supported_highlight_recovery_intents: u32,
    }

    #[derive(Debug, Clone, Copy)]
    struct FfiRawDevelopmentPlanNegotiation {
        requested: FfiRawDevelopmentPlan,
        effective: FfiRawDevelopmentPlan,
        status: FfiRawDevelopmentPlanNegotiationStatus,
        unresolved: u32,
    }

    // Deliberately a fixed field set: this is source-render provenance rather than an
    // extensible recipe payload. New renderer semantics require a new receipt schema instead
    // of silently overloading a map or an untyped byte document.
    #[derive(Debug)]
    #[allow(clippy::struct_excessive_bools)]
    struct FfiRawDevelopmentReceipt {
        schema_version: u32,
        provider_id: String,
        provider_version: String,
        library_version: String,
        development_settings_signature: String,
        requested_plan_identity: String,
        effective_plan_identity: String,
        requested_plan: FfiRawDevelopmentPlan,
        effective_plan: FfiRawDevelopmentPlan,
        plan_negotiation_status: FfiRawDevelopmentPlanNegotiationStatus,
        processed_linear_reference_contract_version: u32,
        declared_image_dimensions: FfiDimensions,
        rendered_dimensions: FfiDimensions,
        orientation: i32,
        half_size: bool,
        use_camera_white_balance: bool,
        use_camera_matrix: bool,
        use_auto_brightness: bool,
        use_exposure_correction: bool,
        brightness: f32,
        maximum_adjustment_threshold: f32,
        output_bits_per_channel: u16,
        demosaic_quality: i32,
        output_color: i32,
        gamma_inverse_power: f64,
        gamma_linear_toe_slope: f64,
        dng_opcode_list_1_bytes: u32,
        dng_opcode_list_2_bytes: u32,
        dng_opcode_list_3_bytes: u32,
        dng_opcode_list_1_execution: FfiDngOpcodeExecutionStatus,
        dng_opcode_list_2_execution: FfiDngOpcodeExecutionStatus,
        dng_opcode_list_3_execution: FfiDngOpcodeExecutionStatus,
        process_warnings: u32,
    }

    #[derive(Debug)]
    enum FfiRawPipelinePath {
        DecodedRaster,
        ShadowRawFrame,
        ProviderProcessedCompatibility,
    }

    #[derive(Debug)]
    enum FfiRawCameraProfileStatus {
        NotConsidered,
        NoMatch,
        Applied,
        MatchedNotApplied,
    }

    // Host-side route provenance. `cache_identity` is the canonical composite identity produced
    // by C++; `pipeline_identity` identifies only the selected algorithm/compatibility stage.
    #[derive(Debug)]
    struct FfiRawPipelineReceipt {
        schema_version: u32,
        path: FfiRawPipelinePath,
        cache_identity: String,
        pipeline_identity: String,
        source_provider_id: String,
        source_provider_version: String,
        fallback_reason: String,
        raw_frame_schema_version: u32,
        raw_developer_version: u32,
        requested_plan: FfiRawDevelopmentPlan,
        effective_plan: FfiRawDevelopmentPlan,
        camera_profile_status: FfiRawCameraProfileStatus,
        camera_profile_catalog_identity: String,
        camera_profile_identity: String,
        camera_profile_name: String,
        camera_profile_diagnostic: String,
        camera_profile_developer_version: u32,
    }

    #[derive(Debug)]
    struct FfiOpticsProfileCandidate {
        camera_maker: String,
        camera_model: String,
        lens_maker: String,
        lens_model: String,
    }

    #[derive(Debug)]
    struct FfiProviderSnapshot {
        id: String,
        version: String,
        dng_sdk: bool,
        rawspeed: bool,
        jpeg: bool,
    }

    #[derive(Debug)]
    struct FfiMetadataSnapshot {
        make: String,
        model: String,
        normalized_make: String,
        normalized_model: String,
        dng_version: String,
        raw_count: u32,
        raw_dimensions: FfiDimensions,
        image_dimensions: FfiDimensions,
        margins: FfiMargins,
        orientation: i32,
        cfa_pattern: String,
        sensor_colors: u32,
        sensor_bits: u32,
        black_level: u32,
        white_level: u32,
        as_shot_neutral_r: f64,
        as_shot_neutral_g1: f64,
        as_shot_neutral_b: f64,
        as_shot_neutral_g2: f64,
        baseline_exposure: f64,
        iso_speed: f64,
        exposure_time_seconds: f64,
        aperture_f_number: f64,
        focal_length_mm: f64,
        captured_at_unix_seconds: i64,
        lens_make: String,
        lens_model: String,
        focal_length_35mm: f64,
    }

    #[derive(Debug)]
    #[allow(clippy::struct_excessive_bools)]
    struct FfiCapabilitySnapshot {
        metadata: bool,
        embedded_previews: bool,
        raw_frame: bool,
        reference_rgb: bool,
        dng_opcode_list_1_bytes: u32,
        dng_opcode_list_2_bytes: u32,
        dng_opcode_list_3_bytes: u32,
        raw_development: FfiRawDevelopmentCapabilities,
    }

    #[derive(Debug)]
    struct FfiPreviewSnapshot {
        provider_id: usize,
        format: FfiPreviewFormat,
        dimensions: FfiDimensions,
        bits_per_channel: u16,
        channels: u16,
        encoded_bytes: u64,
        decodable: bool,
    }

    #[derive(Debug)]
    struct FfiPreviewPayload {
        present: bool,
        descriptor: FfiPreviewSnapshot,
        byte_order: FfiByteOrder,
        bytes: Vec<u8>,
    }

    #[derive(Debug)]
    struct FfiEncodedProxy {
        dimensions: FfiDimensions,
        format: FfiPreviewFormat,
        bits_per_channel: u16,
        channels: u16,
        bytes: Vec<u8>,
    }

    #[derive(Debug)]
    struct FfiEditPreviewAnalysis {
        version: String,
        sample_dimensions: FfiDimensions,
        red: Vec<u64>,
        green: Vec<u64>,
        blue: Vec<u64>,
        luma: Vec<u64>,
        below_zero_samples: Vec<u64>,
        above_one_samples: Vec<u64>,
        hdr_headroom_bins: Vec<u64>,
        hdr_headroom_pixels: u64,
        hdr_peak_headroom_ev: f64,
        pixel_count: u64,
        shadow_clipped_pixels: u64,
        highlight_clipped_pixels: u64,
    }

    #[derive(Debug)]
    enum FfiEditPreviewBackend {
        Cpu,
        Metal,
    }

    #[derive(Debug)]
    struct FfiEditPreviewExecutionReceipt {
        schema_version: u32,
        cache_identity: String,
        adjustment_backend: FfiEditPreviewBackend,
        adjustment_backend_version: u32,
        adjustment_execution_contract_version: u32,
        display_backend: FfiEditPreviewBackend,
        display_backend_version: u32,
        display_output_contract_version: u32,
        fused_pipeline: bool,
        adjustment_fell_back: bool,
        display_fell_back: bool,
        diagnostic: String,
    }

    #[derive(Debug)]
    struct FfiAnalyzedEditPreview {
        proxy: FfiEncodedProxy,
        analysis: FfiEditPreviewAnalysis,
        execution: FfiEditPreviewExecutionReceipt,
    }

    #[derive(Debug)]
    struct FfiCancellableEncodedProxy {
        cancelled: bool,
        proxy: FfiEncodedProxy,
    }

    #[derive(Debug)]
    struct FfiCancellableAnalyzedEditPreview {
        cancelled: bool,
        preview: FfiAnalyzedEditPreview,
    }

    /// A compact RAW-source diagnostic in the exact display dimensions of the prepared preview.
    /// It is absent for display-referred sources and for providers that cannot supply RawFrame.
    #[derive(Debug)]
    struct FfiSensorClippingMask {
        available: bool,
        dimensions: FfiDimensions,
        samples: Vec<u8>,
        highlight_pixel_count: u64,
        shadow_pixel_count: u64,
    }

    #[derive(Debug)]
    struct FfiDisplayLuma {
        width: u32,
        height: u32,
        stride: u32,
        samples: Vec<f32>,
        preprocessing_version: String,
    }

    #[derive(Debug)]
    enum FfiAdjustmentOperation {
        LocalMaskLayerStart,
        LocalMaskLayerEnd,
        Exposure,
        Contrast,
        OklabLightnessToneCurve,
        RgbWhiteBalance,
        Saturation,
        SelectiveTone,
        PerceptualColor,
        OklabColorWarper,
        Lut3D,
        Sharpen,
        SpotHeal,
    }

    #[derive(Debug, Clone, Copy)]
    enum FfiDetailEffectsPass {
        TechnicalDetail,
        ColorGrading,
        FinishingEffects,
    }

    #[derive(Debug)]
    struct FfiAdjustmentNode {
        node_id: String,
        operation: FfiAdjustmentOperation,
        detail_effects_pass: FfiDetailEffectsPass,
        parameter_schema_version: u32,
        implementation_version: u32,
        enabled: bool,
        parameters: Vec<f64>,
        /// Operation-specific immutable binary document. Only Lut3D accepts
        /// a validated `.cube` document; every other operation requires empty.
        payload: Vec<u8>,
        /// Lengths for grouped variable-size parameters. Perceptual Color
        /// stores the number of additional sampled color ranges; Spot Heal
        /// stores its target count.
        parameter_group_lengths: Vec<u32>,
    }

    #[derive(Debug, Clone, Copy)]
    struct FfiPhotoGeometry {
        crop_left: f64,
        crop_top: f64,
        crop_right: f64,
        crop_bottom: f64,
        quarter_turn: u8,
        straighten_degrees: f64,
        flip_horizontal: bool,
        flip_vertical: bool,
    }

    #[derive(Debug)]
    struct FfiAdjustmentRenderRequest {
        nodes: Vec<FfiAdjustmentNode>,
        geometry: FfiPhotoGeometry,
        max_edge: u32,
        jpeg_quality: u8,
    }

    #[derive(Debug, Clone, Copy)]
    struct FfiDetailTileRect {
        x: u32,
        y: u32,
        width: u32,
        height: u32,
    }

    #[derive(Debug)]
    struct FfiAdjustmentDetailTileRequest {
        nodes: Vec<FfiAdjustmentNode>,
        geometry: FfiPhotoGeometry,
        rect: FfiDetailTileRect,
    }

    #[derive(Debug)]
    struct FfiRenderedDetailTile {
        rect: FfiDetailTileRect,
        full_dimensions: FfiDimensions,
        row_stride_bytes: u32,
        bytes: Vec<u8>,
    }

    unsafe extern "C++" {
        include!("shadow/image/cxx_bridge.hpp");

        type DecodeHandle;
        type EditPreviewHandle;
        type EditPreviewCancellationHandle;
        type FullEditDetailHandle;

        fn open_libraw_utf8(path: &str) -> Result<UniquePtr<DecodeHandle>>;
        fn open_photo_utf8(path: &str) -> Result<UniquePtr<DecodeHandle>>;
        fn query_libraw_optics_profiles_utf8(path: &str) -> Result<Vec<FfiOpticsProfileCandidate>>;
        fn query_photo_optics_profiles_utf8(path: &str) -> Result<Vec<FfiOpticsProfileCandidate>>;
        fn query_optics_profiles_for_metadata(
            metadata: &FfiMetadataSnapshot,
        ) -> Vec<FfiOpticsProfileCandidate>;
        fn libraw_provider_version() -> String;
        fn photo_provider_version() -> String;
        fn edit_preview_generator_implementation_identity() -> String;
        fn new_edit_preview_cancellation() -> Result<SharedPtr<EditPreviewCancellationHandle>>;
        fn cancel(self: &EditPreviewCancellationHandle) -> bool;
        fn photo_supported_raster_extensions() -> Vec<String>;
        fn raw_development_plan_identity(plan: &FfiRawDevelopmentPlan) -> Result<String>;
        fn render_photo_reference_proxy(
            path: &str,
            max_edge: u32,
            jpeg_quality: u8,
        ) -> Result<FfiEncodedProxy>;
        fn decode_jpeg_display_luma(encoded: &[u8], max_edge: u32) -> Result<FfiDisplayLuma>;
        fn provider(self: &DecodeHandle) -> FfiProviderSnapshot;
        fn metadata(self: &DecodeHandle) -> FfiMetadataSnapshot;
        fn capabilities(self: &DecodeHandle) -> FfiCapabilitySnapshot;
        fn raw_development_capabilities(self: &DecodeHandle) -> FfiRawDevelopmentCapabilities;
        fn negotiate_raw_development_plan(
            self: &DecodeHandle,
            plan: &FfiRawDevelopmentPlan,
        ) -> Result<FfiRawDevelopmentPlanNegotiation>;
        // The current public Rust API opens a prepared session directly. Keep this lower-level
        // read-only accessor for C++/future bridge callers, where it reports an explicit default
        // before a source render is prepared.
        #[allow(dead_code)]
        fn raw_development_receipt(self: &DecodeHandle) -> Result<FfiRawDevelopmentReceipt>;
        #[allow(dead_code)]
        fn raw_pipeline_receipt(self: &DecodeHandle) -> Result<FfiRawPipelineReceipt>;
        fn previews(self: &DecodeHandle) -> Vec<FfiPreviewSnapshot>;
        fn decode_best_preview(self: Pin<&mut DecodeHandle>) -> Result<FfiPreviewPayload>;
        fn configure_optics(
            self: Pin<&mut DecodeHandle>,
            settings: &FfiOpticsSettings,
        ) -> Result<()>;
        fn render_reference_proxy(
            self: &DecodeHandle,
            max_edge: u32,
            jpeg_quality: u8,
        ) -> Result<FfiEncodedProxy>;
        fn render_adjustment_plan(
            self: &DecodeHandle,
            request: &FfiAdjustmentRenderRequest,
        ) -> Result<FfiEncodedProxy>;
        #[allow(dead_code)]
        fn prepare_edit_preview(
            self: &DecodeHandle,
            max_edge: u32,
        ) -> Result<UniquePtr<EditPreviewHandle>>;
        fn prepare_edit_preview_with_raw_development_plan(
            self: &DecodeHandle,
            max_edge: u32,
            plan: &FfiRawDevelopmentPlan,
        ) -> Result<UniquePtr<EditPreviewHandle>>;
        #[allow(dead_code)]
        fn prepare_edit_detail(self: &DecodeHandle) -> Result<UniquePtr<FullEditDetailHandle>>;
        fn prepare_edit_detail_with_raw_development_plan(
            self: &DecodeHandle,
            plan: &FfiRawDevelopmentPlan,
        ) -> Result<UniquePtr<FullEditDetailHandle>>;
        fn dimensions(self: &EditPreviewHandle) -> FfiDimensions;
        fn max_edge(self: &EditPreviewHandle) -> u32;
        fn optics_receipt(self: &EditPreviewHandle) -> FfiOpticsReceipt;
        fn raw_development_receipt(self: &EditPreviewHandle) -> Result<FfiRawDevelopmentReceipt>;
        fn raw_pipeline_receipt(self: &EditPreviewHandle) -> Result<FfiRawPipelineReceipt>;
        fn sensor_clipping_mask(self: &EditPreviewHandle) -> FfiSensorClippingMask;
        fn render_adjustment_plan(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
        ) -> Result<FfiEncodedProxy>;
        fn render_adjustment_plan_with_analysis(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
        ) -> Result<FfiAnalyzedEditPreview>;
        fn render_adjustment_plan_cancellable(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
            cancellation: &EditPreviewCancellationHandle,
        ) -> Result<FfiCancellableEncodedProxy>;
        fn render_adjustment_plan_with_analysis_cancellable(
            self: &EditPreviewHandle,
            request: &FfiAdjustmentRenderRequest,
            cancellation: &EditPreviewCancellationHandle,
        ) -> Result<FfiCancellableAnalyzedEditPreview>;
        fn dimensions(self: &FullEditDetailHandle) -> FfiDimensions;
        fn retained_bytes(self: &FullEditDetailHandle) -> u64;
        fn optics_receipt(self: &FullEditDetailHandle) -> FfiOpticsReceipt;
        fn raw_development_receipt(self: &FullEditDetailHandle)
        -> Result<FfiRawDevelopmentReceipt>;
        fn raw_pipeline_receipt(self: &FullEditDetailHandle) -> Result<FfiRawPipelineReceipt>;
        fn render_adjustment_plan_tile(
            self: &FullEditDetailHandle,
            request: &FfiAdjustmentDetailTileRequest,
        ) -> Result<FfiRenderedDetailTile>;
    }
}

/// Returns the complete generated-image identity without opening an image:
/// linked `LibRaw` version plus Shadow's processed-linear reference-RGB and
/// display-output transform versions.
pub fn libraw_provider_version() -> String {
    ffi::libraw_provider_version()
}

/// Returns the cache-facing identity of Shadow's source-neutral photo router.
///
/// Unlike [`libraw_provider_version`], this identity stays stable across every source type the
/// router accepts. Catalog inspection records and generated-proxy cache entries can therefore
/// share it without pretending every source used the same decoder.
#[must_use]
pub fn photo_provider_version() -> String {
    ffi::photo_provider_version()
}

/// Returns the cache-safe implementation contract for warm edit-preview generation.
///
/// This identifies compiled adjustment, display and JPEG contracts, not the effective backend
/// of one render. [`AnalyzedEditPreview::execution`] records that post-selection CPU/Metal route.
#[must_use]
pub fn edit_preview_generator_implementation_identity() -> String {
    ffi::edit_preview_generator_implementation_identity()
}

/// Returns ordinary rendered-image suffixes that the linked native photo router can genuinely
/// open. JPEG is mandatory; HEIF/HEIC is added only when this particular build linked libheif.
#[must_use]
pub fn photo_supported_raster_extensions() -> Vec<String> {
    ffi::photo_supported_raster_extensions()
}

/// Returns the native, canonical cache identity for a source-development request. Keeping this
/// calculation in the C++ image contract prevents Rust, desktop, and a future private provider
/// from accidentally serializing equivalent plans differently.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidRawDevelopmentPlan`] when the plan schema is unsupported, or
/// a decoder bridge error if the native contract rejects an invalid enum representation.
pub fn raw_development_plan_identity(plan: RawDevelopmentPlan) -> Result<String, BridgeError> {
    plan.validate()?;
    Ok(ffi::raw_development_plan_identity(
        &ffi_raw_development_plan(plan),
    )?)
}

/// Opens the source-neutral provider just long enough to negotiate a RAW plan. It does not
/// decode pixels or prepare an edit session. Raster sources correctly report a rejected
/// negotiation because a RAW plan has no effect on their already-rendered source pixels.
///
/// # Errors
///
/// Returns an invalid-plan, path, or decoder bridge error.
pub fn negotiate_photo_raw_development_plan(
    path: &Path,
    plan: RawDevelopmentPlan,
) -> Result<RawDevelopmentPlanNegotiation, BridgeError> {
    plan.validate()?;
    let handle = open_photo(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    raw_development_plan_negotiation(
        handle.negotiate_raw_development_plan(&ffi_raw_development_plan(plan))?,
    )
}

/// Reads the provider-neutral RAW-development capability declaration without rendering pixels.
/// This is the preflight counterpart to [`negotiate_photo_raw_development_plan`]: callers can
/// present only meaningful plan choices before they request a preview or a 1:1 detail source.
///
/// # Errors
///
/// Returns a path or decoder bridge error.
pub fn photo_raw_development_capabilities(
    path: &Path,
) -> Result<RawDevelopmentCapabilities, BridgeError> {
    let handle = open_photo(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    Ok(raw_development_capabilities(
        handle.raw_development_capabilities(),
    ))
}

/// Renders a bounded, display-referred JPEG proxy through the `LibRaw`
/// reference path. This is a fallback for RAW files without an embedded
/// preview, not Shadow's eventual camera-domain renderer.
///
/// # Errors
///
/// Returns [`BridgeError`] when the provider cannot render or encode the RAW.
pub fn render_libraw_reference_proxy(
    path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    let handle = open_libraw(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    let proxy = handle.render_reference_proxy(max_edge, jpeg_quality)?;
    Ok(shadow_domain::ProxyPayload {
        dimensions: dimensions(&proxy.dimensions),
        codec: preview_codec(proxy.format),
        bits_per_channel: proxy.bits_per_channel,
        channels: proxy.channels,
        bytes: proxy.bytes,
    })
}

/// Renders a bounded display-referred JPEG proxy through Shadow's source-neutral photo router.
///
/// The router decides whether the source is developed through a RAW provider or decoded from a
/// supported raster. The returned raster is always the same display-proxy contract; inspect the
/// prepared edit session's [`RawDevelopmentReceipt`] to distinguish provider-side RAW
/// development from a raster source.
///
/// # Errors
///
/// Returns an invalid-request, path, source-router, decode, or encoding error.
pub fn render_photo_reference_proxy(
    path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    validate_proxy_max_edge(max_edge)?;
    validate_jpeg_quality(jpeg_quality)?;
    let utf8_path = path
        .to_str()
        .ok_or_else(|| BridgeError::NonUtf8Path(path.to_path_buf()))?;
    Ok(proxy_payload(ffi::render_photo_reference_proxy(
        utf8_path,
        max_edge,
        jpeg_quality,
    )?))
}

/// Renders the fixed-order basic edit recipe as a bounded standard JPEG.
///
/// The C++ kernel configures `LibRaw` to emit 16-bit processed linear-light RGB
/// in sRGB/Rec.709-D65 primaries, normalizes it into the float working proxy,
/// and executes adjustment nodes without intermediate clipping. The versioned
/// display boundary then performs hue-preserving Oklab gamut mapping and the
/// sRGB transfer only while encoding RGB8/JPEG. The `LibRaw` result has already
/// undergone black subtraction, white balance, demosaic, camera-to-output color
/// conversion, and fixed integer scaling (with frame-adaptive maximum disabled);
/// it is not untouched sensor-linear mosaic/radiance data or
/// the eventual full RAW color pipeline.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidEditRequest`] before RAW I/O for non-finite,
/// out-of-range, or unbounded parameters. Decoder and encoder failures are
/// returned as [`BridgeError::Decoder`].
pub fn render_libraw_edited_proxy(
    path: &Path,
    request: EditedProxyRequest,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    request.validate()?;
    let plan = basic_adjustment_render_plan(request.edits)?;
    render_libraw_adjustment_plan(path, &plan, request.max_edge, request.jpeg_quality)
}

/// Renders a typed, dependency-ordered adjustment plan as a bounded JPEG.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidEditRequest`] before RAW I/O for an invalid
/// plan, edge bound, or JPEG quality. Decoder, executor, and encoder failures
/// are returned as [`BridgeError::Decoder`].
pub fn render_libraw_adjustment_plan(
    path: &Path,
    plan: &AdjustmentRenderPlan,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<shadow_domain::ProxyPayload, BridgeError> {
    plan.validate()?;
    validate_proxy_max_edge(max_edge)?;
    validate_jpeg_quality(jpeg_quality)?;
    let handle = open_libraw(path)?;
    let handle = handle.as_ref().ok_or(BridgeError::NullHandle)?;
    let ffi_request = ffi_render_request(plan, max_edge, jpeg_quality);
    let proxy = handle.render_adjustment_plan(&ffi_request)?;
    Ok(proxy_payload(proxy))
}

#[derive(Debug, Error)]
pub enum BridgeError {
    #[error("invalid RAW development plan: {0}")]
    InvalidRawDevelopmentPlan(&'static str),
    #[error("invalid RAW pipeline receipt: {0}")]
    InvalidRawPipelineReceipt(&'static str),
    #[error("invalid edited proxy request: {0}")]
    InvalidEditRequest(&'static str),
    #[error("invalid edit-preview analysis bridge output: {0}")]
    InvalidEditPreviewOutput(&'static str),
    #[error("invalid full edit detail bridge output: {0}")]
    InvalidEditDetailOutput(&'static str),
    #[error("RAW development is unavailable: {0}")]
    RawDevelopmentUnavailable(&'static str),
    #[error("invalid JPEG display-luma request: {0}")]
    InvalidDisplayLumaRequest(&'static str),
    #[error("invalid JPEG display-luma decoder output: {0}")]
    InvalidDisplayLumaOutput(&'static str),
    #[error("the Mac-first decoder bridge currently requires a UTF-8 path: {0}")]
    NonUtf8Path(PathBuf),
    #[error("the C++ decoder bridge returned a null handle")]
    NullHandle,
    #[error("C++ decoder error: {0}")]
    Decoder(#[from] cxx::Exception),
}

#[cfg(test)]
mod tests;
