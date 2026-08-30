//! Typed Consumer admission for Infer Runtime's SAM 2.1 probability masks.
//!
//! Shadow verifies the bounded Runtime response here, then keeps staging,
//! preview, apply/discard, and stale-result authority in the desktop layer.

use base64::{Engine as _, engine::general_purpose::STANDARD};
use image::{ColorType, ImageFormat};
use sha2::{Digest as _, Sha256};
use std::time::Duration;

use super::{
    InferRuntimeClient, InferRuntimeClientError, VisionProvenance, admit_vision_provenance,
    local_metadata,
};
use crate::{
    CancellationToken, MaskPointPolarity, MaskPromptPoint, RasterExtent,
    providers::infer_runtime::SemanticGroundedRegion,
};

pub const INFER_SUBJECT_MASK_CAPABILITY: &str =
    "infer.vision.subject-segmentation-soft-mask@20260814.1";
pub const INFER_SUBJECT_MASK_ENCODING: &str = "gray8_sigmoid_probability_png";
pub const INFER_SUBJECT_MASK_EDGE: u32 = 256;

/// Exact soft-mask evidence admitted from Infer Runtime. `samples` are native
/// SAM Gray8 sigmoid probabilities, never a Shadow thresholded approximation.
#[derive(Debug, Clone)]
pub struct InferSubjectMaskEvidence {
    pub samples: Vec<u8>,
    pub input_extent: RasterExtent,
    pub score: f32,
    pub provenance: VisionProvenance,
}

impl InferRuntimeClient {
    /// Calls the negotiated additive SAM soft-mask capability and validates
    /// the returned probability raster before product code may stage it.
    pub fn segment_subject_soft_mask(
        &self,
        image: &[u8],
        source_revision: &str,
        points: &[MaskPromptPoint],
    ) -> Result<InferSubjectMaskEvidence, InferRuntimeClientError> {
        let cancellation = CancellationToken::default();
        self.segment_subject_soft_mask_cancellable(image, source_revision, points, &cancellation)?
            .ok_or_else(|| {
                InferRuntimeClientError::Input(
                    "subject-mask unexpectedly cancelled without a cancellation request".into(),
                )
            })
    }

    /// The cancellation branch drops the in-flight SDK future. For Infer's
    /// soft-mask capability, that closes the loopback HTTP request and makes
    /// the Runtime kill/wait its resident SAM worker before returning.
    pub fn segment_subject_soft_mask_cancellable(
        &self,
        image: &[u8],
        source_revision: &str,
        points: &[MaskPromptPoint],
        cancellation: &CancellationToken,
    ) -> Result<Option<InferSubjectMaskEvidence>, InferRuntimeClientError> {
        if points.is_empty() || points.len() > 16 {
            return Err(InferRuntimeClientError::Input(
                "subject-mask requires 1 through 16 prompt points".into(),
            ));
        }
        let input_extent = image_extent(image, ImageFormat::Jpeg)?;
        let (staged, media_type) = Self::stage_image(image, "image/jpeg", source_revision)?;
        let points = points
            .iter()
            .map(|point| infer_runtime_client::SegmentationPromptPoint {
                x: point.x.get() as f32,
                y: point.y.get() as f32,
                label: match point.polarity {
                    MaskPointPolarity::Foreground => {
                        infer_runtime_client::SegmentationPromptLabel::Foreground
                    }
                    MaskPointPolarity::Background => {
                        infer_runtime_client::SegmentationPromptLabel::Background
                    }
                },
            })
            .collect::<Vec<_>>();
        let metadata = local_metadata("interactive", None);
        let request = self.sdk().segment_subject_soft_mask(
            staged.path(),
            media_type,
            source_revision,
            &points,
            None,
            &metadata,
        );
        let cancellation = cancellation.clone();
        let response = self.runtime.block_on(async {
            tokio::select! {
                response = request => response.map(Some),
                () = wait_for_cancellation(cancellation) => Ok(None),
            }
        })?;
        response
            .map(|response| admit_soft_mask(response, source_revision, input_extent))
            .transpose()
    }

    /// Refines one normalized grounding box through the same SAM soft-mask
    /// capability without inventing a synthetic click prompt.
    pub fn segment_subject_box_soft_mask_cancellable(
        &self,
        image: &[u8],
        source_revision: &str,
        box_prompt: SemanticGroundedRegion,
        cancellation: &CancellationToken,
    ) -> Result<Option<InferSubjectMaskEvidence>, InferRuntimeClientError> {
        let input_extent = image_extent(image, ImageFormat::Jpeg)?;
        let (staged, media_type) = Self::stage_image(image, "image/jpeg", source_revision)?;
        let metadata = local_metadata("interactive", None);
        let request = self.sdk().segment_subject_soft_mask(
            staged.path(),
            media_type,
            source_revision,
            &[],
            Some(infer_runtime_client::NormalizedBoundingBox {
                x: box_prompt.x,
                y: box_prompt.y,
                width: box_prompt.width,
                height: box_prompt.height,
            }),
            &metadata,
        );
        let cancellation = cancellation.clone();
        let response = self.runtime.block_on(async {
            tokio::select! {
                response = request => response.map(Some),
                () = wait_for_cancellation(cancellation) => Ok(None),
            }
        })?;
        response
            .map(|response| admit_soft_mask(response, source_revision, input_extent))
            .transpose()
    }
}

async fn wait_for_cancellation(cancellation: CancellationToken) {
    while !cancellation.is_cancelled() {
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

fn admit_soft_mask(
    response: infer_runtime_client::SubjectSegmentationSoftMaskResponse,
    expected_source_revision: &str,
    expected_input_extent: RasterExtent,
) -> Result<InferSubjectMaskEvidence, InferRuntimeClientError> {
    if response.object != "vision.subject_segmentation_soft_mask"
        || response.status != "completed"
        || response.source_revision != expected_source_revision
        || response.mask.content_type != "image/png"
        || response.mask.encoding != INFER_SUBJECT_MASK_ENCODING
        || response.mask.width != INFER_SUBJECT_MASK_EDGE
        || response.mask.height != INFER_SUBJECT_MASK_EDGE
        || response.raster_extent.width != INFER_SUBJECT_MASK_EDGE
        || response.raster_extent.height != INFER_SUBJECT_MASK_EDGE
        || response.raster_extent.coordinate_mapping != "linear_full_extent_pixel_centers_v1"
        || response.input_coordinate_extent.orientation != "display_pixels_orientation_normalized"
        || response.input_coordinate_extent.width != expected_input_extent.width
        || response.input_coordinate_extent.height != expected_input_extent.height
        || !response.score.is_finite()
        || !(0.0..=1.0).contains(&response.score)
    {
        return Err(InferRuntimeClientError::MalformedResponse(
            "Infer Runtime returned an invalid subject soft-mask response".into(),
        ));
    }
    let png = STANDARD
        .decode(response.mask.data_base64.as_bytes())
        .map_err(|_| {
            InferRuntimeClientError::MalformedResponse("soft-mask is not base64".into())
        })?;
    let digest = format!("{:x}", Sha256::digest(&png));
    if digest != response.mask.sha256 {
        return Err(InferRuntimeClientError::MalformedResponse(
            "soft-mask SHA-256 does not match its PNG bytes".into(),
        ));
    }
    let decoded = image::load_from_memory_with_format(&png, ImageFormat::Png).map_err(|_| {
        InferRuntimeClientError::MalformedResponse("soft-mask is not a valid PNG".into())
    })?;
    if decoded.color() != ColorType::L8
        || decoded.width() != INFER_SUBJECT_MASK_EDGE
        || decoded.height() != INFER_SUBJECT_MASK_EDGE
    {
        return Err(InferRuntimeClientError::MalformedResponse(
            "soft-mask must be a native 256x256 Gray8 PNG".into(),
        ));
    }
    let samples = decoded.into_luma8().into_raw();
    if samples.len() != (INFER_SUBJECT_MASK_EDGE * INFER_SUBJECT_MASK_EDGE) as usize {
        return Err(InferRuntimeClientError::MalformedResponse(
            "soft-mask decoded sample count is invalid".into(),
        ));
    }
    Ok(InferSubjectMaskEvidence {
        samples,
        input_extent: expected_input_extent,
        score: response.score,
        provenance: admit_vision_provenance(response.provenance)?,
    })
}

fn image_extent(
    image: &[u8],
    format: ImageFormat,
) -> Result<RasterExtent, InferRuntimeClientError> {
    let decoded = image::load_from_memory_with_format(image, format).map_err(|_| {
        InferRuntimeClientError::Input("subject-mask input must be a valid JPEG".into())
    })?;
    RasterExtent::new(decoded.width(), decoded.height()).map_err(|_| {
        InferRuntimeClientError::Input("subject-mask input dimensions are invalid".into())
    })
}
