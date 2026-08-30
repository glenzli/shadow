//! Strict Consumer admission for Infer Runtime image-completion proposals.
//!
//! The Runtime owns `LaMa` model execution. Shadow admits only one exact,
//! bounded 512-square RGB result with matching source, mask, geometry, digest,
//! and provenance before product code may stage it.

use std::time::Duration;

use base64::{Engine as _, engine::general_purpose::STANDARD};
use image::{ColorType, ImageFormat};
use sha2::{Digest as _, Sha256};

use super::{
    InferRuntimeClient, InferRuntimeClientError, VisionProvenance, admit_vision_provenance,
    local_metadata,
};
use crate::{CancellationToken, RasterExtent};

pub const INFER_IMAGE_COMPLETION_CAPABILITY: &str = "infer.vision.image-completion@20260830.1";
pub const INFER_IMAGE_COMPLETION_EDGE: u32 = 512;
const INFER_IMAGE_COMPLETION_ENCODING: &str = "srgb_rgb8_png_mask_bounded_v1";

#[derive(Debug, Clone)]
pub struct InferImageCompletionEvidence {
    pub rgb8: Vec<u8>,
    pub raster_extent: RasterExtent,
    pub input_extent: RasterExtent,
    pub provenance: VisionProvenance,
}

impl InferRuntimeClient {
    /// Completes one prepared 512-square RGB crop and Gray8 selection mask.
    /// Cancellation drops the SDK future, closing the loopback request and
    /// forwarding the Runtime's existing cooperative worker cancellation.
    ///
    /// # Errors
    ///
    /// Returns an error when either PNG violates the bounded input contract,
    /// Runtime discovery or execution fails, or the response does not match
    /// the requested source, mask, geometry, digest, and provenance.
    pub fn complete_image_cancellable(
        &self,
        image_png: &[u8],
        mask_png: &[u8],
        source_revision: &str,
        mask_revision: &str,
        cancellation: &CancellationToken,
    ) -> Result<Option<InferImageCompletionEvidence>, InferRuntimeClientError> {
        let image_extent = png_extent(image_png, false)?;
        let mask_extent = png_extent(mask_png, true)?;
        if image_extent != mask_extent
            || image_extent.width != INFER_IMAGE_COMPLETION_EDGE
            || image_extent.height != INFER_IMAGE_COMPLETION_EDGE
        {
            return Err(InferRuntimeClientError::Input(
                "image completion requires matching 512x512 image and mask PNGs".into(),
            ));
        }
        if source_revision.trim().is_empty() || mask_revision.trim().is_empty() {
            return Err(InferRuntimeClientError::Input(
                "image completion requires non-empty source and mask revisions".into(),
            ));
        }
        let (staged_image, content_type) =
            Self::stage_image(image_png, "image/png", source_revision)?;
        let (staged_mask, _) = Self::stage_image(mask_png, "image/png", mask_revision)?;
        let metadata = local_metadata("interactive", None);
        let request = self.sdk().complete_image(
            staged_image.path(),
            content_type,
            staged_mask.path(),
            source_revision,
            mask_revision,
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
            .map(|response| {
                admit_completion(response, source_revision, mask_revision, image_extent)
            })
            .transpose()
    }
}

async fn wait_for_cancellation(cancellation: CancellationToken) {
    while !cancellation.is_cancelled() {
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

fn admit_completion(
    response: infer_runtime_client::ImageCompletionResponse,
    expected_source_revision: &str,
    expected_mask_revision: &str,
    expected_input_extent: RasterExtent,
) -> Result<InferImageCompletionEvidence, InferRuntimeClientError> {
    if response.object != "vision.image_completion"
        || response.status != "completed"
        || response.source_revision != expected_source_revision
        || response.mask_revision != expected_mask_revision
        || response.input_coordinate_extent.orientation != "display_pixels_orientation_normalized"
        || response.input_coordinate_extent.width != expected_input_extent.width
        || response.input_coordinate_extent.height != expected_input_extent.height
        || response.raster.content_type != "image/png"
        || response.raster.encoding != INFER_IMAGE_COMPLETION_ENCODING
        || response.raster.width != INFER_IMAGE_COMPLETION_EDGE
        || response.raster.height != INFER_IMAGE_COMPLETION_EDGE
    {
        return malformed("image completion response violated Shadow's typed contract");
    }
    let png = STANDARD
        .decode(response.raster.data_base64.as_bytes())
        .map_err(|_| {
            InferRuntimeClientError::MalformedResponse(
                "image completion raster is not base64".into(),
            )
        })?;
    if format!("{:x}", Sha256::digest(&png)) != response.raster.sha256 {
        return malformed("image completion SHA-256 does not match its PNG bytes");
    }
    let decoded = image::load_from_memory_with_format(&png, ImageFormat::Png).map_err(|_| {
        InferRuntimeClientError::MalformedResponse(
            "image completion raster is not a valid PNG".into(),
        )
    })?;
    if decoded.width() != INFER_IMAGE_COMPLETION_EDGE
        || decoded.height() != INFER_IMAGE_COMPLETION_EDGE
        || !matches!(decoded.color(), ColorType::Rgb8 | ColorType::Rgba8)
    {
        return malformed("image completion raster must be a 512x512 RGB8 PNG");
    }
    let rgb8 = decoded.into_rgb8().into_raw();
    if rgb8.len()
        != usize::try_from(INFER_IMAGE_COMPLETION_EDGE * INFER_IMAGE_COMPLETION_EDGE * 3)
            .expect("512-square RGB byte count fits usize")
    {
        return malformed("image completion decoded byte length is invalid");
    }
    Ok(InferImageCompletionEvidence {
        rgb8,
        raster_extent: RasterExtent::new(INFER_IMAGE_COMPLETION_EDGE, INFER_IMAGE_COMPLETION_EDGE)
            .expect("completion extent is valid"),
        input_extent: expected_input_extent,
        provenance: admit_vision_provenance(response.provenance)?,
    })
}

fn png_extent(bytes: &[u8], require_gray: bool) -> Result<RasterExtent, InferRuntimeClientError> {
    let decoded = image::load_from_memory_with_format(bytes, ImageFormat::Png)
        .map_err(|_| InferRuntimeClientError::Input("completion input must be a PNG".into()))?;
    if require_gray && decoded.color() != ColorType::L8 {
        return Err(InferRuntimeClientError::Input(
            "completion mask must be a Gray8 PNG".into(),
        ));
    }
    RasterExtent::new(decoded.width(), decoded.height()).map_err(|_| {
        InferRuntimeClientError::Input("completion input dimensions are invalid".into())
    })
}

fn malformed<T>(message: &str) -> Result<T, InferRuntimeClientError> {
    Err(InferRuntimeClientError::MalformedResponse(message.into()))
}

#[cfg(test)]
mod tests;
