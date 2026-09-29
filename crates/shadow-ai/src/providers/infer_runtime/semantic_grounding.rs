//! Strict Consumer admission for Infer Runtime semantic grounding regions.

use image::ImageFormat;
use std::time::Duration;

use super::{
    InferRuntimeClient, InferRuntimeClientError, VisionProvenance, admit_vision_provenance,
    local_metadata, malformed,
};
use crate::{CancellationToken, RasterExtent};

pub const INFER_SEMANTIC_GROUNDING_CAPABILITY: &str = "infer.vision.semantic-grounding@20260830.1";

#[derive(Debug, Copy, Clone, PartialEq)]
pub struct SemanticGroundedRegion {
    pub score: f32,
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}

#[derive(Debug, Clone, PartialEq)]
pub struct SemanticGroundingEvidence {
    pub source_revision: String,
    pub query_revision: String,
    pub input_extent: RasterExtent,
    pub regions: Vec<SemanticGroundedRegion>,
    pub provenance: VisionProvenance,
}

impl InferRuntimeClient {
    /// Grounds one bounded semantic query into normalized candidate regions.
    /// Dropping the SDK future is the same cooperative cancellation boundary
    /// used by interactive SAM requests.
    ///
    /// # Errors
    ///
    /// Returns an error when the request is invalid, Runtime discovery or
    /// execution fails, or the response violates Shadow's bounded geometry
    /// and provenance contract.
    #[allow(clippy::too_many_arguments)]
    pub fn ground_semantics_cancellable(
        &self,
        image: &[u8],
        source_revision: &str,
        query: &str,
        query_revision: &str,
        maximum_regions: u8,
        score_threshold: f32,
        cancellation: &CancellationToken,
    ) -> Result<Option<SemanticGroundingEvidence>, InferRuntimeClientError> {
        if query.trim().is_empty() || query.len() > 256 {
            return Err(InferRuntimeClientError::Input(
                "semantic grounding query must be non-empty and at most 256 bytes".into(),
            ));
        }
        if query_revision.trim().is_empty() || query_revision.len() > 256 {
            return Err(InferRuntimeClientError::Input(
                "semantic grounding query revision must be non-empty and at most 256 bytes".into(),
            ));
        }
        if !(1..=8).contains(&maximum_regions)
            || !score_threshold.is_finite()
            || !(0.01..=1.0).contains(&score_threshold)
        {
            return Err(InferRuntimeClientError::Input(
                "semantic grounding bounds are invalid".into(),
            ));
        }
        let decoded =
            image::load_from_memory_with_format(image, ImageFormat::Jpeg).map_err(|_| {
                InferRuntimeClientError::Input(
                    "semantic grounding input must be a valid JPEG".into(),
                )
            })?;
        let input_extent = RasterExtent::new(decoded.width(), decoded.height()).map_err(|_| {
            InferRuntimeClientError::Input("semantic grounding input dimensions are invalid".into())
        })?;
        let (staged, media_type) = Self::stage_image(image, "image/jpeg", source_revision)?;
        let metadata = local_metadata("interactive", None);
        let request = self.sdk().ground_semantics(
            staged.path(),
            media_type,
            source_revision,
            query,
            query_revision,
            maximum_regions,
            score_threshold,
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
                admit_grounding(
                    response,
                    source_revision,
                    query_revision,
                    input_extent,
                    maximum_regions,
                    score_threshold,
                )
            })
            .transpose()
    }
}

async fn wait_for_cancellation(cancellation: CancellationToken) {
    while !cancellation.is_cancelled() {
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

fn admit_grounding(
    response: infer_runtime_client::SemanticGroundingResponse,
    expected_source_revision: &str,
    expected_query_revision: &str,
    expected_extent: RasterExtent,
    maximum_regions: u8,
    score_threshold: f32,
) -> Result<SemanticGroundingEvidence, InferRuntimeClientError> {
    if response.object != "vision.semantic_grounding"
        || response.status != "completed"
        || response.source_revision != expected_source_revision
        || response.query_revision != expected_query_revision
        || response.image.orientation != "display_pixels_orientation_normalized"
        || response.image.width != expected_extent.width
        || response.image.height != expected_extent.height
        || response.regions.len() > usize::from(maximum_regions)
    {
        return malformed("semantic grounding response violated Shadow's typed contract");
    }
    let mut regions = Vec::with_capacity(response.regions.len());
    for region in response.regions {
        let bounds = region.bounding_box;
        if region.region_id.trim().is_empty()
            || !region.score.is_finite()
            || !(score_threshold..=1.0).contains(&region.score)
            || !valid_normalized_box(bounds.x, bounds.y, bounds.width, bounds.height)
        {
            return malformed("semantic grounding region violated Shadow's typed contract");
        }
        regions.push(SemanticGroundedRegion {
            score: region.score,
            x: bounds.x,
            y: bounds.y,
            width: bounds.width,
            height: bounds.height,
        });
    }
    Ok(SemanticGroundingEvidence {
        source_revision: response.source_revision,
        query_revision: response.query_revision,
        input_extent: expected_extent,
        regions,
        provenance: admit_vision_provenance(response.provenance)?,
    })
}

fn valid_normalized_box(x: f32, y: f32, width: f32, height: f32) -> bool {
    x.is_finite()
        && y.is_finite()
        && width.is_finite()
        && height.is_finite()
        && x >= 0.0
        && y >= 0.0
        && width > 0.0
        && height > 0.0
        && x + width <= 1.0 + f32::EPSILON
        && y + height <= 1.0 + f32::EPSILON
}

#[cfg(test)]
mod tests;
