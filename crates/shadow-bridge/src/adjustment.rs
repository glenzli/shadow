//! Typed adjustment graph, geometry, local masks, parameters, and validation.
//!
//! This module keeps the complete plan contract together so every renderer entry point shares one
//! fail-closed validation chain.

use std::collections::HashSet;

use shadow_domain::ImageDimensions;

use super::{BridgeError, MAX_WARM_EDIT_PREVIEW_EDGE};

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
pub(super) fn validate_render_operation(
    operation: &AdjustmentRenderOperation,
) -> Result<(), BridgeError> {
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
    pub(super) fn validate(self) -> Result<(), BridgeError> {
        self.edits.validate()?;
        validate_proxy_max_edge(self.max_edge)?;
        validate_jpeg_quality(self.jpeg_quality)
    }
}

pub(super) fn validate_proxy_max_edge(max_edge: u32) -> Result<(), BridgeError> {
    if (1..=16_384).contains(&max_edge) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "max_edge must be in 1..=16384",
        ))
    }
}

pub(super) fn validate_warm_edit_max_edge(max_edge: u32) -> Result<(), BridgeError> {
    if (1..=MAX_WARM_EDIT_PREVIEW_EDGE).contains(&max_edge) {
        Ok(())
    } else {
        Err(BridgeError::InvalidEditRequest(
            "warm edit preview max_edge must be in 1..=4096",
        ))
    }
}

pub(super) fn validate_jpeg_quality(jpeg_quality: u8) -> Result<(), BridgeError> {
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
