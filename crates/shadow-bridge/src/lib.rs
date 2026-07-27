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

use std::{
    collections::HashSet,
    path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};
use shadow_domain::{ImageDimensions, PreviewCodec, RawMetadataSnapshot};
use thiserror::Error;

mod decoder;
mod display_luma;

use decoder::{dimensions, open_libraw, open_photo, preview_codec};
pub use decoder::{
    extract_best_libraw_preview, extract_best_photo_preview, inspect_libraw, inspect_photo,
};
pub use display_luma::{
    DecodedDisplayLuma, JPEG_DISPLAY_LUMA_PREPROCESSING_VERSION_PREFIX, MAX_JPEG_DISPLAY_LUMA_EDGE,
    decode_jpeg_display_luma,
};

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

// SAFETY: the C++ handle owns a fully prepared, immutable float working proxy. It contains no
// decoder or borrowed state, its destructor is thread-independent, and every render allocates
// its edit buffer and libjpeg state locally. C++ contract tests exercise repeated const renders;
// the public Rust wrapper exposes no mutable access to the handle.
unsafe impl Send for ffi::EditPreviewHandle {}
// SAFETY: see the Send implementation above. Concurrent calls only read the working proxy.
unsafe impl Sync for ffi::EditPreviewHandle {}

// SAFETY: the native handle owns only std::stop_source. request_stop() and token copies are
// thread-safe by the C++20 stop-token contract, and Rust receives it only through SharedPtr.
unsafe impl Send for ffi::EditPreviewCancellationHandle {}
// SAFETY: see Send above; all shared access is const except stop_source's synchronized
// request_stop operation.
unsafe impl Sync for ffi::EditPreviewCancellationHandle {}

// SAFETY: the C++ handle owns a fully prepared, immutable u16 reference image. It contains no
// decoder or borrowed state, and every tile render allocates independent float/RGB8 buffers.
// The public wrapper exposes no mutable access to the handle.
unsafe impl Send for ffi::FullEditDetailHandle {}
// SAFETY: see the Send implementation above. Concurrent calls only read the retained source.
unsafe impl Sync for ffi::FullEditDetailHandle {}

/// Cache-key version for the fixed-order basic edited-preview recipe.
pub const BASIC_EDIT_PREVIEW_RECIPE_VERSION: u32 = 1;

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
        }
    }

    fn validate(self) -> Result<(), BridgeError> {
        if self.schema_version != Self::CURRENT_SCHEMA_VERSION {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "RAW development plan uses an unsupported schema",
            ));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RawDevelopmentPlanNegotiationStatus {
    Accepted,
    Adjusted,
    Rejected,
}

impl Default for RawDevelopmentPlanNegotiationStatus {
    fn default() -> Self {
        Self::Rejected
    }
}

#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DngOpcodeExecutionStatus {
    NotDeclared,
    ProviderDefault,
    Applied,
    DeferredToShadow,
    SkippedForPreview,
    Unsupported,
}

impl Default for DngOpcodeExecutionStatus {
    fn default() -> Self {
        Self::NotDeclared
    }
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
    pub const CURRENT_CAMERA_PROFILE_DEVELOPER_VERSION: u32 = 1;

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

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct OpticsProfileCandidate {
    pub camera_maker: String,
    pub camera_model: String,
    pub lens_maker: String,
    pub lens_model: String,
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

fn ffi_metadata_snapshot(metadata: &RawMetadataSnapshot) -> ffi::FfiMetadataSnapshot {
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
        captured_at_unix_seconds: metadata.captured_at_unix_seconds,
        lens_make: metadata.lens_make.clone(),
        lens_model: metadata.lens_model.clone(),
        focal_length_35mm: metadata.focal_length_35mm,
    }
}

fn ffi_optics_settings(settings: &OpticsSettings) -> ffi::FfiOpticsSettings {
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

fn optics_receipt(receipt: ffi::FfiOpticsReceipt) -> OpticsReceipt {
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

fn ffi_raw_development_plan(plan: RawDevelopmentPlan) -> ffi::FfiRawDevelopmentPlan {
    ffi::FfiRawDevelopmentPlan {
        schema_version: plan.schema_version,
        intent: ffi_raw_development_intent(plan.intent),
        quality: ffi_raw_development_quality(plan.quality),
        dng_opcode_policy: ffi_dng_opcode_policy(plan.dng_opcode_policy),
        noise_reduction: ffi_raw_noise_reduction_intent(plan.noise_reduction),
        highlight_recovery: ffi_raw_highlight_recovery_intent(plan.highlight_recovery),
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
    })
}

fn raw_development_capabilities(
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

fn raw_development_plan_negotiation(
    negotiation: ffi::FfiRawDevelopmentPlanNegotiation,
) -> Result<RawDevelopmentPlanNegotiation, BridgeError> {
    Ok(RawDevelopmentPlanNegotiation {
        requested: raw_development_plan(negotiation.requested)?,
        effective: raw_development_plan(negotiation.effective)?,
        status: raw_development_plan_negotiation_status(negotiation.status)?,
        unresolved: negotiation.unresolved,
    })
}

fn preflight_photo_edit_development(
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

fn raw_development_receipt(
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

fn raw_pipeline_receipt(
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

/// Hard memory bound for the reusable float working proxy.
///
/// A square proxy at this edge consumes at most 192 MiB for interleaved RGB
/// float32. The intended UI values are 1600 and 2048.
pub const MAX_WARM_EDIT_PREVIEW_EDGE: u32 = 4_096;

/// Number of bins in every warm edit-preview display histogram.
pub const EDIT_PREVIEW_HISTOGRAM_BIN_COUNT: usize = 256;

/// Exact semantic contract for warm edit-preview analysis.
///
/// Histograms cover the complete uncompressed display-encoded sRGB RGB8 warm proxy
/// immediately before JPEG encoding. Clipping counts inspect the edited
/// processed-linear working-RGB samples before display clamping and use strict `< 0` and `> 1`
/// comparisons; exact zero and one are not clipped. HDR headroom bins instead use linear
/// Rec.709 luminance above `1.0` display white and retain the peak positive EV; they are an
/// output-readiness diagnostic, not sensor dynamic-range evidence.
pub const EDIT_PREVIEW_ANALYSIS_VERSION: &str = concat!(
    "shadow.edit-preview-analysis.v2:rgb8-before-jpeg:rec709-encoded-q16:",
    "pre-clamp-linear-strict-lt-gt-any-channel:linear-headroom-log2-v1"
);
pub const EDIT_PREVIEW_HDR_HEADROOM_BIN_COUNT: usize = 16;
pub const EDIT_PREVIEW_EXECUTION_RECEIPT_SCHEMA_VERSION: u32 = 1;
pub const EDIT_PREVIEW_EXECUTION_PLAN_CONTRACT_VERSION: u32 = 1;
pub const EDIT_PREVIEW_CPU_ADJUSTMENT_BACKEND_VERSION: u32 = 1;
pub const EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION: u32 = 1;
pub const EDIT_PREVIEW_CPU_DISPLAY_BACKEND_VERSION: u32 = 1;
pub const EDIT_PREVIEW_METAL_DISPLAY_BACKEND_VERSION: u32 = 1;
pub const DISPLAY_SRGB8_OUTPUT_CONTRACT_VERSION: u32 = 1;

/// Hard width and height bound for one full-resolution detail tile.
pub const MAX_EDIT_DETAIL_TILE_SIDE: u32 = 1_024;

/// Hard bound for the complete immutable u16 source retained by one detail session.
pub const MAX_EDIT_DETAIL_RETAINED_BYTES: u64 = 512 * 1_024 * 1_024;

/// Numeric v1 contract used by the non-curve adjustment operations.
pub const ADJUSTMENT_PARAMETER_SCHEMA_VERSION: u32 = 1;
/// Numeric v1 executor revision.
pub const ADJUSTMENT_IMPLEMENTATION_VERSION: u32 = 1;
/// Numeric parameter contract for the complete guided scene-linear Selective Tone filter.
pub const SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION: u32 = 1;
/// Numeric executor revision for the complete guided Selective Tone filter.
pub const SELECTIVE_TONE_IMPLEMENTATION_VERSION: u32 = 1;
/// Numeric contract for Shadow's sole Oklab-L perceptual curve.
pub const OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION: u32 = 1;
/// Hard bound for one linearized render plan crossing the language boundary.
pub const MAX_ADJUSTMENT_RENDER_NODES: usize = 256;
/// Hard bound for one immutable `.cube` document crossing the render bridge.
pub const MAX_LUT_DOCUMENT_BYTES: usize = 16 * 1_024 * 1_024;
/// Hard bound for diagnostic node identities crossing the language boundary.
pub const MAX_ADJUSTMENT_NODE_ID_BYTES: usize = 256;
/// Mirrors the CPU reference Tone Curve bound without exposing a C++ type.
pub const MAX_TONE_CURVE_POINTS: usize = 256;
/// Fixed hue anchors used by the first perceptual Color Mixer contract.
pub const COLOR_MIXER_BAND_COUNT: usize = 8;
pub const MAX_POINT_COLOR_RANGES: usize = 16;
/// Photoshop-compatible Selective Color has six chromatic target families
/// plus white, neutral, and black. Each target owns CMYK amounts.
pub const SELECTIVE_COLOR_TARGET_COUNT: usize = 9;
pub const SELECTIVE_COLOR_COMPONENT_COUNT: usize = 4;
pub const SELECTIVE_COLOR_VALUE_COUNT: usize =
    SELECTIVE_COLOR_TARGET_COUNT * SELECTIVE_COLOR_COMPONENT_COUNT;
pub const PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION: u32 = 1;
/// Fixed Color Warper mesh dimensions. The typed bridge deliberately mirrors
/// the native 5×5 Oklab a/b lattice rather than exposing a second UI-specific
/// mesh shape.
pub const OKLAB_COLOR_WARPER_GRID_SIDE: usize = 5;
pub const OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT: usize =
    OKLAB_COLOR_WARPER_GRID_SIDE * OKLAB_COLOR_WARPER_GRID_SIDE;
pub const OKLAB_COLOR_WARPER_MAXIMUM_OFFSET: f64 = 0.32;
pub const OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION: u32 = 1;
/// The visible Detail & Effects payload is one 37-value FFI record,
/// compiled into three ordered v1 internal passes.
pub const TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const TECHNICAL_DETAIL_IMPLEMENTATION_VERSION: u32 = 1;
pub const COLOR_GRADING_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const COLOR_GRADING_IMPLEMENTATION_VERSION: u32 = 1;
pub const FINISHING_EFFECTS_PARAMETER_SCHEMA_VERSION: u32 = 1;
pub const FINISHING_EFFECTS_IMPLEMENTATION_VERSION: u32 = 1;

/// One authored point in Shadow's perceptual tone-curve contract.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ToneCurvePoint {
    pub x: f64,
    pub y: f64,
}

/// A smooth curve for the Oklab L axis alone.  The a/b opponent axes are
/// retained exactly, so its normal use is tonal shaping without a hue or
/// chroma adjustment.
#[derive(Debug, Clone, PartialEq)]
pub struct OklabLightnessToneCurve {
    pub lightness: Vec<ToneCurvePoint>,
}

impl Default for OklabLightnessToneCurve {
    fn default() -> Self {
        Self {
            lightness: vec![
                ToneCurvePoint { x: 0.0, y: 0.0 },
                ToneCurvePoint { x: 1.0, y: 1.0 },
            ],
        }
    }
}

/// Guided local tonal zones in the processed linear-light RGB working space. Values are normalized
/// user intent in `[-1, 1]`; the executor owns the versioned EV weighting, mask, and strength.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SelectiveToneParameters {
    pub highlights: f64,
    pub shadows: f64,
    pub whites: f64,
    pub blacks: f64,
}

impl Default for SelectiveToneParameters {
    fn default() -> Self {
        Self {
            highlights: 0.0,
            shadows: 0.0,
            whites: 0.0,
            blacks: 0.0,
        }
    }
}

/// One soft, circular hue range layered on top of the fixed Color Mixer.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ColorRangeParameters {
    pub enabled: bool,
    pub center_hue_degrees: f64,
    pub width_degrees: f64,
    pub softness: f64,
    pub hue_shift_degrees: f64,
    pub saturation: f64,
    pub lightness: f64,
}

impl Default for ColorRangeParameters {
    fn default() -> Self {
        Self {
            enabled: false,
            center_hue_degrees: 0.0,
            width_degrees: 30.0,
            softness: 0.5,
            hue_shift_degrees: 0.0,
            saturation: 0.0,
            lightness: 0.0,
        }
    }
}

/// Perceptual color controls evaluated together so Oklab conversion happens
/// once per pixel instead of once per UI slider.
#[derive(Debug, Clone, PartialEq)]
pub struct PerceptualColorParameters {
    /// Global green↔red Oklab opponent balance, independent of RAW white
    /// balance and intentionally applied in the perceptual color operation.
    pub global_a_balance: f64,
    /// Global blue↔yellow Oklab opponent balance.
    pub global_b_balance: f64,
    pub vibrance: f64,
    pub hue_shifts: [f64; COLOR_MIXER_BAND_COUNT],
    pub saturation: [f64; COLOR_MIXER_BAND_COUNT],
    pub lightness: [f64; COLOR_MIXER_BAND_COUNT],
    pub color_range: ColorRangeParameters,
    pub additional_color_ranges: Vec<ColorRangeParameters>,
    /// `true` mirrors Photoshop's default Relative method: corrections scale
    /// existing CMYK ink. `false` is the Absolute method.
    pub selective_color_relative: bool,
    /// 0 retains Photoshop-like CMYK lightness behaviour; 1 restores the
    /// source Oklab L after the correction so the tool becomes a hue/chroma
    /// correction with perceptual exposure protection.
    pub selective_color_lightness_protection: f64,
    /// Nine target families × cyan, magenta, yellow, black, all in [-1, 1].
    pub selective_color_cmyk: [f64; SELECTIVE_COLOR_VALUE_COUNT],
}

impl Default for PerceptualColorParameters {
    fn default() -> Self {
        Self {
            global_a_balance: 0.0,
            global_b_balance: 0.0,
            vibrance: 0.0,
            hue_shifts: [0.0; COLOR_MIXER_BAND_COUNT],
            saturation: [0.0; COLOR_MIXER_BAND_COUNT],
            lightness: [0.0; COLOR_MIXER_BAND_COUNT],
            color_range: ColorRangeParameters::default(),
            additional_color_ranges: Vec::new(),
            selective_color_relative: true,
            selective_color_lightness_protection: 0.0,
            selective_color_cmyk: [0.0; SELECTIVE_COLOR_VALUE_COUNT],
        }
    }
}

/// One row-major target displacement in the immutable Oklab Color Warper
/// lattice. The source lattice positions are implicit and stable, which keeps
/// Recipes compact and makes a later mesh editor deterministic.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct OklabColorWarperControlPoint {
    pub a_offset: f64,
    pub b_offset: f64,
}

/// A fixed 5×5 Oklab a/b displacement lattice. Unlike the hue-keyed Color
/// Mixer and sampled Point Color ranges, this is one connected chroma field
/// that can be attached to a single masked Grade Node.
#[derive(Debug, Clone, PartialEq)]
pub struct OklabColorWarperParameters {
    pub control_points: [OklabColorWarperControlPoint; OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT],
    pub strength: f64,
}

impl Default for OklabColorWarperParameters {
    fn default() -> Self {
        Self {
            control_points: [OklabColorWarperControlPoint {
                a_offset: 0.0,
                b_offset: 0.0,
            }; OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT],
            strength: 1.0,
        }
    }
}

/// Spatial sharpening, detail, and finishing controls. `radius` is the
/// level-0 Gaussian sigma in pixels; the remaining values are normalized user
/// intent.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SharpenParameters {
    pub amount: f64,
    pub radius: f64,
    pub threshold: f64,
    pub masking: f64,
    /// Signed Oklab-L multi-scale detail controls. Clarity operates on the
    /// edge-protected middle residual; Texture operates on the finer residual.
    pub clarity: f64,
    pub texture: f64,
    /// Broad edge-aware Oklab-L contrast, intentionally distinct from the
    /// smaller-frequency Clarity and Texture bands.
    pub local_contrast: f64,
    /// Normalized native support scale for `local_contrast`.
    pub local_contrast_scale: f64,
    pub denoise_luminance: f64,
    pub denoise_detail: f64,
    pub denoise_color: f64,
    pub dehaze: f64,
    pub defringe_purple_amount: f64,
    pub defringe_purple_hue_low: f64,
    pub defringe_purple_hue_high: f64,
    pub defringe_green_amount: f64,
    pub defringe_green_hue_low: f64,
    pub defringe_green_hue_high: f64,
    pub shadows_hue: f64,
    pub shadows_saturation: f64,
    pub shadows_luminance: f64,
    pub midtones_hue: f64,
    pub midtones_saturation: f64,
    pub midtones_luminance: f64,
    pub highlights_hue: f64,
    pub highlights_saturation: f64,
    pub highlights_luminance: f64,
    pub grading_blending: f64,
    pub grading_balance: f64,
    pub grain_amount: f64,
    pub grain_size: f64,
    pub grain_roughness: f64,
    pub vignette_amount: f64,
    pub vignette_midpoint: f64,
    pub vignette_roundness: f64,
    pub vignette_feather: f64,
    pub vignette_highlights: f64,
}

impl Default for SharpenParameters {
    fn default() -> Self {
        Self {
            amount: 0.0,
            radius: 1.0,
            threshold: 0.0,
            masking: 0.0,
            clarity: 0.0,
            texture: 0.0,
            local_contrast: 0.0,
            local_contrast_scale: 0.5,
            denoise_luminance: 0.0,
            denoise_detail: 0.5,
            denoise_color: 0.0,
            dehaze: 0.0,
            defringe_purple_amount: 0.0,
            defringe_purple_hue_low: 270.0,
            defringe_purple_hue_high: 340.0,
            defringe_green_amount: 0.0,
            defringe_green_hue_low: 100.0,
            defringe_green_hue_high: 165.0,
            shadows_hue: 0.0,
            shadows_saturation: 0.0,
            shadows_luminance: 0.0,
            midtones_hue: 0.0,
            midtones_saturation: 0.0,
            midtones_luminance: 0.0,
            highlights_hue: 0.0,
            highlights_saturation: 0.0,
            highlights_luminance: 0.0,
            grading_blending: 0.5,
            grading_balance: 0.0,
            grain_amount: 0.0,
            grain_size: 0.5,
            grain_roughness: 0.5,
            vignette_amount: 0.0,
            vignette_midpoint: 0.5,
            vignette_roundness: 0.0,
            vignette_feather: 0.5,
            vignette_highlights: 0.0,
        }
    }
}

/// Typed pixel operation in execution order.
#[derive(Debug, Clone, PartialEq)]
pub enum AdjustmentRenderOperation {
    /// Opens one complete Grade Node layer in the flat bridge stream. The
    /// native executor snapshots its input, runs the enclosed adjustments,
    /// then mixes the result by this spatial mask before continuing. Keeping
    /// the boundary in the existing ordered node stream preserves the stable
    /// CXX request shape while adding true per-node-instance locality.
    LocalMaskLayerStart {
        opacity: f64,
        mask: Option<AdjustmentLocalMask>,
    },
    /// Closes the current local-mask layer opened by
    /// [`AdjustmentRenderOperation::LocalMaskLayerStart`].
    LocalMaskLayerEnd,
    Exposure {
        stops: f64,
    },
    Contrast {
        factor: f64,
        pivot: f64,
    },
    OklabLightnessToneCurve {
        curve: Box<OklabLightnessToneCurve>,
    },
    RgbWhiteBalance {
        temperature: f64,
        tint: f64,
    },
    Saturation {
        factor: f64,
    },
    SelectiveTone {
        parameters: SelectiveToneParameters,
    },
    PerceptualColor {
        parameters: Box<PerceptualColorParameters>,
    },
    OklabColorWarper {
        parameters: Box<OklabColorWarperParameters>,
    },
    Lut3D {
        document: Vec<u8>,
        intensity: f64,
    },
    Sharpen {
        pass: AdjustmentDetailEffectsPass,
        parameters: Box<SharpenParameters>,
    },
    /// Deterministic, non-generative repair of small defects. Each target is
    /// expressed in original-image coordinates and sampled from a surrounding
    /// ring so preview, detail tile, and export share the exact same intent.
    SpotHeal {
        targets: Vec<AdjustmentSpotHealTarget>,
        /// A drag is one retained, continuous swept-circle region. Keeping
        /// this alongside legacy targets makes old click-to-repair recipes
        /// decode and render exactly as before.
        strokes: Vec<AdjustmentRetouchStroke>,
    },
}

/// Explicit processing role for the shared Detail & Effects parameter bundle.
///
/// This is intentionally independent from contract versions: all three roles
/// use the current v1 contract, while this enum determines pipeline placement.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum AdjustmentDetailEffectsPass {
    TechnicalDetail,
    ColorGrading,
    FinishingEffects,
}

/// One bounded source-space target for [`AdjustmentRenderOperation::SpotHeal`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentSpotHealTarget {
    pub center_x: f64,
    pub center_y: f64,
    pub radius_level_zero_pixels: u16,
    /// 0 = heal, 1 = clone.
    pub mode: u8,
    pub source_offset_x_radii: f64,
    pub source_offset_y_radii: f64,
    pub feather: f64,
}

/// One normalized sampled point in a continuous repair/clone stroke.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentRetouchStrokePoint {
    pub x: f64,
    pub y: f64,
}

/// A bounded swept brush region for [`AdjustmentRenderOperation::SpotHeal`].
///
/// The source offset is measured in brush radii and stays fixed over the
/// whole stroke, so clone source and target keep the same shape.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentRetouchStroke {
    pub points: Vec<AdjustmentRetouchStrokePoint>,
    pub radius_level_zero_pixels: u16,
    /// 0 = heal, 1 = clone.
    pub mode: u8,
    pub source_offset_x_radii: f64,
    pub source_offset_y_radii: f64,
    pub feather: f64,
}

/// Lossless right-angle orientation for the photo-level final canvas.
///
/// This remains separate from adjustment operations because it changes the
/// output raster geometry rather than mutating samples in the current raster.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum AdjustmentQuarterTurn {
    Zero,
    Clockwise90,
    Clockwise180,
    Clockwise270,
}

/// A bounded photo-level crop/orientation request ready for native rendering.
///
/// Crop values are original-image edge coordinates. The native kernel and the
/// Rust detail scheduler both turn those normalized edges into exactly the
/// same pixel-aligned canvas before they schedule tiles.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentGeometry {
    pub crop_left: f64,
    pub crop_top: f64,
    pub crop_right: f64,
    pub crop_bottom: f64,
    pub quarter_turn: AdjustmentQuarterTurn,
    pub straighten_degrees: f64,
    pub flip_horizontal: bool,
    pub flip_vertical: bool,
}

impl Default for AdjustmentGeometry {
    fn default() -> Self {
        Self::identity()
    }
}

impl AdjustmentGeometry {
    #[must_use]
    pub const fn identity() -> Self {
        Self {
            crop_left: 0.0,
            crop_top: 0.0,
            crop_right: 1.0,
            crop_bottom: 1.0,
            quarter_turn: AdjustmentQuarterTurn::Zero,
            straighten_degrees: 0.0,
            flip_horizontal: false,
            flip_vertical: false,
        }
    }

    #[must_use]
    pub const fn is_identity(self) -> bool {
        self.crop_left == 0.0
            && self.crop_top == 0.0
            && self.crop_right == 1.0
            && self.crop_bottom == 1.0
            && matches!(self.quarter_turn, AdjustmentQuarterTurn::Zero)
            && self.straighten_degrees == 0.0
            && !self.flip_horizontal
            && !self.flip_vertical
    }

    fn validate(self) -> Result<(), BridgeError> {
        let values = [
            self.crop_left,
            self.crop_top,
            self.crop_right,
            self.crop_bottom,
        ];
        if values
            .iter()
            .any(|value| !value.is_finite() || !(0.0..=1.0).contains(value))
        {
            return Err(BridgeError::InvalidEditRequest(
                "photo geometry crop edges must be finite and normalized to 0..=1",
            ));
        }
        if self.crop_left >= self.crop_right || self.crop_top >= self.crop_bottom {
            return Err(BridgeError::InvalidEditRequest(
                "photo geometry crop must retain non-zero width and height",
            ));
        }
        if !self.straighten_degrees.is_finite()
            || !(-45.0..=45.0).contains(&self.straighten_degrees)
        {
            return Err(BridgeError::InvalidEditRequest(
                "photo geometry straighten angle must be in -45..=45 degrees",
            ));
        }
        Ok(())
    }

    /// Computes the exact pixel canvas used by geometry-aware detail tiles.
    pub fn output_dimensions(
        self,
        source: ImageDimensions,
    ) -> Result<ImageDimensions, BridgeError> {
        self.validate()?;
        let crop_axis = |source_extent: u32, lower: f64, upper: f64| {
            if source_extent == 0 {
                return Err(BridgeError::InvalidEditRequest(
                    "photo geometry requires non-zero source dimensions",
                ));
            }
            let extent = f64::from(source_extent);
            let lower = (lower * extent).floor().clamp(0.0, extent - 1.0) as u32;
            let upper = (upper * extent).ceil().clamp(1.0, extent) as u32;
            upper.checked_sub(lower).filter(|value| *value > 0).ok_or(
                BridgeError::InvalidEditRequest(
                    "photo geometry crop has no addressable source pixels",
                ),
            )
        };
        let width = crop_axis(source.width, self.crop_left, self.crop_right)?;
        let height = crop_axis(source.height, self.crop_top, self.crop_bottom)?;
        let (width, height) = match self.quarter_turn {
            AdjustmentQuarterTurn::Zero | AdjustmentQuarterTurn::Clockwise180 => (width, height),
            AdjustmentQuarterTurn::Clockwise90 | AdjustmentQuarterTurn::Clockwise270 => {
                (height, width)
            }
        };
        Ok(ImageDimensions { width, height })
    }
}

/// A normalized spatial mask ready for the native layer mixer.
#[derive(Debug, Clone, PartialEq)]
pub enum AdjustmentLocalMask {
    LinearGradient {
        start_x: f64,
        start_y: f64,
        end_x: f64,
        end_y: f64,
        invert: bool,
    },
    RadialGradient {
        center_x: f64,
        center_y: f64,
        radius_x: f64,
        radius_y: f64,
        feather: f64,
        invert: bool,
    },
    Brush {
        points: Vec<AdjustmentMaskBrushPoint>,
        radius: f64,
        feather: f64,
        invert: bool,
    },
}

/// One normalized freehand-mask sample prepared for the native mixer.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AdjustmentMaskBrushPoint {
    pub x: f64,
    pub y: f64,
    pub begins_stroke: bool,
}

/// A bounded, versioned node ready for the C++ reference executor.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentRenderNode {
    pub node_id: String,
    pub parameter_schema_version: u32,
    pub implementation_version: u32,
    pub enabled: bool,
    pub operation: AdjustmentRenderOperation,
}

/// A dependency-ordered execution plan.
///
/// Graph topology, stages, shared revisions, and immutable local-mask
/// definitions are compiled before this boundary. A mask-bearing recipe uses
/// explicit layer boundary records; ordinary recipes remain a compact flat
/// stream and keep their existing accelerated fast path. Neighborhood
/// footprint scheduling remains inside the C++ image kernel.
#[derive(Debug, Clone, PartialEq)]
pub struct AdjustmentRenderPlan {
    pub nodes: Vec<AdjustmentRenderNode>,
    /// Photo-local final-canvas geometry compiled independently from the
    /// original-coordinate adjustment stream.
    pub geometry: AdjustmentGeometry,
}

impl AdjustmentRenderPlan {
    /// Validates bounded bridge structure and finite parameter storage.
    /// Operation-specific numerical semantics remain authoritatively checked
    /// by the C++ executor before it touches pixels.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] for empty or oversized
    /// plans, duplicate/invalid node ids, unsupported versions, malformed Tone
    /// Curves, or non-finite values.
    pub fn validate(&self) -> Result<(), BridgeError> {
        self.geometry.validate()?;
        if self.nodes.is_empty() || self.nodes.len() > MAX_ADJUSTMENT_RENDER_NODES {
            return Err(BridgeError::InvalidEditRequest(
                "adjustment render plan must contain 1 through 256 nodes",
            ));
        }
        let mut node_ids = HashSet::with_capacity(self.nodes.len());
        for node in &self.nodes {
            if node.node_id.trim().is_empty() || node.node_id.len() > MAX_ADJUSTMENT_NODE_ID_BYTES {
                return Err(BridgeError::InvalidEditRequest(
                    "adjustment node id must contain 1 through 256 bytes",
                ));
            }
            if !node_ids.insert(node.node_id.as_str()) {
                return Err(BridgeError::InvalidEditRequest(
                    "adjustment render plan contains duplicate node ids",
                ));
            }
            let contract_matches = match &node.operation {
                AdjustmentRenderOperation::LocalMaskLayerStart { .. }
                | AdjustmentRenderOperation::LocalMaskLayerEnd => {
                    (
                        ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        ADJUSTMENT_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
                AdjustmentRenderOperation::OklabLightnessToneCurve { .. } => {
                    (
                        OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
                        OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
                AdjustmentRenderOperation::SelectiveTone { .. } => {
                    (
                        SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION,
                        SELECTIVE_TONE_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
                AdjustmentRenderOperation::PerceptualColor { .. } => {
                    (
                        PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION,
                        PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
                AdjustmentRenderOperation::OklabColorWarper { .. } => {
                    (
                        OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION,
                        OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
                AdjustmentRenderOperation::Sharpen { .. } => {
                    node.parameter_schema_version == TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION
                        && node.implementation_version == TECHNICAL_DETAIL_IMPLEMENTATION_VERSION
                }
                AdjustmentRenderOperation::SpotHeal { .. } => {
                    (
                        ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        ADJUSTMENT_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
                _ => {
                    (
                        ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        ADJUSTMENT_IMPLEMENTATION_VERSION,
                    ) == (node.parameter_schema_version, node.implementation_version)
                }
            };
            if !contract_matches {
                return Err(BridgeError::InvalidEditRequest(
                    "adjustment node uses an unsupported schema or implementation version",
                ));
            }
            validate_render_operation(&node.operation)?;
        }
        Ok(())
    }
}

#[allow(clippy::float_cmp)] // Tone Curve schemas require exact normalized endpoints.
fn validate_render_operation(operation: &AdjustmentRenderOperation) -> Result<(), BridgeError> {
    match operation {
        AdjustmentRenderOperation::LocalMaskLayerStart { opacity, mask } => {
            validate_finite_render_parameter(*opacity)?;
            if !(0.0..=1.0).contains(opacity) {
                return Err(BridgeError::InvalidEditRequest(
                    "local-mask layer opacity must be in 0..=1",
                ));
            }
            if let Some(mask) = mask {
                validate_adjustment_local_mask(mask)?;
            }
            Ok(())
        }
        AdjustmentRenderOperation::LocalMaskLayerEnd => Ok(()),
        AdjustmentRenderOperation::Exposure { stops } => {
            validate_finite_render_parameter(*stops)?;
            let gain = stops.exp2();
            if gain.is_finite() && gain > 0.0 {
                Ok(())
            } else {
                Err(BridgeError::InvalidEditRequest(
                    "exposure stops must produce a finite, positive gain",
                ))
            }
        }
        AdjustmentRenderOperation::Contrast { factor, pivot } => {
            validate_finite_render_parameter(*factor)?;
            validate_finite_render_parameter(*pivot)?;
            if *factor >= 0.0 && *pivot >= 0.0 {
                Ok(())
            } else {
                Err(BridgeError::InvalidEditRequest(
                    "contrast factor and pivot must be non-negative",
                ))
            }
        }
        AdjustmentRenderOperation::OklabLightnessToneCurve { curve } => {
            validate_tone_curve_points(&curve.lightness)
        }
        AdjustmentRenderOperation::RgbWhiteBalance { temperature, tint } => {
            for value in [temperature, tint] {
                validate_finite_render_parameter(*value)?;
                if !(-1.0..=1.0).contains(value) {
                    return Err(BridgeError::InvalidEditRequest(
                        "RGB white balance values must be in -1..=1",
                    ));
                }
            }
            Ok(())
        }
        AdjustmentRenderOperation::Saturation { factor } => {
            validate_finite_render_parameter(*factor)?;
            if *factor >= 0.0 {
                Ok(())
            } else {
                Err(BridgeError::InvalidEditRequest(
                    "saturation factor must be non-negative",
                ))
            }
        }
        AdjustmentRenderOperation::SelectiveTone { parameters } => {
            validate_selective_tone(*parameters)
        }
        AdjustmentRenderOperation::PerceptualColor { parameters } => {
            validate_perceptual_color(parameters)
        }
        AdjustmentRenderOperation::OklabColorWarper { parameters } => {
            validate_oklab_color_warper(parameters)
        }
        AdjustmentRenderOperation::Lut3D {
            document,
            intensity,
        } => {
            validate_finite_render_parameter(*intensity)?;
            if !(0.0..=1.0).contains(intensity) {
                return Err(BridgeError::InvalidEditRequest(
                    "3D LUT intensity must be in 0..=1",
                ));
            }
            if document.is_empty() {
                if *intensity == 0.0 {
                    Ok(())
                } else {
                    Err(BridgeError::InvalidEditRequest(
                        "an active 3D LUT requires a document",
                    ))
                }
            } else if document.len() <= MAX_LUT_DOCUMENT_BYTES {
                Ok(())
            } else {
                Err(BridgeError::InvalidEditRequest(
                    "3D LUT document exceeds 16 MiB",
                ))
            }
        }
        AdjustmentRenderOperation::Sharpen { parameters, .. } => validate_sharpen(parameters),
        AdjustmentRenderOperation::SpotHeal { targets, strokes } => {
            if (targets.is_empty() && strokes.is_empty()) || targets.len() > 64 {
                return Err(BridgeError::InvalidEditRequest(
                    "spot-heal must contain a repair target or continuous stroke",
                ));
            }
            for target in targets {
                for value in [
                    target.center_x,
                    target.center_y,
                    target.source_offset_x_radii,
                    target.source_offset_y_radii,
                    target.feather,
                ] {
                    validate_finite_render_parameter(value)?;
                }
                for value in [target.center_x, target.center_y, target.feather] {
                    if !(0.0..=1.0).contains(&value) {
                        return Err(BridgeError::InvalidEditRequest(
                            "spot-heal coordinates and feather must be normalized to 0..=1",
                        ));
                    }
                }
                if target.mode > 1
                    || !(-2.0..=2.0).contains(&target.source_offset_x_radii)
                    || !(-2.0..=2.0).contains(&target.source_offset_y_radii)
                    || !(1..=128).contains(&target.radius_level_zero_pixels)
                {
                    return Err(BridgeError::InvalidEditRequest(
                        "spot-heal mode, source offset, or radius is outside its supported range",
                    ));
                }
            }
            if strokes.len() > 64 {
                return Err(BridgeError::InvalidEditRequest(
                    "spot-heal supports at most 64 continuous strokes",
                ));
            }
            for stroke in strokes {
                if !(1..=512).contains(&stroke.points.len()) {
                    return Err(BridgeError::InvalidEditRequest(
                        "a continuous repair stroke must contain 1 through 512 points",
                    ));
                }
                for value in [
                    stroke.source_offset_x_radii,
                    stroke.source_offset_y_radii,
                    stroke.feather,
                ] {
                    validate_finite_render_parameter(value)?;
                }
                if stroke.mode > 1
                    || !(-2.0..=2.0).contains(&stroke.source_offset_x_radii)
                    || !(-2.0..=2.0).contains(&stroke.source_offset_y_radii)
                    || !(0.0..=1.0).contains(&stroke.feather)
                    || !(1..=128).contains(&stroke.radius_level_zero_pixels)
                {
                    return Err(BridgeError::InvalidEditRequest(
                        "continuous spot-heal behavior is outside the supported range",
                    ));
                }
                for point in &stroke.points {
                    validate_finite_render_parameter(point.x)?;
                    validate_finite_render_parameter(point.y)?;
                    if !(0.0..=1.0).contains(&point.x) || !(0.0..=1.0).contains(&point.y) {
                        return Err(BridgeError::InvalidEditRequest(
                            "continuous spot-heal points must be normalized to 0..=1",
                        ));
                    }
                }
            }
            Ok(())
        }
    }
}

fn validate_adjustment_local_mask(mask: &AdjustmentLocalMask) -> Result<(), BridgeError> {
    let unit = |value: f64| {
        validate_finite_render_parameter(value)?;
        if (0.0..=1.0).contains(&value) {
            Ok(())
        } else {
            Err(BridgeError::InvalidEditRequest(
                "local-mask coordinates must be normalized to 0..=1",
            ))
        }
    };
    match mask {
        AdjustmentLocalMask::LinearGradient {
            start_x,
            start_y,
            end_x,
            end_y,
            ..
        } => {
            for value in [*start_x, *start_y, *end_x, *end_y] {
                unit(value)?;
            }
            let dx = *end_x - *start_x;
            let dy = *end_y - *start_y;
            if dx.mul_add(dx, dy * dy) <= f64::EPSILON {
                return Err(BridgeError::InvalidEditRequest(
                    "local-mask linear gradient must have a non-zero direction",
                ));
            }
        }
        AdjustmentLocalMask::RadialGradient {
            center_x,
            center_y,
            radius_x,
            radius_y,
            feather,
            ..
        } => {
            for value in [*center_x, *center_y, *radius_x, *radius_y, *feather] {
                unit(value)?;
            }
            if *radius_x <= 0.0 || *radius_y <= 0.0 {
                return Err(BridgeError::InvalidEditRequest(
                    "local-mask radial gradient radii must both be greater than zero",
                ));
            }
        }
        AdjustmentLocalMask::Brush {
            points,
            radius,
            feather,
            ..
        } => {
            for value in [*radius, *feather] {
                unit(value)?;
            }
            if *radius <= 0.0 {
                return Err(BridgeError::InvalidEditRequest(
                    "local-mask brush radius must be greater than zero",
                ));
            }
            if points.len() > 4_096 {
                return Err(BridgeError::InvalidEditRequest(
                    "local-mask brush supports at most 4096 points",
                ));
            }
            for point in points {
                unit(point.x)?;
                unit(point.y)?;
            }
        }
    }
    Ok(())
}

#[allow(clippy::float_cmp)] // Tone Curve schemas require exact normalized endpoints.
fn validate_tone_curve_points(points: &[ToneCurvePoint]) -> Result<(), BridgeError> {
    if !(2..=MAX_TONE_CURVE_POINTS).contains(&points.len()) {
        return Err(BridgeError::InvalidEditRequest(
            "tone curve must contain 2 through 256 points",
        ));
    }
    if points.first().is_none_or(|point| point.x != 0.0)
        || points.last().is_none_or(|point| point.x != 1.0)
    {
        return Err(BridgeError::InvalidEditRequest(
            "tone curve x coordinates must start at zero and end at one",
        ));
    }
    let mut previous: Option<ToneCurvePoint> = None;
    for point in points {
        validate_finite_render_parameter(point.x)?;
        validate_finite_render_parameter(point.y)?;
        if let Some(previous_point) = previous {
            if point.x <= previous_point.x {
                return Err(BridgeError::InvalidEditRequest(
                    "tone curve x coordinates must be strictly increasing",
                ));
            }
            let slope = (point.y - previous_point.y) / (point.x - previous_point.x);
            if !slope.is_finite() {
                return Err(BridgeError::InvalidEditRequest(
                    "tone curve segment slopes must be finite",
                ));
            }
        }
        previous = Some(*point);
    }
    Ok(())
}

fn validate_finite_render_parameter(value: f64) -> Result<(), BridgeError> {
    if value.is_finite() {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "adjustment render parameters must be finite",
        ))
    }
}

fn validate_selective_tone(parameters: SelectiveToneParameters) -> Result<(), BridgeError> {
    for value in [
        parameters.highlights,
        parameters.shadows,
        parameters.whites,
        parameters.blacks,
    ] {
        validate_finite_render_parameter(value)?;
        if !(-1.0..=1.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "selective tone values must be in -1..=1",
            ));
        }
    }
    Ok(())
}

fn validate_oklab_color_warper(parameters: &OklabColorWarperParameters) -> Result<(), BridgeError> {
    validate_finite_render_parameter(parameters.strength)?;
    if !(0.0..=1.0).contains(&parameters.strength) {
        return Err(BridgeError::InvalidEditRequest(
            "Oklab Color Warper strength must be normalized to 0..=1",
        ));
    }
    for point in &parameters.control_points {
        for value in [point.a_offset, point.b_offset] {
            validate_finite_render_parameter(value)?;
            if value.abs() > OKLAB_COLOR_WARPER_MAXIMUM_OFFSET {
                return Err(BridgeError::InvalidEditRequest(
                    "Oklab Color Warper control offsets exceed the declared mesh extent",
                ));
            }
        }
    }
    Ok(())
}

fn validate_perceptual_color(parameters: &PerceptualColorParameters) -> Result<(), BridgeError> {
    for (value, name) in [
        (parameters.global_a_balance, "global Oklab a balance"),
        (parameters.global_b_balance, "global Oklab b balance"),
        (parameters.vibrance, "vibrance"),
    ] {
        validate_finite_render_parameter(value)?;
        if !(-1.0..=1.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(match name {
                "vibrance" => "vibrance must be in -1..=1",
                _ => "global Oklab balance must be in -1..=1",
            }));
        }
    }
    for values in [
        &parameters.hue_shifts,
        &parameters.saturation,
        &parameters.lightness,
    ] {
        for value in values {
            validate_finite_render_parameter(*value)?;
            if !(-1.0..=1.0).contains(value) {
                return Err(BridgeError::InvalidEditRequest(
                    "Color Mixer values must be in -1..=1",
                ));
            }
        }
    }
    if 1 + parameters.additional_color_ranges.len() > MAX_POINT_COLOR_RANGES {
        return Err(BridgeError::InvalidEditRequest(
            "Point Color supports at most 16 ordered ranges",
        ));
    }
    for range in
        std::iter::once(&parameters.color_range).chain(parameters.additional_color_ranges.iter())
    {
        for value in [
            range.center_hue_degrees,
            range.width_degrees,
            range.softness,
            range.hue_shift_degrees,
            range.saturation,
            range.lightness,
        ] {
            validate_finite_render_parameter(value)?;
        }
        if !(0.0..=360.0).contains(&range.center_hue_degrees)
            || !(1.0..=180.0).contains(&range.width_degrees)
            || !(0.0..=1.0).contains(&range.softness)
            || !(-180.0..=180.0).contains(&range.hue_shift_degrees)
            || !(-1.0..=1.0).contains(&range.saturation)
            || !(-1.0..=1.0).contains(&range.lightness)
        {
            return Err(BridgeError::InvalidEditRequest(
                "perceptual color range parameters are outside their contract",
            ));
        }
    }
    validate_finite_render_parameter(parameters.selective_color_lightness_protection)?;
    if !(0.0..=1.0).contains(&parameters.selective_color_lightness_protection) {
        return Err(BridgeError::InvalidEditRequest(
            "Selective Color lightness protection must be in 0..=1",
        ));
    }
    Ok(())
}

fn validate_sharpen(parameters: &SharpenParameters) -> Result<(), BridgeError> {
    for value in [
        parameters.amount,
        parameters.radius,
        parameters.threshold,
        parameters.masking,
        parameters.clarity,
        parameters.texture,
        parameters.local_contrast,
        parameters.local_contrast_scale,
        parameters.denoise_luminance,
        parameters.denoise_detail,
        parameters.denoise_color,
        parameters.defringe_purple_amount,
        parameters.defringe_green_amount,
        parameters.shadows_saturation,
        parameters.midtones_saturation,
        parameters.highlights_saturation,
        parameters.grading_blending,
        parameters.grain_amount,
        parameters.grain_size,
        parameters.grain_roughness,
        parameters.vignette_midpoint,
        parameters.vignette_feather,
        parameters.vignette_highlights,
    ] {
        validate_finite_render_parameter(value)?;
    }
    for value in [
        parameters.dehaze,
        parameters.clarity,
        parameters.texture,
        parameters.shadows_luminance,
        parameters.midtones_luminance,
        parameters.highlights_luminance,
        parameters.grading_balance,
        parameters.vignette_amount,
        parameters.vignette_roundness,
    ] {
        validate_finite_render_parameter(value)?;
        if !(-1.0..=1.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "signed Detail & Effects values must be in -1..=1",
            ));
        }
    }
    for value in [
        parameters.shadows_hue,
        parameters.midtones_hue,
        parameters.highlights_hue,
    ] {
        validate_finite_render_parameter(value)?;
        if !(0.0..=360.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "Color Grading hue values must be in 0..=360",
            ));
        }
    }
    for value in [
        parameters.defringe_purple_hue_low,
        parameters.defringe_purple_hue_high,
        parameters.defringe_green_hue_low,
        parameters.defringe_green_hue_high,
    ] {
        validate_finite_render_parameter(value)?;
        if !(0.0..=360.0).contains(&value) {
            return Err(BridgeError::InvalidEditRequest(
                "defringe hue values must be in 0..=360",
            ));
        }
    }
    if parameters.defringe_purple_hue_low + 10.0 > parameters.defringe_purple_hue_high
        || parameters.defringe_green_hue_low + 10.0 > parameters.defringe_green_hue_high
    {
        return Err(BridgeError::InvalidEditRequest(
            "defringe hue ranges must have at least a 10 degree span",
        ));
    }
    if !(0.0..=2.0).contains(&parameters.amount)
        || !(0.1..=5.0).contains(&parameters.radius)
        || !(0.0..=1.0).contains(&parameters.threshold)
        || !(0.0..=1.0).contains(&parameters.masking)
        || !(-1.0..=1.0).contains(&parameters.clarity)
        || !(-1.0..=1.0).contains(&parameters.texture)
        || !(-1.0..=1.0).contains(&parameters.local_contrast)
        || !(0.0..=1.0).contains(&parameters.local_contrast_scale)
        || [
            parameters.denoise_luminance,
            parameters.denoise_detail,
            parameters.denoise_color,
            parameters.defringe_purple_amount,
            parameters.defringe_green_amount,
            parameters.shadows_saturation,
            parameters.midtones_saturation,
            parameters.highlights_saturation,
            parameters.grading_blending,
            parameters.grain_amount,
            parameters.grain_size,
            parameters.grain_roughness,
            parameters.vignette_midpoint,
            parameters.vignette_feather,
            parameters.vignette_highlights,
        ]
        .into_iter()
        .any(|value| !(0.0..=1.0).contains(&value))
    {
        return Err(BridgeError::InvalidEditRequest(
            "sharpen parameters are outside their contract",
        ));
    }
    Ok(())
}

/// The first small, deterministic subset of Shadow's edit graph.
///
/// Execution order is exposure, contrast, processed-RGB white balance, then
/// saturation. Temperature/tint are creative D65 chromatic adaptation and are
/// deliberately not advertised as sensor-domain RAW white balance.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BasicEditParameters {
    pub exposure_stops: f64,
    pub contrast_factor: f64,
    pub white_balance_temperature: f64,
    pub white_balance_tint: f64,
    pub saturation_factor: f64,
}

impl Default for BasicEditParameters {
    fn default() -> Self {
        Self {
            exposure_stops: 0.0,
            contrast_factor: 1.0,
            white_balance_temperature: 0.0,
            white_balance_tint: 0.0,
            saturation_factor: 1.0,
        }
    }
}

impl BasicEditParameters {
    fn validate(self) -> Result<(), BridgeError> {
        validate_inclusive(
            self.exposure_stops,
            -16.0,
            16.0,
            "exposure_stops must be finite and in -16..=16",
        )?;
        validate_inclusive(
            self.contrast_factor,
            0.0,
            8.0,
            "contrast_factor must be finite and in 0..=8",
        )?;
        validate_inclusive(
            self.white_balance_temperature,
            -1.0,
            1.0,
            "white_balance_temperature must be finite and in -1..=1",
        )?;
        validate_inclusive(
            self.white_balance_tint,
            -1.0,
            1.0,
            "white_balance_tint must be finite and in -1..=1",
        )?;
        validate_inclusive(
            self.saturation_factor,
            0.0,
            8.0,
            "saturation_factor must be finite and in 0..=8",
        )
    }
}

/// Builds the compatibility four-node plan used by the current Precision
/// sliders. New renderer integrations should compile their persisted Recipe
/// directly instead of treating this fixed subset as the source of truth.
///
/// # Errors
///
/// Returns [`BridgeError::InvalidEditRequest`] when a basic parameter violates
/// its public range.
pub fn basic_adjustment_render_plan(
    edits: BasicEditParameters,
) -> Result<AdjustmentRenderPlan, BridgeError> {
    edits.validate()?;
    let node = |node_id: &str, operation| AdjustmentRenderNode {
        node_id: node_id.to_owned(),
        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
        enabled: true,
        operation,
    };
    let plan = AdjustmentRenderPlan {
        nodes: vec![
            node(
                "basic-rgb-white-balance",
                AdjustmentRenderOperation::RgbWhiteBalance {
                    temperature: edits.white_balance_temperature,
                    tint: edits.white_balance_tint,
                },
            ),
            node(
                "basic-exposure",
                AdjustmentRenderOperation::Exposure {
                    stops: edits.exposure_stops,
                },
            ),
            node(
                "basic-contrast",
                AdjustmentRenderOperation::Contrast {
                    factor: edits.contrast_factor,
                    pivot: 0.18,
                },
            ),
            node(
                "basic-saturation",
                AdjustmentRenderOperation::Saturation {
                    factor: edits.saturation_factor,
                },
            ),
        ],
        geometry: AdjustmentGeometry::identity(),
    };
    plan.validate()?;
    Ok(plan)
}

/// Parameters for a bounded, standard-JPEG edited preview.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct EditedProxyRequest {
    pub edits: BasicEditParameters,
    pub max_edge: u32,
    pub jpeg_quality: u8,
}

impl Default for EditedProxyRequest {
    fn default() -> Self {
        Self {
            edits: BasicEditParameters::default(),
            max_edge: 2_048,
            jpeg_quality: 95,
        }
    }
}

impl EditedProxyRequest {
    fn validate(self) -> Result<(), BridgeError> {
        self.edits.validate()?;
        validate_proxy_max_edge(self.max_edge)?;
        validate_jpeg_quality(self.jpeg_quality)
    }
}

fn validate_warm_edit_max_edge(max_edge: u32) -> Result<(), BridgeError> {
    if (1..=MAX_WARM_EDIT_PREVIEW_EDGE).contains(&max_edge) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "warm edit preview max_edge must be in 1..=4096",
        ))
    }
}

fn validate_jpeg_quality(jpeg_quality: u8) -> Result<(), BridgeError> {
    if (1..=100).contains(&jpeg_quality) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "jpeg_quality must be in 1..=100",
        ))
    }
}

fn validate_inclusive(
    value: f64,
    minimum: f64,
    maximum: f64,
    message: &'static str,
) -> Result<(), BridgeError> {
    if value.is_finite() && (minimum..=maximum).contains(&value) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(message))
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

/// A reusable, bounded processed linear-light RGB working proxy for interactive edits.
///
/// [`Self::open`] asks Shadow's source router for processed linear-light sRGB-primary RGB once.
/// The resulting C++ handle retains only an immutable, max-edge-bounded RGB
/// float buffer; it does not retain a decoder or borrow the input path. The
/// handle is both [`Send`] and [`Sync`], and concurrent [`Self::render`] calls
/// use independent edit and JPEG buffers.
pub struct LibRawEditPreviewSession {
    handle: cxx::UniquePtr<ffi::EditPreviewHandle>,
    dimensions: ImageDimensions,
    max_edge: u32,
    sensor_clipping_mask: SensorClippingMask,
    raw_development_receipt: RawDevelopmentReceipt,
    raw_pipeline_receipt: RawPipelineReceipt,
    optics_receipt: OpticsReceipt,
}

/// Per-preview-pixel source headroom information projected from unprocessed RAW samples.
///
/// Bit 0 denotes a CFA sample at its calibrated white level; bit 1 denotes a display cell whose
/// complete source region is at or below calibrated black. Both are source facts, not output
/// histogram thresholds, and no mask is fabricated for JPEG/HEIF or an unavailable provider.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct SensorClippingMask {
    pub available: bool,
    pub dimensions: ImageDimensions,
    pub samples: Vec<u8>,
    pub highlight_pixel_count: u64,
    pub shadow_pixel_count: u64,
}

impl SensorClippingMask {
    const HIGHLIGHT_BIT: u8 = 1 << 0;
    const SHADOW_BIT: u8 = 1 << 1;

    const fn unavailable() -> Self {
        Self {
            available: false,
            dimensions: ImageDimensions {
                width: 0,
                height: 0,
            },
            samples: Vec::new(),
            highlight_pixel_count: 0,
            shadow_pixel_count: 0,
        }
    }
}

/// Transient analysis of one complete warm-proxy edit render.
///
/// The four histograms are derived from uncompressed display-encoded sRGB RGB8 bytes
/// before JPEG encoding. Per-channel and any-channel clipping counts are
/// derived from the same render's processed-linear working-RGB samples before output clamping;
/// they are not sensor-domain exposure measurements.
#[derive(Debug, Clone, PartialEq)]
pub struct EditPreviewAnalysis {
    pub version: String,
    pub sample_dimensions: ImageDimensions,
    pub red: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub green: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub blue: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub luma: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT],
    pub below_zero_samples: [u64; 3],
    pub above_one_samples: [u64; 3],
    pub hdr_headroom_bins: [u64; EDIT_PREVIEW_HDR_HEADROOM_BIN_COUNT],
    pub hdr_headroom_pixels: u64,
    pub hdr_peak_headroom_ev: f64,
    pub pixel_count: u64,
    pub shadow_clipped_pixels: u64,
    pub highlight_clipped_pixels: u64,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq, Hash)]
pub enum EditPreviewBackend {
    Cpu,
    Metal,
}

/// Effective adjustment and display route for one completed warm preview.
///
/// `cache_identity` is canonical C++ output and is the only field callers should hash into a
/// durable key. Diagnostic fallback state remains available for inspection but is deliberately
/// absent from that identity.
#[derive(Debug, Clone, Eq, PartialEq, Hash)]
pub struct EditPreviewExecutionReceipt {
    pub schema_version: u32,
    pub cache_identity: String,
    pub adjustment_backend: EditPreviewBackend,
    pub adjustment_backend_version: u32,
    pub adjustment_execution_contract_version: u32,
    pub display_backend: EditPreviewBackend,
    pub display_backend_version: u32,
    pub display_output_contract_version: u32,
    pub fused_pipeline: bool,
    pub adjustment_fell_back: bool,
    pub display_fell_back: bool,
    pub diagnostic: Option<String>,
}

impl EditPreviewExecutionReceipt {
    #[must_use]
    pub const fn uses_current_schema(&self) -> bool {
        self.schema_version == EDIT_PREVIEW_EXECUTION_RECEIPT_SCHEMA_VERSION
    }
}

/// A JPEG preview and its generation-matched transient analysis.
#[derive(Debug, Clone, PartialEq)]
pub struct AnalyzedEditPreview {
    pub proxy: shadow_domain::ProxyPayload,
    pub analysis: EditPreviewAnalysis,
    pub execution: EditPreviewExecutionReceipt,
}

/// One exact rectangle in the processed full-resolution image coordinate space.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub struct DetailTileRect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

/// One bounded, unscaled full-resolution tile request.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub struct DetailTileRequest {
    pub rect: DetailTileRect,
}

impl DetailTileRequest {
    fn validate(self, full_dimensions: ImageDimensions) -> Result<(), BridgeError> {
        let rect = self.rect;
        if rect.width == 0
            || rect.height == 0
            || rect.width > MAX_EDIT_DETAIL_TILE_SIDE
            || rect.height > MAX_EDIT_DETAIL_TILE_SIDE
        {
            return Err(BridgeError::InvalidEditRequest(
                "detail tile width and height must be in 1..=1024",
            ));
        }
        if rect.x >= full_dimensions.width
            || rect.y >= full_dimensions.height
            || rect.width > full_dimensions.width - rect.x
            || rect.height > full_dimensions.height - rect.y
        {
            return Err(BridgeError::InvalidEditRequest(
                "detail tile rectangle must be fully inside the retained image",
            ));
        }
        Ok(())
    }
}

/// Packed display-encoded sRGB RGB8 bytes for one exact full-resolution rectangle.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RenderedDetailTile {
    pub rect: DetailTileRect,
    pub full_dimensions: ImageDimensions,
    pub row_stride_bytes: u32,
    pub bytes: Vec<u8>,
}

/// A reusable immutable full-resolution processed-linear u16 RGB source in sRGB primaries for 1:1 tiles.
///
/// Preparation performs one source-router reference render, retains no decoder, and fails when
/// either the metadata worst-case RGB allocation or the actual retained allocation exceeds
/// 512 MiB. Repeated tile renders convert and edit only the requested rectangle. The wrapper is
/// [`Send`] + [`Sync`], and concurrent renders own independent temporary buffers.
pub struct LibRawEditDetailSession {
    handle: cxx::UniquePtr<ffi::FullEditDetailHandle>,
    dimensions: ImageDimensions,
    retained_bytes: u64,
    raw_development_receipt: RawDevelopmentReceipt,
    raw_pipeline_receipt: RawPipelineReceipt,
    optics_receipt: OpticsReceipt,
}

/// Source-neutral name for an immutable interactive photo-editing session.
///
/// The legacy `LibRawEditPreviewSession` name remains available for source compatibility, while
/// the constructor now enters through Shadow's photo router. RAW receipts remain explicit and
/// absent for a raster source that did not perform RAW development.
pub type PhotoEditPreviewSession = LibRawEditPreviewSession;

/// One-shot cancellation shared by all clones of this handle.
///
/// Cancelling is idempotent: the first call returns `true`, while later calls return `false`.
/// A cancelled handle stays cancelled and is intentionally not reusable for a later render.
#[derive(Clone)]
pub struct EditPreviewCancellation {
    handle: cxx::SharedPtr<ffi::EditPreviewCancellationHandle>,
}

impl std::fmt::Debug for EditPreviewCancellation {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("EditPreviewCancellation")
            .finish_non_exhaustive()
    }
}

impl EditPreviewCancellation {
    /// Creates a new independent cancellation source.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::NullHandle`] if the native bridge cannot allocate its shared
    /// cancellation source.
    pub fn new() -> Result<Self, BridgeError> {
        let handle = ffi::new_edit_preview_cancellation()?;
        if handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        Ok(Self { handle })
    }

    /// Requests cancellation. Returns `true` only for the first successful request.
    pub fn cancel(&self) -> bool {
        self.handle
            .as_ref()
            .is_some_and(ffi::EditPreviewCancellationHandle::cancel)
    }
}

/// Terminal native preview outcome. Cancellation is control flow, never a decoder/backend error.
#[derive(Debug, Clone, Eq, PartialEq)]
pub enum CancellableEditPreview<T> {
    Completed(T),
    Cancelled,
}

/// Source-neutral name for an immutable full-resolution photo-detail session.
///
/// See [`PhotoEditPreviewSession`] for the compatibility and RAW-provenance contract.
pub type PhotoEditDetailSession = LibRawEditDetailSession;

impl std::fmt::Debug for LibRawEditDetailSession {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LibRawEditDetailSession")
            .field("dimensions", &self.dimensions)
            .field("retained_bytes", &self.retained_bytes)
            .field("raw_development_receipt", &self.raw_development_receipt)
            .field("raw_pipeline_receipt", &self.raw_pipeline_receipt)
            .field("optics_receipt", &self.optics_receipt)
            .finish_non_exhaustive()
    }
}

impl std::fmt::Debug for LibRawEditPreviewSession {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("LibRawEditPreviewSession")
            .field("dimensions", &self.dimensions)
            .field("max_edge", &self.max_edge)
            .field(
                "sensor_clipping_available",
                &self.sensor_clipping_mask.available,
            )
            .field("raw_development_receipt", &self.raw_development_receipt)
            .field("raw_pipeline_receipt", &self.raw_pipeline_receipt)
            .field("optics_receipt", &self.optics_receipt)
            .finish_non_exhaustive()
    }
}

impl LibRawEditPreviewSession {
    /// Opens a supported photo into a reusable processed linear-light float RGB proxy in sRGB
    /// primaries.
    ///
    /// `max_edge` must be in `1..=4096`; 1600 or 2048 are the intended UI
    /// values. The bound is checked before the input path is opened.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] before source I/O for an
    /// invalid bound, or a decoder error if preparation fails.
    pub fn open(path: &Path, max_edge: u32) -> Result<Self, BridgeError> {
        Self::open_with_optics(path, max_edge, &OpticsSettings::default())
    }

    /// Opens a reusable preview session with explicit input-stage optical correction settings.
    ///
    /// # Errors
    ///
    /// Returns an invalid-request, path, decoder, resource-limit, or bridge-output error when
    /// validation or preparation fails.
    pub fn open_with_optics(
        path: &Path,
        max_edge: u32,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        Self::open_with_raw_development_plan_and_optics(
            path,
            max_edge,
            RawDevelopmentPlan::preview(),
            optics,
        )
    }

    /// Opens a preview with an explicit RAW source-development request. The plan is validated
    /// before the source path is opened; `Preview` intent is required because the prepared
    /// session is a bounded interactive raster rather than a native-detail source.
    pub fn open_with_raw_development_plan(
        path: &Path,
        max_edge: u32,
        raw_development_plan: RawDevelopmentPlan,
    ) -> Result<Self, BridgeError> {
        Self::open_with_raw_development_plan_and_optics(
            path,
            max_edge,
            raw_development_plan,
            &OpticsSettings::default(),
        )
    }

    /// Opens a preview with explicit RAW source-development and optical-correction contracts.
    /// JPEG/HEIF sources retain their ordinary decoded-raster behavior; they never fabricate a
    /// RAW receipt merely because a caller supplied the canonical preview plan.
    pub fn open_with_raw_development_plan_and_optics(
        path: &Path,
        max_edge: u32,
        raw_development_plan: RawDevelopmentPlan,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        validate_warm_edit_max_edge(max_edge)?;
        raw_development_plan.validate()?;
        if raw_development_plan.intent != RawDevelopmentIntent::Preview {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "warm edit previews require preview RAW-development intent",
            ));
        }
        let mut decode_handle = open_photo(path)?;
        if decode_handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        {
            let handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
            preflight_photo_edit_development(handle, raw_development_plan)?;
        }
        decode_handle
            .pin_mut()
            .configure_optics(&ffi_optics_settings(optics))?;
        let decode_handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let handle = decode_handle.prepare_edit_preview_with_raw_development_plan(
            max_edge,
            &ffi_raw_development_plan(raw_development_plan),
        )?;
        let prepared = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let prepared_dimensions = dimensions(&prepared.dimensions());
        let prepared_max_edge = prepared.max_edge();
        // This is optional inspection data prepared alongside the immutable source raster. It
        // must never reopen or unpack a RAW file merely to drive a zebra overlay.
        let sensor_clipping_mask = match validate_sensor_clipping_mask(
            prepared.sensor_clipping_mask(),
            prepared_dimensions,
        ) {
            Ok(mask) => mask,
            Err(error) => {
                eprintln!("Shadow: ignoring invalid RAW clipping diagnostic: {error}");
                SensorClippingMask::unavailable()
            }
        };
        let raw_development_receipt = raw_development_receipt(prepared.raw_development_receipt()?)?;
        let raw_pipeline_receipt = raw_pipeline_receipt(prepared.raw_pipeline_receipt()?)?;
        let optics_receipt = optics_receipt(prepared.optics_receipt());

        Ok(Self {
            handle,
            dimensions: prepared_dimensions,
            max_edge: prepared_max_edge,
            sensor_clipping_mask,
            raw_development_receipt,
            raw_pipeline_receipt,
            optics_receipt,
        })
    }

    /// Returns the fixed pixel dimensions of every preview from this session.
    #[must_use]
    pub const fn dimensions(&self) -> ImageDimensions {
        self.dimensions
    }

    /// Returns the requested longest-edge bound used during preparation.
    #[must_use]
    pub const fn max_edge(&self) -> u32 {
        self.max_edge
    }

    /// Returns source-domain clipping information computed once while this immutable preview was
    /// prepared. Slider renders reuse this data and do not reopen or unpack the RAW file.
    #[must_use]
    pub const fn sensor_clipping_mask(&self) -> &SensorClippingMask {
        &self.sensor_clipping_mask
    }

    /// Returns immutable provenance for the exact RAW development, if any, retained by this
    /// preview.
    #[must_use]
    pub const fn raw_development_receipt(&self) -> &RawDevelopmentReceipt {
        &self.raw_development_receipt
    }

    /// Returns the typed host-side route and canonical cache identity that produced this preview.
    #[must_use]
    pub const fn raw_pipeline_receipt(&self) -> &RawPipelineReceipt {
        &self.raw_pipeline_receipt
    }

    #[must_use]
    pub const fn optics_receipt(&self) -> &OpticsReceipt {
        &self.optics_receipt
    }

    /// Re-runs only the fixed-order basic nodes and JPEG encoding.
    ///
    /// This method never reopens or decodes the source. Since the prepared working
    /// proxy is immutable, calls may run concurrently from worker threads.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] before entering C++ for
    /// invalid edit values or JPEG quality, and [`BridgeError::Decoder`] for
    /// edit or encoding failures.
    pub fn render(
        &self,
        edits: BasicEditParameters,
        jpeg_quality: u8,
    ) -> Result<shadow_domain::ProxyPayload, BridgeError> {
        let plan = basic_adjustment_render_plan(edits)?;
        self.render_plan(&plan, jpeg_quality)
    }

    /// Executes a dependency-ordered typed plan against the prepared proxy.
    /// This method never reopens or decodes the source.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] for an invalid plan or JPEG
    /// quality, and [`BridgeError::Decoder`] for authoritative C++ numeric or
    /// encoding failures.
    pub fn render_plan(
        &self,
        plan: &AdjustmentRenderPlan,
        jpeg_quality: u8,
    ) -> Result<shadow_domain::ProxyPayload, BridgeError> {
        plan.validate()?;
        validate_jpeg_quality(jpeg_quality)?;
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let request = ffi_render_request(plan, self.max_edge, jpeg_quality);
        let proxy = handle.render_adjustment_plan(&request)?;
        let proxy = proxy_payload(proxy);
        if proxy.dimensions != output_dimensions {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "geometry-aware preview dimensions do not match the rendered canvas",
            ));
        }
        Ok(proxy)
    }

    /// Executes a typed plan with cooperative native cancellation.
    ///
    /// A cancelled render returns [`CancellableEditPreview::Cancelled`] and never fabricates a
    /// backend failure, fallback receipt, histogram, or JPEG. The cancellation handle is
    /// one-shot; create a fresh handle for each independently cancellable render.
    pub fn render_plan_cancellable(
        &self,
        plan: &AdjustmentRenderPlan,
        jpeg_quality: u8,
        cancellation: &EditPreviewCancellation,
    ) -> Result<CancellableEditPreview<shadow_domain::ProxyPayload>, BridgeError> {
        plan.validate()?;
        validate_jpeg_quality(jpeg_quality)?;
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let cancellation = cancellation
            .handle
            .as_ref()
            .ok_or(BridgeError::NullHandle)?;
        let request = ffi_render_request(plan, self.max_edge, jpeg_quality);
        let rendered = handle.render_adjustment_plan_cancellable(&request, cancellation)?;
        if rendered.cancelled {
            return Ok(CancellableEditPreview::Cancelled);
        }
        let proxy = proxy_payload(rendered.proxy);
        if proxy.dimensions != output_dimensions {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "geometry-aware preview dimensions do not match the rendered canvas",
            ));
        }
        Ok(CancellableEditPreview::Completed(proxy))
    }

    /// Executes a typed plan and returns its JPEG plus generation-matched
    /// display histogram and pre-clamp clipping analysis.
    ///
    /// The analysis covers the complete prepared warm proxy, not the current
    /// viewport. It is computed from uncompressed pixels before JPEG encoding,
    /// so changing `jpeg_quality` cannot change its values.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] for an invalid plan or JPEG
    /// quality, [`BridgeError::Decoder`] for authoritative C++ failures, or
    /// [`BridgeError::InvalidEditPreviewOutput`] if any returned analysis field
    /// violates the versioned bridge contract.
    pub fn render_plan_with_analysis(
        &self,
        plan: &AdjustmentRenderPlan,
        jpeg_quality: u8,
    ) -> Result<AnalyzedEditPreview, BridgeError> {
        plan.validate()?;
        validate_jpeg_quality(jpeg_quality)?;
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let request = ffi_render_request(plan, self.max_edge, jpeg_quality);
        let analyzed = handle.render_adjustment_plan_with_analysis(&request)?;
        let proxy = proxy_payload(analyzed.proxy);
        validate_analyzed_edit_preview(
            proxy,
            analyzed.analysis,
            analyzed.execution,
            output_dimensions,
        )
    }

    /// Executes a typed plan with generation-matched analysis and cooperative cancellation.
    ///
    /// Cancellation before completion returns no partial pixels, analysis, or execution receipt.
    pub fn render_plan_with_analysis_cancellable(
        &self,
        plan: &AdjustmentRenderPlan,
        jpeg_quality: u8,
        cancellation: &EditPreviewCancellation,
    ) -> Result<CancellableEditPreview<AnalyzedEditPreview>, BridgeError> {
        plan.validate()?;
        validate_jpeg_quality(jpeg_quality)?;
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let cancellation = cancellation
            .handle
            .as_ref()
            .ok_or(BridgeError::NullHandle)?;
        let request = ffi_render_request(plan, self.max_edge, jpeg_quality);
        let rendered =
            handle.render_adjustment_plan_with_analysis_cancellable(&request, cancellation)?;
        if rendered.cancelled {
            return Ok(CancellableEditPreview::Cancelled);
        }
        let analyzed = rendered.preview;
        let proxy = proxy_payload(analyzed.proxy);
        let completed = validate_analyzed_edit_preview(
            proxy,
            analyzed.analysis,
            analyzed.execution,
            output_dimensions,
        )?;
        Ok(CancellableEditPreview::Completed(completed))
    }
}

impl LibRawEditDetailSession {
    /// Opens a supported photo into immutable full-resolution processed-linear u16 RGB pixels in
    /// sRGB primaries.
    ///
    /// Provider metadata is checked against the worst-case RGB retention limit before the
    /// reference render starts. The returned allocation is checked independently before it is
    /// retained by the session.
    ///
    /// # Errors
    ///
    /// Returns a path, decoder, resource-limit, or invalid bridge-output error. Sources whose
    /// worst-case or actual retained allocation exceeds 512 MiB fail closed.
    pub fn open(path: &Path) -> Result<Self, BridgeError> {
        Self::open_with_optics(path, &OpticsSettings::default())
    }

    /// Opens a full-resolution detail session with explicit input-stage optical settings.
    ///
    /// # Errors
    ///
    /// Returns a path, decoder, resource-limit, invalid-request, or bridge-output error when
    /// validation or preparation fails.
    pub fn open_with_optics(path: &Path, optics: &OpticsSettings) -> Result<Self, BridgeError> {
        Self::open_with_raw_development_plan_and_optics(path, RawDevelopmentPlan::detail(), optics)
    }

    /// Opens an immutable full-resolution session with an explicit RAW source-development plan.
    /// Detail and ExportImage intents are accepted; Preview is rejected so a warm half-size
    /// source can never enter a full-resolution pipeline.
    pub fn open_with_raw_development_plan(
        path: &Path,
        raw_development_plan: RawDevelopmentPlan,
    ) -> Result<Self, BridgeError> {
        Self::open_with_raw_development_plan_and_optics(
            path,
            raw_development_plan,
            &OpticsSettings::default(),
        )
    }

    /// Opens a native-detail session with explicit RAW source-development and optical settings.
    pub fn open_with_raw_development_plan_and_optics(
        path: &Path,
        raw_development_plan: RawDevelopmentPlan,
        optics: &OpticsSettings,
    ) -> Result<Self, BridgeError> {
        raw_development_plan.validate()?;
        if !matches!(
            raw_development_plan.intent,
            RawDevelopmentIntent::Detail | RawDevelopmentIntent::ExportImage
        ) {
            return Err(BridgeError::InvalidRawDevelopmentPlan(
                "full-resolution edit requires detail or export-image RAW-development intent",
            ));
        }
        let mut decode_handle = open_photo(path)?;
        if decode_handle.is_null() {
            return Err(BridgeError::NullHandle);
        }
        {
            let handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
            preflight_photo_edit_development(handle, raw_development_plan)?;
        }
        decode_handle
            .pin_mut()
            .configure_optics(&ffi_optics_settings(optics))?;
        let decode_handle = decode_handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let handle = decode_handle.prepare_edit_detail_with_raw_development_plan(
            &ffi_raw_development_plan(raw_development_plan),
        )?;
        let prepared = handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let prepared_dimensions = dimensions(&prepared.dimensions());
        let retained_bytes = prepared.retained_bytes();
        let raw_development_receipt = raw_development_receipt(prepared.raw_development_receipt()?)?;
        let raw_pipeline_receipt = raw_pipeline_receipt(prepared.raw_pipeline_receipt()?)?;
        let optics_receipt = optics_receipt(prepared.optics_receipt());
        if prepared_dimensions.width == 0 || prepared_dimensions.height == 0 {
            return Err(BridgeError::InvalidEditDetailOutput(
                "prepared dimensions must be non-zero",
            ));
        }
        if retained_bytes == 0 || retained_bytes > MAX_EDIT_DETAIL_RETAINED_BYTES {
            return Err(BridgeError::InvalidEditDetailOutput(
                "retained bytes must be in 1..=512 MiB",
            ));
        }
        Ok(Self {
            handle,
            dimensions: prepared_dimensions,
            retained_bytes,
            raw_development_receipt,
            raw_pipeline_receipt,
            optics_receipt,
        })
    }

    /// Returns the processed full-resolution image dimensions used by tile coordinates.
    #[must_use]
    pub const fn dimensions(&self) -> ImageDimensions {
        self.dimensions
    }

    /// Returns the actual immutable u16 allocation retained by this session.
    #[must_use]
    pub const fn retained_bytes(&self) -> u64 {
        self.retained_bytes
    }

    /// Returns immutable provenance for the exact full-resolution RAW development, if any,
    /// retained by this detail session.
    #[must_use]
    pub const fn raw_development_receipt(&self) -> &RawDevelopmentReceipt {
        &self.raw_development_receipt
    }

    /// Returns the typed host-side route and canonical cache identity for this retained source.
    #[must_use]
    pub const fn raw_pipeline_receipt(&self) -> &RawPipelineReceipt {
        &self.raw_pipeline_receipt
    }

    #[must_use]
    pub const fn optics_receipt(&self) -> &OpticsReceipt {
        &self.optics_receipt
    }

    /// Executes a dependency-ordered typed plan against one exact full-resolution rectangle.
    ///
    /// Plan and rectangle shape/bounds are rejected in Rust before entering C++. The C++ kernel
    /// validates them again, normalizes only the processed-linear crop to float, executes the
    /// typed nodes, expands neighborhood footprints inside the kernel, and
    /// returns the requested core as tightly packed display-encoded sRGB RGB8
    /// without compression or scaling.
    /// No source I/O occurs during this method.
    ///
    /// # Errors
    ///
    /// Returns [`BridgeError::InvalidEditRequest`] for an invalid plan or rectangle,
    /// [`BridgeError::Decoder`] for authoritative kernel failures, or
    /// [`BridgeError::InvalidEditDetailOutput`] if bridge output violates its contract.
    pub fn render_plan_tile(
        &self,
        plan: &AdjustmentRenderPlan,
        request: DetailTileRequest,
    ) -> Result<RenderedDetailTile, BridgeError> {
        plan.validate()?;
        let output_dimensions = plan.geometry.output_dimensions(self.dimensions)?;
        request.validate(output_dimensions)?;
        let handle = self.handle.as_ref().ok_or(BridgeError::NullHandle)?;
        let rendered =
            handle.render_adjustment_plan_tile(&ffi_detail_tile_request(plan, request))?;
        let rect = detail_tile_rect(rendered.rect);
        let full_dimensions = dimensions(&rendered.full_dimensions);
        if rect != request.rect || full_dimensions != output_dimensions {
            return Err(BridgeError::InvalidEditDetailOutput(
                "returned identity does not match the requested tile and prepared source",
            ));
        }
        let expected_stride =
            rect.width
                .checked_mul(3)
                .ok_or(BridgeError::InvalidEditDetailOutput(
                    "RGB8 row stride overflows",
                ))?;
        if rendered.row_stride_bytes != expected_stride {
            return Err(BridgeError::InvalidEditDetailOutput(
                "RGB8 row stride must equal width times three",
            ));
        }
        let expected_len = usize::try_from(expected_stride)
            .ok()
            .and_then(|stride| {
                usize::try_from(rect.height)
                    .ok()
                    .and_then(|height| stride.checked_mul(height))
            })
            .ok_or(BridgeError::InvalidEditDetailOutput(
                "RGB8 byte length overflows addressable memory",
            ))?;
        if rendered.bytes.len() != expected_len {
            return Err(BridgeError::InvalidEditDetailOutput(
                "RGB8 byte length must equal row stride times height",
            ));
        }
        Ok(RenderedDetailTile {
            rect,
            full_dimensions,
            row_stride_bytes: rendered.row_stride_bytes,
            bytes: rendered.bytes,
        })
    }
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

fn validate_proxy_max_edge(max_edge: u32) -> Result<(), BridgeError> {
    if (1..=16_384).contains(&max_edge) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "max_edge must be in 1..=16384",
        ))
    }
}

fn ffi_render_request(
    plan: &AdjustmentRenderPlan,
    max_edge: u32,
    jpeg_quality: u8,
) -> ffi::FfiAdjustmentRenderRequest {
    ffi::FfiAdjustmentRenderRequest {
        nodes: plan.nodes.iter().map(ffi_render_node).collect(),
        geometry: ffi_photo_geometry(plan.geometry),
        max_edge,
        jpeg_quality,
    }
}

fn ffi_detail_tile_request(
    plan: &AdjustmentRenderPlan,
    request: DetailTileRequest,
) -> ffi::FfiAdjustmentDetailTileRequest {
    ffi::FfiAdjustmentDetailTileRequest {
        nodes: plan.nodes.iter().map(ffi_render_node).collect(),
        geometry: ffi_photo_geometry(plan.geometry),
        rect: ffi_detail_tile_rect(request.rect),
    }
}

const fn ffi_photo_geometry(geometry: AdjustmentGeometry) -> ffi::FfiPhotoGeometry {
    ffi::FfiPhotoGeometry {
        crop_left: geometry.crop_left,
        crop_top: geometry.crop_top,
        crop_right: geometry.crop_right,
        crop_bottom: geometry.crop_bottom,
        quarter_turn: match geometry.quarter_turn {
            AdjustmentQuarterTurn::Zero => 0,
            AdjustmentQuarterTurn::Clockwise90 => 1,
            AdjustmentQuarterTurn::Clockwise180 => 2,
            AdjustmentQuarterTurn::Clockwise270 => 3,
        },
        straighten_degrees: geometry.straighten_degrees,
        flip_horizontal: geometry.flip_horizontal,
        flip_vertical: geometry.flip_vertical,
    }
}

const fn ffi_detail_tile_rect(rect: DetailTileRect) -> ffi::FfiDetailTileRect {
    ffi::FfiDetailTileRect {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
    }
}

const fn detail_tile_rect(rect: ffi::FfiDetailTileRect) -> DetailTileRect {
    DetailTileRect {
        x: rect.x,
        y: rect.y,
        width: rect.width,
        height: rect.height,
    }
}

// The flat CXX wire record is intentionally assembled in one auditable operation.
#[allow(clippy::too_many_lines)]
fn ffi_render_node(node: &AdjustmentRenderNode) -> ffi::FfiAdjustmentNode {
    let (operation, parameters, parameter_group_lengths, payload) = match &node.operation {
        AdjustmentRenderOperation::LocalMaskLayerStart { opacity, mask } => {
            let (kind, x0, y0, x1, y1, radius_x, radius_y, feather, invert, brush_points) =
                match mask {
                    None => (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, Vec::new()),
                    Some(AdjustmentLocalMask::LinearGradient {
                        start_x,
                        start_y,
                        end_x,
                        end_y,
                        invert,
                    }) => (
                        1.0,
                        *start_x,
                        *start_y,
                        *end_x,
                        *end_y,
                        0.0,
                        0.0,
                        0.0,
                        if *invert { 1.0 } else { 0.0 },
                        Vec::new(),
                    ),
                    Some(AdjustmentLocalMask::RadialGradient {
                        center_x,
                        center_y,
                        radius_x,
                        radius_y,
                        feather,
                        invert,
                    }) => (
                        2.0,
                        *center_x,
                        *center_y,
                        0.0,
                        0.0,
                        *radius_x,
                        *radius_y,
                        *feather,
                        if *invert { 1.0 } else { 0.0 },
                        Vec::new(),
                    ),
                    Some(AdjustmentLocalMask::Brush {
                        points,
                        radius,
                        feather,
                        invert,
                    }) => (
                        3.0,
                        0.0,
                        0.0,
                        0.0,
                        0.0,
                        *radius,
                        0.0,
                        *feather,
                        if *invert { 1.0 } else { 0.0 },
                        points
                            .iter()
                            .flat_map(|point| {
                                [
                                    point.x,
                                    point.y,
                                    if point.begins_stroke { 1.0 } else { 0.0 },
                                ]
                            })
                            .collect(),
                    ),
                };
            let point_count = u32::try_from(brush_points.len() / 3)
                .expect("validated brush point count fits in u32");
            let mut parameters = vec![
                *opacity, kind, x0, y0, x1, y1, radius_x, radius_y, feather, invert,
            ];
            parameters.extend(brush_points);
            (
                ffi::FfiAdjustmentOperation::LocalMaskLayerStart,
                parameters,
                if kind == 3.0 {
                    vec![point_count]
                } else {
                    vec![]
                },
                vec![],
            )
        }
        AdjustmentRenderOperation::LocalMaskLayerEnd => (
            ffi::FfiAdjustmentOperation::LocalMaskLayerEnd,
            vec![],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::Exposure { stops } => (
            ffi::FfiAdjustmentOperation::Exposure,
            vec![*stops],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::Contrast { factor, pivot } => (
            ffi::FfiAdjustmentOperation::Contrast,
            vec![*factor, *pivot],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::OklabLightnessToneCurve { curve } => (
            ffi::FfiAdjustmentOperation::OklabLightnessToneCurve,
            curve
                .lightness
                .iter()
                .flat_map(|point| [point.x, point.y])
                .collect(),
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::RgbWhiteBalance { temperature, tint } => (
            ffi::FfiAdjustmentOperation::RgbWhiteBalance,
            vec![*temperature, *tint],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::Saturation { factor } => (
            ffi::FfiAdjustmentOperation::Saturation,
            vec![*factor],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::SelectiveTone { parameters } => (
            ffi::FfiAdjustmentOperation::SelectiveTone,
            vec![
                parameters.highlights,
                parameters.shadows,
                parameters.whites,
                parameters.blacks,
            ],
            vec![],
            vec![],
        ),
        AdjustmentRenderOperation::PerceptualColor { parameters } => {
            let mut flattened =
                Vec::with_capacity(72 + parameters.additional_color_ranges.len() * 7);
            flattened.push(parameters.vibrance);
            flattened.extend(parameters.hue_shifts);
            flattened.extend(parameters.saturation);
            flattened.extend(parameters.lightness);
            flattened.extend([
                if parameters.color_range.enabled {
                    1.0
                } else {
                    0.0
                },
                parameters.color_range.center_hue_degrees,
                parameters.color_range.width_degrees,
                parameters.color_range.softness,
                parameters.color_range.hue_shift_degrees,
                parameters.color_range.saturation,
                parameters.color_range.lightness,
            ]);
            flattened.push(if parameters.selective_color_relative {
                1.0
            } else {
                0.0
            });
            flattened.push(parameters.selective_color_lightness_protection);
            flattened.extend(parameters.selective_color_cmyk);
            // Preserve the original v1 Color Mixer / Point Color order and
            // add global opponent controls before the variable range tail.
            flattened.extend([parameters.global_a_balance, parameters.global_b_balance]);
            for range in &parameters.additional_color_ranges {
                flattened.extend([
                    if range.enabled { 1.0 } else { 0.0 },
                    range.center_hue_degrees,
                    range.width_degrees,
                    range.softness,
                    range.hue_shift_degrees,
                    range.saturation,
                    range.lightness,
                ]);
            }
            (
                ffi::FfiAdjustmentOperation::PerceptualColor,
                flattened,
                vec![
                    u32::try_from(parameters.additional_color_ranges.len())
                        .expect("validated Point Color range count fits u32"),
                ],
                vec![],
            )
        }
        AdjustmentRenderOperation::OklabColorWarper { parameters } => {
            // The wire order is strength followed by row-major `(a, b)` pairs.
            // It is deliberately fixed-size: a Recipe and the native lattice
            // can never disagree about topology.
            let mut flattened = Vec::with_capacity(1 + OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT * 2);
            flattened.push(parameters.strength);
            for point in &parameters.control_points {
                flattened.extend([point.a_offset, point.b_offset]);
            }
            (
                ffi::FfiAdjustmentOperation::OklabColorWarper,
                flattened,
                vec![],
                vec![],
            )
        }
        AdjustmentRenderOperation::Lut3D {
            document,
            intensity,
        } => (
            ffi::FfiAdjustmentOperation::Lut3D,
            vec![*intensity],
            vec![],
            document.clone(),
        ),
        AdjustmentRenderOperation::Sharpen { parameters, .. } => {
            let mut flattened = vec![
                parameters.amount,
                parameters.radius,
                parameters.threshold,
                parameters.masking,
            ];
            flattened.extend([
                parameters.clarity,
                parameters.texture,
                parameters.local_contrast,
                parameters.local_contrast_scale,
                parameters.denoise_luminance,
                parameters.denoise_detail,
                parameters.denoise_color,
                parameters.dehaze,
                parameters.defringe_purple_amount,
                parameters.defringe_purple_hue_low,
                parameters.defringe_purple_hue_high,
                parameters.defringe_green_amount,
                parameters.defringe_green_hue_low,
                parameters.defringe_green_hue_high,
                parameters.shadows_hue,
                parameters.shadows_saturation,
                parameters.shadows_luminance,
                parameters.midtones_hue,
                parameters.midtones_saturation,
                parameters.midtones_luminance,
                parameters.highlights_hue,
                parameters.highlights_saturation,
                parameters.highlights_luminance,
                parameters.grading_blending,
                parameters.grading_balance,
                parameters.grain_amount,
                parameters.grain_size,
                parameters.grain_roughness,
                parameters.vignette_amount,
                parameters.vignette_midpoint,
                parameters.vignette_roundness,
                parameters.vignette_feather,
                parameters.vignette_highlights,
            ]);
            (
                ffi::FfiAdjustmentOperation::Sharpen,
                flattened,
                vec![],
                vec![],
            )
        }
        AdjustmentRenderOperation::SpotHeal { targets, strokes } => {
            let stroke_parameters = strokes
                .iter()
                .map(|stroke| 5 + stroke.points.len() * 2)
                .sum::<usize>();
            let mut flattened = Vec::with_capacity(targets.len() * 7 + stroke_parameters);
            let mut parameter_group_lengths = vec![
                u32::try_from(targets.len()).expect("validated spot-heal target count fits u32"),
                u32::try_from(strokes.len()).expect("validated continuous stroke count fits u32"),
            ];
            for target in targets {
                flattened.extend([
                    target.center_x,
                    target.center_y,
                    f64::from(target.radius_level_zero_pixels),
                    f64::from(target.mode),
                    target.source_offset_x_radii,
                    target.source_offset_y_radii,
                    target.feather,
                ]);
            }
            for stroke in strokes {
                parameter_group_lengths.push(
                    u32::try_from(stroke.points.len())
                        .expect("validated continuous stroke point count fits u32"),
                );
                flattened.extend([
                    f64::from(stroke.radius_level_zero_pixels),
                    f64::from(stroke.mode),
                    stroke.source_offset_x_radii,
                    stroke.source_offset_y_radii,
                    stroke.feather,
                ]);
                for point in &stroke.points {
                    flattened.extend([point.x, point.y]);
                }
            }
            (
                ffi::FfiAdjustmentOperation::SpotHeal,
                flattened,
                parameter_group_lengths,
                vec![],
            )
        }
    };
    let detail_effects_pass = match &node.operation {
        AdjustmentRenderOperation::Sharpen {
            pass: AdjustmentDetailEffectsPass::TechnicalDetail,
            ..
        } => ffi::FfiDetailEffectsPass::TechnicalDetail,
        AdjustmentRenderOperation::Sharpen {
            pass: AdjustmentDetailEffectsPass::ColorGrading,
            ..
        } => ffi::FfiDetailEffectsPass::ColorGrading,
        AdjustmentRenderOperation::Sharpen {
            pass: AdjustmentDetailEffectsPass::FinishingEffects,
            ..
        } => ffi::FfiDetailEffectsPass::FinishingEffects,
        _ => ffi::FfiDetailEffectsPass::TechnicalDetail,
    };
    ffi::FfiAdjustmentNode {
        node_id: node.node_id.clone(),
        operation,
        detail_effects_pass,
        parameter_schema_version: node.parameter_schema_version,
        implementation_version: node.implementation_version,
        enabled: node.enabled,
        parameters,
        payload,
        parameter_group_lengths,
    }
}

fn proxy_payload(proxy: ffi::FfiEncodedProxy) -> shadow_domain::ProxyPayload {
    shadow_domain::ProxyPayload {
        dimensions: dimensions(&proxy.dimensions),
        codec: preview_codec(proxy.format),
        bits_per_channel: proxy.bits_per_channel,
        channels: proxy.channels,
        bytes: proxy.bytes,
    }
}

fn validate_edit_preview_proxy(
    proxy: &shadow_domain::ProxyPayload,
    expected_dimensions: ImageDimensions,
) -> Result<(), BridgeError> {
    if proxy.dimensions != expected_dimensions
        || proxy.dimensions.width == 0
        || proxy.dimensions.height == 0
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "proxy dimensions must match the prepared warm session",
        ));
    }
    if proxy.codec != PreviewCodec::Jpeg
        || proxy.bits_per_channel != 8
        || proxy.channels != 3
        || !proxy.bytes.starts_with(&[0xff, 0xd8])
        || !proxy.bytes.ends_with(&[0xff, 0xd9])
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "proxy must be a non-empty standard 8-bit three-channel JPEG",
        ));
    }
    Ok(())
}

fn validate_analyzed_edit_preview(
    proxy: shadow_domain::ProxyPayload,
    analysis: ffi::FfiEditPreviewAnalysis,
    execution: ffi::FfiEditPreviewExecutionReceipt,
    expected_dimensions: ImageDimensions,
) -> Result<AnalyzedEditPreview, BridgeError> {
    validate_edit_preview_proxy(&proxy, expected_dimensions)?;
    let analysis = validate_edit_preview_analysis(analysis, expected_dimensions)?;
    let execution = edit_preview_execution_receipt(execution)?;
    Ok(AnalyzedEditPreview {
        proxy,
        analysis,
        execution,
    })
}

fn edit_preview_backend(
    backend: ffi::FfiEditPreviewBackend,
) -> Result<EditPreviewBackend, BridgeError> {
    match backend {
        ffi::FfiEditPreviewBackend::Cpu => Ok(EditPreviewBackend::Cpu),
        ffi::FfiEditPreviewBackend::Metal => Ok(EditPreviewBackend::Metal),
        _ => Err(BridgeError::InvalidEditPreviewOutput(
            "edit-preview receipt contains an unsupported backend",
        )),
    }
}

fn edit_preview_execution_receipt(
    receipt: ffi::FfiEditPreviewExecutionReceipt,
) -> Result<EditPreviewExecutionReceipt, BridgeError> {
    if receipt.schema_version != EDIT_PREVIEW_EXECUTION_RECEIPT_SCHEMA_VERSION {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "edit-preview receipt uses an unsupported schema",
        ));
    }
    if receipt.cache_identity.is_empty() || receipt.cache_identity.len() > 512 {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "edit-preview receipt cache identity is empty or unbounded",
        ));
    }
    let adjustment_backend = edit_preview_backend(receipt.adjustment_backend)?;
    let display_backend = edit_preview_backend(receipt.display_backend)?;
    let adjustment_backend_version_is_current = match adjustment_backend {
        EditPreviewBackend::Cpu => {
            receipt.adjustment_backend_version == EDIT_PREVIEW_CPU_ADJUSTMENT_BACKEND_VERSION
        }
        EditPreviewBackend::Metal => {
            receipt.adjustment_backend_version == EDIT_PREVIEW_METAL_ADJUSTMENT_BACKEND_VERSION
        }
    };
    let display_backend_version_is_current = match display_backend {
        EditPreviewBackend::Cpu => {
            receipt.display_backend_version == EDIT_PREVIEW_CPU_DISPLAY_BACKEND_VERSION
        }
        EditPreviewBackend::Metal => {
            receipt.display_backend_version == EDIT_PREVIEW_METAL_DISPLAY_BACKEND_VERSION
        }
    };
    if !adjustment_backend_version_is_current
        || !display_backend_version_is_current
        || (receipt.fused_pipeline && display_backend != EditPreviewBackend::Metal)
        || (receipt.fused_pipeline && (receipt.adjustment_fell_back || receipt.display_fell_back))
        || receipt.adjustment_execution_contract_version
            != EDIT_PREVIEW_EXECUTION_PLAN_CONTRACT_VERSION
        || receipt.display_output_contract_version != DISPLAY_SRGB8_OUTPUT_CONTRACT_VERSION
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "edit-preview receipt contains unsupported implementation contracts",
        ));
    }
    let any_fallback = receipt.adjustment_fell_back || receipt.display_fell_back;
    if (receipt.adjustment_fell_back && adjustment_backend != EditPreviewBackend::Cpu)
        || (receipt.display_fell_back && display_backend != EditPreviewBackend::Cpu)
        || any_fallback != !receipt.diagnostic.is_empty()
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "edit-preview receipt contains incoherent fallback provenance",
        ));
    }
    Ok(EditPreviewExecutionReceipt {
        schema_version: receipt.schema_version,
        cache_identity: receipt.cache_identity,
        adjustment_backend,
        adjustment_backend_version: receipt.adjustment_backend_version,
        adjustment_execution_contract_version: receipt.adjustment_execution_contract_version,
        display_backend,
        display_backend_version: receipt.display_backend_version,
        display_output_contract_version: receipt.display_output_contract_version,
        fused_pipeline: receipt.fused_pipeline,
        adjustment_fell_back: receipt.adjustment_fell_back,
        display_fell_back: receipt.display_fell_back,
        diagnostic: (!receipt.diagnostic.is_empty()).then_some(receipt.diagnostic),
    })
}

fn validate_sensor_clipping_mask(
    mask: ffi::FfiSensorClippingMask,
    expected_dimensions: ImageDimensions,
) -> Result<SensorClippingMask, BridgeError> {
    if !mask.available {
        if mask.dimensions.width != 0
            || mask.dimensions.height != 0
            || !mask.samples.is_empty()
            || mask.highlight_pixel_count != 0
            || mask.shadow_pixel_count != 0
        {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "unavailable sensor clipping mask must not contain data",
            ));
        }
        return Ok(SensorClippingMask::unavailable());
    }

    let dimensions = dimensions(&mask.dimensions);
    if dimensions != expected_dimensions {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "sensor clipping dimensions must match the prepared preview",
        ));
    }
    let expected_len = usize::try_from(dimensions.pixel_count()).map_err(|_| {
        BridgeError::InvalidEditPreviewOutput("sensor clipping mask is too large for this host")
    })?;
    if mask.samples.len() != expected_len {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "sensor clipping mask byte count must equal preview pixel count",
        ));
    }

    let mut highlights = 0_u64;
    let mut shadows = 0_u64;
    for sample in &mask.samples {
        if *sample & !(SensorClippingMask::HIGHLIGHT_BIT | SensorClippingMask::SHADOW_BIT) != 0 {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "sensor clipping mask contains unsupported bits",
            ));
        }
        highlights += u64::from(*sample & SensorClippingMask::HIGHLIGHT_BIT != 0);
        shadows += u64::from(*sample & SensorClippingMask::SHADOW_BIT != 0);
    }
    if highlights != mask.highlight_pixel_count || shadows != mask.shadow_pixel_count {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "sensor clipping counts do not match its mask samples",
        ));
    }
    Ok(SensorClippingMask {
        available: true,
        dimensions,
        samples: mask.samples,
        highlight_pixel_count: highlights,
        shadow_pixel_count: shadows,
    })
}

fn validated_histogram(
    bins: Vec<u64>,
    pixel_count: u64,
) -> Result<[u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT], BridgeError> {
    let bins: [u64; EDIT_PREVIEW_HISTOGRAM_BIN_COUNT] = bins.try_into().map_err(|_| {
        BridgeError::InvalidEditPreviewOutput("every histogram must contain exactly 256 bins")
    })?;
    let sum = bins.iter().try_fold(0_u64, |sum, count| {
        sum.checked_add(*count)
            .ok_or(BridgeError::InvalidEditPreviewOutput(
                "histogram sample count overflows u64",
            ))
    })?;
    if sum != pixel_count {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "every histogram sum must equal pixel_count",
        ));
    }
    Ok(bins)
}

fn validated_channel_counts(values: Vec<u64>) -> Result<[u64; 3], BridgeError> {
    values.try_into().map_err(|_| {
        BridgeError::InvalidEditPreviewOutput(
            "per-channel clipping counts must contain exactly three values",
        )
    })
}

fn validated_hdr_headroom(
    bins: Vec<u64>,
    headroom_pixels: u64,
    peak_ev: f64,
    pixel_count: u64,
) -> Result<[u64; EDIT_PREVIEW_HDR_HEADROOM_BIN_COUNT], BridgeError> {
    let bins: [u64; EDIT_PREVIEW_HDR_HEADROOM_BIN_COUNT] = bins.try_into().map_err(|_| {
        BridgeError::InvalidEditPreviewOutput(
            "HDR headroom must contain exactly sixteen one-stop bins",
        )
    })?;
    let sum = bins.iter().try_fold(0_u64, |sum, count| {
        sum.checked_add(*count)
            .ok_or(BridgeError::InvalidEditPreviewOutput(
                "HDR headroom sample count overflows u64",
            ))
    })?;
    if headroom_pixels > pixel_count || sum != headroom_pixels {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "HDR headroom bins must exactly cover their bounded pixel count",
        ));
    }
    if !peak_ev.is_finite()
        || peak_ev < 0.0
        || (headroom_pixels == 0 && peak_ev != 0.0)
        || (headroom_pixels > 0 && peak_ev <= 0.0)
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "HDR headroom peak EV is inconsistent with its sample coverage",
        ));
    }
    Ok(bins)
}

fn validate_any_channel_clip_count(
    per_channel: &[u64; 3],
    any_channel: u64,
    pixel_count: u64,
) -> Result<(), BridgeError> {
    let per_channel_sum = per_channel.iter().try_fold(0_u64, |sum, count| {
        if *count > pixel_count {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "per-channel clipping count exceeds pixel_count",
            ));
        }
        sum.checked_add(*count)
            .ok_or(BridgeError::InvalidEditPreviewOutput(
                "clipping sample count overflows u64",
            ))
    })?;
    let largest_channel = per_channel.iter().copied().max().unwrap_or(0);
    if any_channel > pixel_count || any_channel < largest_channel || any_channel > per_channel_sum {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "any-channel clipping count violates per-channel bounds",
        ));
    }
    Ok(())
}

fn validate_edit_preview_analysis(
    analysis: ffi::FfiEditPreviewAnalysis,
    proxy_dimensions: ImageDimensions,
) -> Result<EditPreviewAnalysis, BridgeError> {
    if analysis.version != EDIT_PREVIEW_ANALYSIS_VERSION {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "analysis version is unsupported",
        ));
    }
    let sample_dimensions = dimensions(&analysis.sample_dimensions);
    if sample_dimensions.width == 0
        || sample_dimensions.height == 0
        || sample_dimensions != proxy_dimensions
    {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "analysis dimensions must be non-zero and match the proxy",
        ));
    }
    if analysis.pixel_count != sample_dimensions.pixel_count() {
        return Err(BridgeError::InvalidEditPreviewOutput(
            "analysis pixel_count must equal width times height",
        ));
    }

    let red = validated_histogram(analysis.red, analysis.pixel_count)?;
    let green = validated_histogram(analysis.green, analysis.pixel_count)?;
    let blue = validated_histogram(analysis.blue, analysis.pixel_count)?;
    let luma = validated_histogram(analysis.luma, analysis.pixel_count)?;
    let below_zero_samples = validated_channel_counts(analysis.below_zero_samples)?;
    let above_one_samples = validated_channel_counts(analysis.above_one_samples)?;
    let hdr_headroom_bins = validated_hdr_headroom(
        analysis.hdr_headroom_bins,
        analysis.hdr_headroom_pixels,
        analysis.hdr_peak_headroom_ev,
        analysis.pixel_count,
    )?;

    for channel in 0..3 {
        if below_zero_samples[channel]
            .checked_add(above_one_samples[channel])
            .is_none_or(|total| total > analysis.pixel_count)
        {
            return Err(BridgeError::InvalidEditPreviewOutput(
                "one channel cannot be below zero and above one for the same pixel",
            ));
        }
    }
    validate_any_channel_clip_count(
        &below_zero_samples,
        analysis.shadow_clipped_pixels,
        analysis.pixel_count,
    )?;
    validate_any_channel_clip_count(
        &above_one_samples,
        analysis.highlight_clipped_pixels,
        analysis.pixel_count,
    )?;

    Ok(EditPreviewAnalysis {
        version: analysis.version,
        sample_dimensions,
        red,
        green,
        blue,
        luma,
        below_zero_samples,
        above_one_samples,
        hdr_headroom_bins,
        hdr_headroom_pixels: analysis.hdr_headroom_pixels,
        hdr_peak_headroom_ev: analysis.hdr_peak_headroom_ev,
        pixel_count: analysis.pixel_count,
        shadow_clipped_pixels: analysis.shadow_clipped_pixels,
        highlight_clipped_pixels: analysis.highlight_clipped_pixels,
    })
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
