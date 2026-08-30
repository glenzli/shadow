//! Public adjustment-contract index and cross-operation plan composition.
//!
//! The complete operation sum type and plan dispatch remain the shared composition boundary.
//! Responsibility-named children own geometry, masks, retouch, LUT, tone, color, and Detail &
//! Effects contracts whose invariants evolve independently.

use std::collections::HashSet;

use super::{BridgeError, MAX_WARM_EDIT_PREVIEW_EDGE};

mod detail_effects;
mod geometry;
mod image_completion;
mod liquify;
mod local_mask;
mod lut;
mod oklab_color_warper;
mod oklab_lightness_curve;
mod parameter_validation;
mod perceptual_color;
mod retouch;
mod selective_tone;

pub use detail_effects::{
    AdjustmentDetailEffectsPass, COLOR_GRADING_IMPLEMENTATION_VERSION,
    COLOR_GRADING_PARAMETER_SCHEMA_VERSION, FINISHING_EFFECTS_IMPLEMENTATION_VERSION,
    FINISHING_EFFECTS_PARAMETER_SCHEMA_VERSION, SharpenParameters,
    TECHNICAL_DETAIL_IMPLEMENTATION_VERSION, TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION,
};
pub use geometry::{AdjustmentGeometry, AdjustmentQuarterTurn};
pub use image_completion::{
    AdjustmentImageCompletionPatch, MAX_ADJUSTMENT_IMAGE_COMPLETION_BYTES,
    MAX_ADJUSTMENT_IMAGE_COMPLETION_EDGE, MAX_ADJUSTMENT_IMAGE_COMPLETION_PATCHES,
};
pub use liquify::{
    AdjustmentLiquify, AdjustmentLiquifyPoint, AdjustmentLiquifyPushStroke,
    AdjustmentLiquifyReconstructStroke, AdjustmentLiquifyStroke,
    MAX_ADJUSTMENT_LIQUIFY_POINTS_PER_STROKE, MAX_ADJUSTMENT_LIQUIFY_STROKES,
};
pub use local_mask::{
    AdjustmentLocalMask, AdjustmentMaskBrushPoint, AdjustmentRasterMaskEncoding,
    MAX_MANAGED_RASTER_MASK_BYTES,
};
pub use lut::MAX_LUT_DOCUMENT_BYTES;
pub use oklab_color_warper::{
    OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT, OKLAB_COLOR_WARPER_GRID_SIDE,
    OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION, OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
    OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION, OklabColorWarperControlPoint,
    OklabColorWarperParameters,
};
pub use oklab_lightness_curve::{
    MAX_TONE_CURVE_POINTS, OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION, OklabLightnessToneCurve, ToneCurvePoint,
};
pub use perceptual_color::{
    COLOR_MIXER_BAND_COUNT, ColorRangeParameters, MAX_POINT_COLOR_RANGES,
    PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION, PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION,
    PerceptualColorParameters, SELECTIVE_COLOR_COMPONENT_COUNT, SELECTIVE_COLOR_TARGET_COUNT,
    SELECTIVE_COLOR_VALUE_COUNT,
};
pub use retouch::{
    AdjustmentRetouchStroke, AdjustmentRetouchStrokePoint, AdjustmentSpotHealTarget,
};
pub use selective_tone::{
    SELECTIVE_TONE_IMPLEMENTATION_VERSION, SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION,
    SelectiveToneParameters,
};

use detail_effects::validate_sharpen;
use image_completion::validate_image_completion;
use local_mask::validate_adjustment_local_mask;
use lut::validate_lut;
use oklab_color_warper::validate_oklab_color_warper;
use oklab_lightness_curve::validate_tone_curve_points;
use parameter_validation::{validate_finite_render_parameter, validate_inclusive};
use perceptual_color::validate_perceptual_color;
use retouch::validate_spot_heal;
use selective_tone::validate_selective_tone;

/// Numeric v1 contract used by the non-curve adjustment operations.
pub const ADJUSTMENT_PARAMETER_SCHEMA_VERSION: u32 = 1;
/// Numeric v1 executor revision.
pub const ADJUSTMENT_IMPLEMENTATION_VERSION: u32 = 1;
/// Hard bound for one linearized render plan crossing the language boundary.
pub const MAX_ADJUSTMENT_RENDER_NODES: usize = 256;
/// Hard bound for diagnostic node identities crossing the language boundary.
pub const MAX_ADJUSTMENT_NODE_ID_BYTES: usize = 256;

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
    /// Accepted photo-local generated pixels. Alpha in every RGBA8 patch is
    /// the exact user-authored selection and bounds use original-image space.
    ImageCompletion {
        patches: Vec<AdjustmentImageCompletionPatch>,
    },
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
    /// Optional, photo-private displacement authored in original-image
    /// coordinates. Execution order is fixed after `nodes` and before
    /// `geometry`; it is not a shareable or reorderable adjustment node.
    pub liquify: Option<AdjustmentLiquify>,
    /// Photo-local final-canvas geometry compiled independently from the
    /// original-coordinate adjustment stream and applied after Liquify.
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
        if let Some(liquify) = &self.liquify {
            liquify.validate()?;
        }
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
                AdjustmentRenderOperation::Sharpen { pass, .. } => {
                    pass.contract_versions()
                        == (node.parameter_schema_version, node.implementation_version)
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
        } => validate_lut(document, *intensity),
        AdjustmentRenderOperation::Sharpen { parameters, .. } => validate_sharpen(parameters),
        AdjustmentRenderOperation::SpotHeal { targets, strokes } => {
            validate_spot_heal(targets, strokes)
        }
        AdjustmentRenderOperation::ImageCompletion { patches } => {
            validate_image_completion(patches)
        }
    }
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
        liquify: None,
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
