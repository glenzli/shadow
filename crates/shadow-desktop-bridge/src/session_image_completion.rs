//! Desktop-session orchestration for AI image completion.
//!
//! Brush authoring stays transient. Generation renders a completion-free,
//! identity-geometry source, prepares one bounded crop and exact mask, and
//! stages a candidate. Only the separate apply transaction promotes bytes and
//! appends one region to the fixed photo-local completion node.

use std::io::Cursor;

use anyhow::{Context, Result as AnyResult, bail};
use image::{DynamicImage, GrayImage, ImageFormat, imageops::FilterType};
use shadow_ai::{MaskPointPolarity, MaskPromptPoint, RasterExtent, UnitInterval as AiUnitInterval};
use shadow_domain::{PhotoId, UnitInterval};

use super::{
    DesktopSession, ffi,
    image_completion_runtime::{ImageCompletionInvocation, ImageCompletionRuntimeError},
    image_completion_service::{ImageCompletionCompletion, ImageCompletionPlacement},
    recipe_v1::{decode_grade_stack_draft_recipe_v1, resolve_recipe_render},
    subject_mask_runtime::geometry::{
        map_output_prompt_to_original, project_rgba8_patch_to_output,
    },
    wall_clock::current_time_ms,
};

const COMPLETION_INPUT_MAX_EDGE: u32 = 1_024;
const COMPLETION_INPUT_JPEG_QUALITY: u8 = 95;
const COMPLETION_MODEL_EDGE: u32 = 512;
const COMPLETION_PREVIEW_EDGE: u32 = 512;
const MAX_BRUSH_POINTS: usize = 8_192;
const MIN_BRUSH_RADIUS: f64 = 0.002;
const MAX_BRUSH_RADIUS: f64 = 0.25;

#[derive(Debug, Clone, Copy)]
struct OriginalBrushPoint {
    x: f64,
    y: f64,
    radius_x: f64,
    radius_y: f64,
    erase: bool,
    stroke_id: u32,
}

#[derive(Debug)]
struct PreparedCompletionInput {
    crop_png: Vec<u8>,
    mask_png: Vec<u8>,
    mask_gray8: Vec<u8>,
    placement: ImageCompletionPlacement,
}

impl DesktopSession {
    pub(crate) fn begin_image_completion_job(&self) -> AnyResult<u64> {
        Ok(self.image_completions.begin_job()?)
    }

    pub(crate) fn cancel_image_completion_job(&self, job_token: u64) -> AnyResult<()> {
        if let Some(render_token) = self.image_completions.cancel_job(job_token)? {
            let _ = self.cancel_basic_edit_preview(render_token);
        }
        Ok(())
    }

    pub(crate) fn execute_image_completion_job(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiImageCompletionRequest,
    ) -> AnyResult<ffi::FfiImageCompletionResult> {
        match self.execute_image_completion_job_inner(photo_id, source_path, request) {
            Ok(result) => Ok(result),
            Err(error) => {
                let _ = self.image_completions.finish_job(request.job_token);
                Err(error)
            }
        }
    }

    fn execute_image_completion_job_inner(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiImageCompletionRequest,
    ) -> AnyResult<ffi::FfiImageCompletionResult> {
        self.validated_photo_source(photo_id, source_path)?;
        let cancellation = self.image_completions.cancellation(request.job_token)?;
        if cancellation.is_cancelled() {
            self.image_completions.finish_job(request.job_token)?;
            return Ok(image_completion_terminal(
                request,
                ffi::FfiImageCompletionTerminal::Cancelled,
                0,
                String::new(),
            ));
        }
        if request.points.is_empty() || request.points.len() > MAX_BRUSH_POINTS {
            bail!("AI completion requires 1 through {MAX_BRUSH_POINTS} brush samples");
        }
        let grade_stack = decode_grade_stack_draft_recipe_v1(&request.settings)?;
        if grade_stack
            .liquify
            .as_ref()
            .is_some_and(shadow_domain::PhotoLiquifyNode::enabled)
        {
            bail!("AI completion is authored before Liquify; bypass Liquify before generating");
        }
        let coordinate_geometry = grade_stack.canvas.effective_geometry();

        let mut source_settings = request.settings.clone();
        // Completion is evaluated after Repair and before the two structural
        // stages. Preserve already accepted completion regions as the input
        // to a later region, but never bake Liquify or Canvas into its pixels.
        source_settings.liquify_enabled = false;
        source_settings.liquify_strokes.clear();
        source_settings.geometry = identity_ffi_geometry();
        let photo_id_parsed = photo_id
            .parse::<PhotoId>()
            .context("parse AI completion photo identity")?;
        let source_recipe = resolve_recipe_render(
            &self.catalog,
            &self.cache_root,
            photo_id_parsed,
            &request.base_commit_id,
            &source_settings,
            true,
        )?;
        let source_recipe_blake3 = source_recipe
            .snapshot_digest
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>();

        let render_token = self.begin_basic_edit_preview();
        if render_token == 0 {
            bail!("AI completion input preview registry is full");
        }
        self.image_completions
            .attach_preview_render(request.job_token, render_token)?;
        let input = self.render_subject_mask_input_preview(
            photo_id,
            source_path,
            &ffi::FfiEditPreviewRequest {
                base_commit_id: request.base_commit_id.clone(),
                settings: source_settings,
                render_token,
                max_edge: COMPLETION_INPUT_MAX_EDGE,
                jpeg_quality: COMPLETION_INPUT_JPEG_QUALITY,
                policy: ffi::FfiEditPreviewPolicy::Settled,
                use_working_recipe: true,
                mask_coverage_requested: false,
                mask_coverage_target_layer_index: 0,
                mask_coverage_component_requested: false,
                mask_coverage_target_component_index: 0,
                mask_selection_revision: 0,
            },
        )?;
        if input.terminal == ffi::FfiEditPreviewTerminal::Cancelled || cancellation.is_cancelled() {
            self.image_completions.finish_job(request.job_token)?;
            return Ok(image_completion_terminal(
                request,
                ffi::FfiImageCompletionTerminal::Cancelled,
                0,
                String::new(),
            ));
        }
        if input.terminal != ffi::FfiEditPreviewTerminal::Completed || input.row_stride_bytes != 0 {
            bail!("AI completion input preview returned an invalid terminal payload");
        }
        let coordinate_extent = RasterExtent::new(input.width, input.height)
            .context("AI completion input preview dimensions are invalid")?;
        let original_points =
            original_brush_points(&request.points, coordinate_geometry, coordinate_extent)?;
        let prepared = prepare_completion_input(&input.bytes, coordinate_extent, &original_points)?;
        let mask_revision = blake3::hash(&prepared.mask_gray8).to_hex().to_string();
        let receipt = match self.image_completion_runtime.stage(
            self.image_completions.store(),
            ImageCompletionInvocation {
                request_id: format!(
                    "shadow-image-completion-{}-{}",
                    request.job_token, request.generation
                ),
                promotion_id: format!(
                    "shadow-image-completion-promotion-{}-{}",
                    request.job_token, request.generation
                ),
                generation: request.generation,
                photo_id: photo_id.to_owned(),
                prepared_crop_png: prepared.crop_png,
                prepared_mask_png: prepared.mask_png,
                prepared_mask_gray8: prepared.mask_gray8,
                coordinate_extent,
                source_recipe_blake3,
                mask_revision,
            },
            &cancellation,
        ) {
            Ok(receipt) => receipt,
            Err(ImageCompletionRuntimeError::Infer(error)) => {
                self.image_completions.finish_job(request.job_token)?;
                return Ok(image_completion_terminal(
                    request,
                    ffi::FfiImageCompletionTerminal::Unavailable,
                    0,
                    error.to_string(),
                ));
            }
            Err(error) => return Err(error.into()),
        };
        let completion =
            self.image_completions
                .complete_job(request.job_token, receipt, prepared.placement)?;
        Ok(match completion {
            ImageCompletionCompletion::Staged {
                generation,
                proposal_token,
            } => {
                let preview = self.image_completions.proposal_preview(proposal_token)?;
                if preview.generation != generation {
                    let _ = self.image_completions.discard_proposal(proposal_token);
                    bail!("AI completion candidate generation changed before presentation");
                }
                let raster_extent = RasterExtent::new(preview.raster_width, preview.raster_height)
                    .context("AI completion candidate extent is invalid")?;
                let output_extent =
                    RasterExtent::new(COMPLETION_PREVIEW_EDGE, COMPLETION_PREVIEW_EDGE)
                        .expect("completion preview extent is valid");
                let Some(preview_rgba8) = project_rgba8_patch_to_output(
                    &preview.rgba8,
                    raster_extent,
                    coordinate_extent,
                    preview.placement.bounds_left,
                    preview.placement.bounds_top,
                    preview.placement.bounds_right,
                    preview.placement.bounds_bottom,
                    coordinate_geometry,
                    output_extent,
                ) else {
                    let _ = self.image_completions.discard_proposal(proposal_token);
                    bail!("AI completion candidate projection failed");
                };
                ffi::FfiImageCompletionResult {
                    terminal: ffi::FfiImageCompletionTerminal::Staged,
                    job_token: request.job_token,
                    generation,
                    proposal_token,
                    detail: String::new(),
                    preview_width: output_extent.width,
                    preview_height: output_extent.height,
                    preview_rgba8,
                }
            }
            ImageCompletionCompletion::Unavailable { reason } => image_completion_terminal(
                request,
                ffi::FfiImageCompletionTerminal::Unavailable,
                0,
                format!("{reason:?}"),
            ),
            ImageCompletionCompletion::Cancelled => image_completion_terminal(
                request,
                ffi::FfiImageCompletionTerminal::Cancelled,
                0,
                String::new(),
            ),
            ImageCompletionCompletion::Failed { failure } => image_completion_terminal(
                request,
                ffi::FfiImageCompletionTerminal::Failed,
                0,
                format!("{failure:?}"),
            ),
        })
    }

    pub(crate) fn apply_image_completion_proposal(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiImageCompletionApplyRequest,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        self.validated_photo_source(photo_id, source_path)?;
        let mut grade_stack = decode_grade_stack_draft_recipe_v1(&request.settings)?;
        let region = self
            .image_completions
            .promote_proposal(request.proposal_token, request.generation)?;
        grade_stack.image_completions.push(region);
        grade_stack.image_completion_enabled = true;
        self.autosave_grade_stack_working_at(
            photo_id,
            source_path,
            &request.base_commit_id,
            &request.expected_working_commit_id,
            &grade_stack,
            current_time_ms()?,
        )
    }

    pub(crate) fn discard_image_completion_proposal(&self, proposal_token: u64) -> AnyResult<()> {
        Ok(self.image_completions.discard_proposal(proposal_token)?)
    }
}

fn original_brush_points(
    points: &[ffi::FfiImageCompletionBrushPoint],
    geometry: shadow_domain::PhotoGeometry,
    extent: RasterExtent,
) -> AnyResult<Vec<OriginalBrushPoint>> {
    points
        .iter()
        .enumerate()
        .map(|(index, point)| {
            if !point.x.is_finite()
                || !point.y.is_finite()
                || !point.radius.is_finite()
                || !(0.0..=1.0).contains(&point.x)
                || !(0.0..=1.0).contains(&point.y)
                || !(MIN_BRUSH_RADIUS..=MAX_BRUSH_RADIUS).contains(&point.radius)
            {
                bail!("AI completion brush sample {index} is invalid");
            }
            let map = |x: f64, y: f64| {
                map_output_prompt_to_original(
                    MaskPromptPoint {
                        x: AiUnitInterval::new(x.clamp(0.0, 1.0))
                            .expect("clamped brush x is valid"),
                        y: AiUnitInterval::new(y.clamp(0.0, 1.0))
                            .expect("clamped brush y is valid"),
                        polarity: MaskPointPolarity::Foreground,
                    },
                    geometry,
                    extent,
                )
            };
            let center = map(point.x, point.y);
            let horizontal = map((point.x + point.radius).min(1.0), point.y);
            let vertical = map(point.x, (point.y + point.radius).min(1.0));
            Ok(OriginalBrushPoint {
                x: center.x.get(),
                y: center.y.get(),
                radius_x: (horizontal.x.get() - center.x.get())
                    .abs()
                    .max(MIN_BRUSH_RADIUS),
                radius_y: (vertical.y.get() - center.y.get())
                    .abs()
                    .max(MIN_BRUSH_RADIUS),
                erase: point.erase,
                stroke_id: point.stroke_id,
            })
        })
        .collect()
}

fn prepare_completion_input(
    jpeg: &[u8],
    coordinate_extent: RasterExtent,
    points: &[OriginalBrushPoint],
) -> AnyResult<PreparedCompletionInput> {
    let decoded = image::load_from_memory_with_format(jpeg, ImageFormat::Jpeg)
        .context("decode AI completion input preview")?;
    if decoded.width() != coordinate_extent.width || decoded.height() != coordinate_extent.height {
        bail!("AI completion input geometry changed during preparation");
    }
    let placement = completion_placement(points)?;
    let left = bounded_floor_u32(
        placement.bounds_left.get() * f64::from(decoded.width()),
        decoded.width() - 1,
    );
    let top = bounded_floor_u32(
        placement.bounds_top.get() * f64::from(decoded.height()),
        decoded.height() - 1,
    );
    let right = bounded_ceil_u32(
        placement.bounds_right.get() * f64::from(decoded.width()),
        decoded.width(),
    )
    .max(left + 1);
    let bottom = bounded_ceil_u32(
        placement.bounds_bottom.get() * f64::from(decoded.height()),
        decoded.height(),
    )
    .max(top + 1);
    let crop = decoded.crop_imm(left, top, right - left, bottom - top);
    let crop = crop.resize_exact(
        COMPLETION_MODEL_EDGE,
        COMPLETION_MODEL_EDGE,
        FilterType::CatmullRom,
    );
    let crop_png = encode_png(&crop)?;
    let mask_gray8 = rasterize_mask(points, placement);
    if !mask_gray8.iter().any(|sample| *sample != 0) {
        bail!("AI completion selection is empty after brush projection");
    }
    let mask_png = encode_png(&DynamicImage::ImageLuma8(
        GrayImage::from_raw(
            COMPLETION_MODEL_EDGE,
            COMPLETION_MODEL_EDGE,
            mask_gray8.clone(),
        )
        .context("construct AI completion Gray8 mask")?,
    ))?;
    Ok(PreparedCompletionInput {
        crop_png,
        mask_png,
        mask_gray8,
        placement,
    })
}

fn completion_placement(points: &[OriginalBrushPoint]) -> AnyResult<ImageCompletionPlacement> {
    let mut left = 1.0_f64;
    let mut top = 1.0_f64;
    let mut right = 0.0_f64;
    let mut bottom = 0.0_f64;
    for point in points.iter().filter(|point| !point.erase) {
        left = left.min(point.x - point.radius_x);
        top = top.min(point.y - point.radius_y);
        right = right.max(point.x + point.radius_x);
        bottom = bottom.max(point.y + point.radius_y);
    }
    if left >= right || top >= bottom {
        bail!("AI completion selection contains no painted area");
    }
    let padding = ((right - left).max(bottom - top) * 0.75).max(0.06);
    left = (left - padding).clamp(0.0, 1.0);
    top = (top - padding).clamp(0.0, 1.0);
    right = (right + padding).clamp(0.0, 1.0);
    bottom = (bottom + padding).clamp(0.0, 1.0);
    Ok(ImageCompletionPlacement {
        bounds_left: UnitInterval::new(left)?,
        bounds_top: UnitInterval::new(top)?,
        bounds_right: UnitInterval::new(right)?,
        bounds_bottom: UnitInterval::new(bottom)?,
    })
}

fn rasterize_mask(points: &[OriginalBrushPoint], placement: ImageCompletionPlacement) -> Vec<u8> {
    let mut mask = vec![0_u8; 512_usize * 512_usize];
    let mut previous: Option<OriginalBrushPoint> = None;
    for point in points {
        if let Some(start) = previous.filter(|start| start.stroke_id == point.stroke_id) {
            let distance = bounded_ceil_u32(
                ((point.x - start.x).powi(2) + (point.y - start.y).powi(2)).sqrt()
                    * f64::from(COMPLETION_MODEL_EDGE)
                    * 2.0,
                COMPLETION_MODEL_EDGE * 2,
            )
            .max(1);
            for step in 0..=distance {
                let t = f64::from(step) / f64::from(distance);
                paint_mask_point(
                    &mut mask,
                    OriginalBrushPoint {
                        x: start.x + (point.x - start.x) * t,
                        y: start.y + (point.y - start.y) * t,
                        radius_x: start.radius_x + (point.radius_x - start.radius_x) * t,
                        radius_y: start.radius_y + (point.radius_y - start.radius_y) * t,
                        erase: point.erase,
                        stroke_id: point.stroke_id,
                    },
                    placement,
                );
            }
        } else {
            paint_mask_point(&mut mask, *point, placement);
        }
        previous = Some(*point);
    }
    mask
}

fn paint_mask_point(
    mask: &mut [u8],
    point: OriginalBrushPoint,
    placement: ImageCompletionPlacement,
) {
    let width = placement.bounds_right.get() - placement.bounds_left.get();
    let height = placement.bounds_bottom.get() - placement.bounds_top.get();
    let center_x =
        (point.x - placement.bounds_left.get()) / width * f64::from(COMPLETION_MODEL_EDGE);
    let center_y =
        (point.y - placement.bounds_top.get()) / height * f64::from(COMPLETION_MODEL_EDGE);
    let radius_x = (point.radius_x / width * f64::from(COMPLETION_MODEL_EDGE)).max(1.0);
    let radius_y = (point.radius_y / height * f64::from(COMPLETION_MODEL_EDGE)).max(1.0);
    let min_x = bounded_floor_u32(center_x - radius_x, COMPLETION_MODEL_EDGE - 1);
    let max_x = bounded_ceil_u32(center_x + radius_x, COMPLETION_MODEL_EDGE - 1);
    let min_y = bounded_floor_u32(center_y - radius_y, COMPLETION_MODEL_EDGE - 1);
    let max_y = bounded_ceil_u32(center_y + radius_y, COMPLETION_MODEL_EDGE - 1);
    for y in min_y..=max_y {
        for x in min_x..=max_x {
            let dx = (f64::from(x) + 0.5 - center_x) / radius_x;
            let dy = (f64::from(y) + 0.5 - center_y) / radius_y;
            if dx.mul_add(dx, dy * dy) <= 1.0 {
                let index = usize::try_from(y * COMPLETION_MODEL_EDGE + x)
                    .expect("bounded completion mask index fits usize");
                mask[index] = if point.erase { 0 } else { 255 };
            }
        }
    }
}

fn bounded_floor_u32(value: f64, maximum: u32) -> u32 {
    let bounded = value.floor().clamp(0.0, f64::from(maximum));
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    {
        bounded as u32
    }
}

fn bounded_ceil_u32(value: f64, maximum: u32) -> u32 {
    let bounded = value.ceil().clamp(0.0, f64::from(maximum));
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    {
        bounded as u32
    }
}

fn encode_png(image: &DynamicImage) -> AnyResult<Vec<u8>> {
    let mut output = Cursor::new(Vec::new());
    image
        .write_to(&mut output, ImageFormat::Png)
        .context("encode AI completion PNG")?;
    Ok(output.into_inner())
}

fn image_completion_terminal(
    request: &ffi::FfiImageCompletionRequest,
    terminal: ffi::FfiImageCompletionTerminal,
    proposal_token: u64,
    detail: String,
) -> ffi::FfiImageCompletionResult {
    ffi::FfiImageCompletionResult {
        terminal,
        job_token: request.job_token,
        generation: request.generation,
        proposal_token,
        detail,
        preview_width: 0,
        preview_height: 0,
        preview_rgba8: Vec::new(),
    }
}

const fn identity_ffi_geometry() -> ffi::FfiPhotoGeometry {
    ffi::FfiPhotoGeometry {
        present: false,
        enabled: true,
        crop_left: 0.0,
        crop_top: 0.0,
        crop_right: 1.0,
        crop_bottom: 1.0,
        quarter_turn: 0,
        straighten_degrees: 0.0,
        perspective_vertical: 0.0,
        perspective_horizontal: 0.0,
        flip_horizontal: false,
        flip_vertical: false,
    }
}

#[cfg(test)]
mod tests;
