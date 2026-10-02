//! Desktop-session orchestration for AI image completion.
//!
//! Brush authoring stays transient. Generation uses developed, ungraded pixels
//! so accepted regions can receive subsequent Grade Nodes. The candidate is
//! previewed through that exact stage before the separate apply transaction.

use std::{
    io::{Cursor, Read},
    path::Path,
    time::Instant,
};

use anyhow::{Context, Result as AnyResult, bail};
use image::{DynamicImage, GrayImage, ImageFormat, imageops::FilterType};
use shadow_ai::{MaskPointPolarity, MaskPromptPoint, RasterExtent, UnitInterval as AiUnitInterval};
use shadow_bridge::AdjustmentImageCompletionPatch;
use shadow_domain::{PhotoId, RepresentationKind, UnitInterval};

use super::{
    DesktopSession, ffi,
    image_completion_runtime::{ImageCompletionInvocation, ImageCompletionRuntimeError},
    image_completion_service::{
        ImageCompletionCompletion, ImageCompletionPlacement, ImageCompletionService,
    },
    recipe_v1::{decode_grade_stack_draft_recipe_v1, new_basic_grade_node, resolve_recipe_render},
    session_pipeline::representation_kind,
    subject_mask_runtime::geometry::{map_output_prompt_to_original, output_canvas_extent},
    wall_clock::current_time_ms,
};

// Match the editor's warm FIT source. A second edge makes a completed RAW
// preview decode again before AI generation and gives the 512px crop less
// original detail. The input and candidate must use the same retained edge.
const COMPLETION_INPUT_MAX_EDGE: u32 = 1_536;
const COMPLETION_INPUT_JPEG_QUALITY: u8 = 95;
const COMPLETION_MODEL_EDGE: u32 = 512;
// A tiny source crop leaves the model with little surrounding evidence even
// after it is enlarged to 512 pixels. Keep a bounded amount of real context.
const COMPLETION_MIN_CROP_EDGE: u32 = 256;
const MAX_BRUSH_POINTS: usize = 8_192;
const MIN_BRUSH_RADIUS: f64 = 0.002;
const MAX_BRUSH_RADIUS: f64 = 0.25;
const MAX_SELECTION_EXPANSION: f64 = 0.06;

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

/// Opt-in, payload-free timing for one background completion request.
struct CompletionPhaseTiming {
    job_token: u64,
    started: Option<Instant>,
    previous: Instant,
}

impl CompletionPhaseTiming {
    fn new(job_token: u64) -> Self {
        let now = Instant::now();
        Self {
            job_token,
            started: std::env::var("SHADOW_INTERACTIVE_TIMING")
                .is_ok_and(|value| value == "1")
                .then_some(now),
            previous: now,
        }
    }

    fn checkpoint(&mut self, stage: &str) {
        let Some(started) = self.started else {
            return;
        };
        let now = Instant::now();
        eprintln!(
            "shadow.completion-timing token={} stage={stage} phase_ms={} elapsed_ms={}",
            self.job_token,
            now.duration_since(self.previous).as_millis(),
            now.duration_since(started).as_millis(),
        );
        self.previous = now;
    }
}

struct CandidateProposalCleanup<'a> {
    service: &'a ImageCompletionService,
    token: u64,
}

impl CandidateProposalCleanup<'_> {
    fn disarm(&mut self) {
        self.token = 0;
    }
}

impl Drop for CandidateProposalCleanup<'_> {
    fn drop(&mut self) {
        if self.token != 0 {
            let _ = self.service.discard_proposal(self.token);
        }
    }
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
        let result = self.execute_image_completion_job_inner(photo_id, source_path, request);
        let _ = self.image_completions.finish_job(request.job_token);
        match result {
            Ok(result) => Ok(result),
            Err(error) => Err(error),
        }
    }

    fn execute_image_completion_job_inner(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiImageCompletionRequest,
    ) -> AnyResult<ffi::FfiImageCompletionResult> {
        let mut timing = CompletionPhaseTiming::new(request.job_token);
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
        let refresh_index = usize::try_from(request.refresh_region_index).ok();
        if refresh_index.is_none()
            && (request.points.is_empty() || request.points.len() > MAX_BRUSH_POINTS)
        {
            bail!("AI completion requires 1 through {MAX_BRUSH_POINTS} brush samples");
        }
        if refresh_index.is_some() && !request.points.is_empty() {
            bail!("AI completion refresh cannot also contain brush samples");
        }
        if !request.selection_expansion.is_finite()
            || !(0.0..=MAX_SELECTION_EXPANSION).contains(&request.selection_expansion)
            || (refresh_index.is_some() && request.selection_expansion != 0.0)
        {
            bail!("AI completion selection expansion is invalid");
        }
        let grade_stack = decode_grade_stack_draft_recipe_v1(&request.settings)?;
        let refresh_region = refresh_index
            .map(|index| {
                grade_stack
                    .image_completions
                    .get(index)
                    .cloned()
                    .context("AI completion refresh target is unavailable")
            })
            .transpose()?;
        // A refresh uses the accepted patch's original-space mask. Only new
        // display-space brush selections need Liquify bypassed for alignment.
        if refresh_region.is_none()
            && grade_stack
                .liquify
                .as_ref()
                .is_some_and(shadow_domain::PhotoLiquifyNode::enabled)
        {
            bail!("AI completion is authored before Liquify; bypass Liquify before generating");
        }
        let coordinate_geometry = grade_stack.canvas.effective_geometry();

        let mut source_settings = request.settings.clone();
        // A newly accepted region is composed before Grade Nodes. Keep the
        // current source development and earlier pre-grade regions, but do not
        // bake a historical grade, repair, paint or Canvas transform into it.
        source_settings.grade_nodes = vec![completion_source_node()?];
        source_settings.retouch_spots.clear();
        source_settings.retouch_strokes.clear();
        source_settings.paint_layers.clear();
        source_settings.image_completions = source_settings
            .image_completions
            .into_iter()
            .enumerate()
            .filter_map(|(index, region)| {
                (region.pre_grade && refresh_index.is_none_or(|refresh| index < refresh))
                    .then_some(region)
            })
            .collect();
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
        timing.checkpoint("source-recipe-ready");

        let render_token = self.begin_basic_edit_preview();
        if render_token == 0 {
            bail!("AI completion input preview registry is full");
        }
        if let Err(error) = self
            .image_completions
            .attach_preview_render(request.job_token, render_token)
        {
            let _ = self.cancel_basic_edit_preview(render_token);
            let _ = self.claim_basic_edit_preview_terminal(render_token);
            if cancellation.is_cancelled() {
                return Ok(image_completion_terminal(
                    request,
                    ffi::FfiImageCompletionTerminal::Cancelled,
                    0,
                    String::new(),
                ));
            }
            return Err(error.into());
        }
        let (input, color_basis) = self.render_completion_input_preview(
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
        if input.terminal != ffi::FfiEditPreviewTerminal::Completed
            || input.row_stride_bytes != input.width * 3
        {
            bail!("AI completion input preview returned an invalid terminal payload");
        }
        timing.checkpoint("source-preview-ready");
        let coordinate_extent = RasterExtent::new(input.width, input.height)
            .context("AI completion input preview dimensions are invalid")?;
        // One bounded, lossless handoff. No JPEG is decoded and re-encoded for inference.
        let input_rgb = image::RgbImage::from_raw(input.width, input.height, input.bytes)
            .context("AI completion RGB input length is invalid")?;
        let input_image = DynamicImage::ImageRgb8(input_rgb);
        let prepared = if let Some(region) = &refresh_region {
            prepare_completion_refresh_input(
                &input_image,
                coordinate_extent,
                region.patch(),
                self.image_completions.store(),
            )?
        } else {
            let crop_points =
                original_brush_points(&request.points, coordinate_geometry, coordinate_extent)?;
            if request.selection_expansion == 0.0 {
                prepare_completion_input(&input_image, coordinate_extent, &crop_points)?
            } else {
                let expanded = expanded_brush_points(&request.points, request.selection_expansion);
                let mask_points =
                    original_brush_points(&expanded, coordinate_geometry, coordinate_extent)?;
                prepare_completion_input_with_crop_points(
                    &input_image,
                    coordinate_extent,
                    &mask_points,
                    &crop_points,
                )?
            }
        };
        let mask_revision = blake3::hash(&prepared.mask_gray8).to_hex().to_string();
        timing.checkpoint("model-input-ready");
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
                source_context: Some(shadow_domain::ImageCompletionSourceContext {
                    color_basis,
                    foundation: grade_stack.foundation.clone(),
                    raw_ai_denoise: grade_stack.raw_ai_denoise,
                }),
                mask_revision,
                // RAW input remains scene-referred through development; SDR
                // originals retain display-referred appearance. Only the
                // former receives Shadow's neutral display shoulder.
                force_regenerate: request.force_regenerate || refresh_index.is_some(),
                scene_referred_input: representation_kind(Path::new(source_path))
                    == Some(RepresentationKind::OriginalRaw),
            },
            &cancellation,
        ) {
            Ok(receipt) => receipt,
            Err(ImageCompletionRuntimeError::Infer(error)) => {
                timing.checkpoint("model-unavailable");
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
        timing.checkpoint("model-result-staged");
        let completion =
            self.image_completions
                .complete_job(request.job_token, receipt, prepared.placement)?;
        timing.checkpoint("proposal-ready");
        Ok(match completion {
            ImageCompletionCompletion::Staged {
                generation,
                proposal_token,
            } => {
                let mut proposal_cleanup = CandidateProposalCleanup {
                    service: &self.image_completions,
                    token: proposal_token,
                };
                let preview = self.image_completions.proposal_preview(proposal_token)?;
                if preview.generation != generation {
                    bail!("AI completion candidate generation changed before presentation");
                }
                let candidate_render_token = self.begin_basic_edit_preview();
                if candidate_render_token == 0 {
                    bail!("AI completion candidate preview registry is full");
                }
                if let Err(error) = self
                    .image_completions
                    .replace_preview_render(request.job_token, candidate_render_token)
                {
                    let _ = self.cancel_basic_edit_preview(candidate_render_token);
                    let _ = self.claim_basic_edit_preview_terminal(candidate_render_token);
                    if cancellation.is_cancelled() {
                        return Ok(image_completion_terminal(
                            request,
                            ffi::FfiImageCompletionTerminal::Cancelled,
                            0,
                            String::new(),
                        ));
                    }
                    return Err(error.into());
                }
                let candidate_position = refresh_index.map(|index| {
                    request.settings.image_completions[..index]
                        .iter()
                        .filter(|region| region.pre_grade && region.enabled)
                        .count()
                });
                let mut candidate_settings = request.settings.clone();
                if let Some(index) = refresh_index {
                    candidate_settings.image_completions.remove(index);
                }
                let rendered_candidate = self.render_completion_candidate_preview(
                    photo_id,
                    source_path,
                    &ffi::FfiEditPreviewRequest {
                        base_commit_id: request.base_commit_id.clone(),
                        settings: candidate_settings,
                        render_token: candidate_render_token,
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
                    AdjustmentImageCompletionPatch {
                        raster_width: preview.raster_width,
                        raster_height: preview.raster_height,
                        coordinate_width: coordinate_extent.width,
                        coordinate_height: coordinate_extent.height,
                        bounds_left: preview.placement.bounds_left.get(),
                        bounds_top: preview.placement.bounds_top.get(),
                        bounds_right: preview.placement.bounds_right.get(),
                        bounds_bottom: preview.placement.bounds_bottom.get(),
                        strength: 1.0,
                        linear_rgba_f32: preview.linear_rgba_f32,
                        source_color_basis: preview.source_color_basis,
                        rgba8: preview.rgba8,
                    },
                    candidate_position,
                )?;
                timing.checkpoint("candidate-render-ready");
                if rendered_candidate.terminal != ffi::FfiEditPreviewTerminal::Completed {
                    if cancellation.is_cancelled()
                        || rendered_candidate.terminal == ffi::FfiEditPreviewTerminal::Cancelled
                    {
                        return Ok(image_completion_terminal(
                            request,
                            ffi::FfiImageCompletionTerminal::Cancelled,
                            0,
                            String::new(),
                        ));
                    }
                    bail!("AI completion candidate preview did not complete");
                }
                if cancellation.is_cancelled() {
                    return Ok(image_completion_terminal(
                        request,
                        ffi::FfiImageCompletionTerminal::Cancelled,
                        0,
                        String::new(),
                    ));
                }
                let candidate_image = image::load_from_memory_with_format(
                    &rendered_candidate.bytes,
                    ImageFormat::Jpeg,
                )
                .context("decode AI completion candidate preview")?
                .into_rgba8();
                timing.checkpoint("candidate-decode-ready");
                proposal_cleanup.disarm();
                ffi::FfiImageCompletionResult {
                    terminal: ffi::FfiImageCompletionTerminal::Staged,
                    job_token: request.job_token,
                    generation,
                    proposal_token,
                    detail: String::new(),
                    preview_width: candidate_image.width(),
                    preview_height: candidate_image.height(),
                    preview_rgba8: candidate_image.into_raw(),
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
        let (parsed_photo_id, _) = self.validated_photo_source(photo_id, source_path)?;
        self.require_active_photo_variant(parsed_photo_id, &request.expected_variant_id)?;
        self.image_completions
            .validate_proposal_photo(request.proposal_token, parsed_photo_id)?;
        let mut grade_stack = decode_grade_stack_draft_recipe_v1(&request.settings)?;
        let replace_index = usize::try_from(request.replace_region_index).ok();
        if replace_index.is_some_and(|index| index >= grade_stack.image_completions.len()) {
            bail!("AI completion replacement target is unavailable");
        }
        let region = self
            .image_completions
            .promote_proposal(request.proposal_token, request.generation)?;
        if let Some(index) = replace_index {
            grade_stack.image_completions.remove(index);
            grade_stack
                .image_completions
                .insert(index, region.with_pre_grade(true));
        } else {
            grade_stack
                .image_completions
                .push(region.with_pre_grade(true));
        }
        grade_stack.image_completion_enabled = true;
        self.autosave_grade_stack_working_for_variant_at(
            photo_id,
            source_path,
            &request.base_commit_id,
            &request.expected_working_commit_id,
            &grade_stack,
            current_time_ms()?,
            Some(&request.expected_variant_id),
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
    let (width, height) = output_canvas_extent(geometry, extent);
    let shorter_edge = width.min(height);
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
            let radius_x = point.radius * shorter_edge / width;
            let radius_y = point.radius * shorter_edge / height;
            // Sample inward near canvas edges. Use both components of the
            // mapped basis so quarter turns cannot collapse the brush radius.
            let horizontal = map(
                point.x + if point.x > 0.5 { -radius_x } else { radius_x },
                point.y,
            );
            let vertical = map(
                point.x,
                point.y + if point.y > 0.5 { -radius_y } else { radius_y },
            );
            Ok(OriginalBrushPoint {
                x: center.x.get(),
                y: center.y.get(),
                radius_x: (horizontal.x.get() - center.x.get())
                    .hypot(vertical.x.get() - center.x.get())
                    .max(f64::EPSILON),
                radius_y: (horizontal.y.get() - center.y.get())
                    .hypot(vertical.y.get() - center.y.get())
                    .max(f64::EPSILON),
                erase: point.erase,
                stroke_id: point.stroke_id,
            })
        })
        .collect()
}

fn expanded_brush_points(
    points: &[ffi::FfiImageCompletionBrushPoint],
    expansion: f64,
) -> Vec<ffi::FfiImageCompletionBrushPoint> {
    points
        .iter()
        .map(|point| ffi::FfiImageCompletionBrushPoint {
            x: point.x,
            y: point.y,
            radius: if point.erase {
                point.radius
            } else {
                (point.radius + expansion).min(MAX_BRUSH_RADIUS)
            },
            erase: point.erase,
            stroke_id: point.stroke_id,
        })
        .collect()
}

// Reserved, transient identities keep identical source snapshots/cache keys stable.
fn completion_source_node() -> AnyResult<ffi::FfiGradeNode> {
    let mut node = new_basic_grade_node("Completion source")?;
    for (name, slot) in [
        ("grade", &mut node.grade_node_id),
        ("exposure", &mut node.exposure_render_op_id),
        ("contrast", &mut node.contrast_render_op_id),
        ("tone", &mut node.selective_tone_render_op_id),
        ("wb", &mut node.white_balance_render_op_id),
        ("saturation", &mut node.saturation_render_op_id),
        ("color", &mut node.perceptual_color_render_op_id),
        ("lut", &mut node.lut_render_op_id),
        ("detail", &mut node.sharpen_render_op_id),
    ] {
        let digest = blake3::hash(format!("shadow.completion.source-node.v1:{name}").as_bytes());
        let mut bytes = [0_u8; 16];
        bytes.copy_from_slice(&digest.as_bytes()[..16]);
        bytes[6] = (bytes[6] & 0x0f) | 0x80;
        bytes[8] = (bytes[8] & 0x3f) | 0x80;
        *slot = uuid::Uuid::from_bytes(bytes).to_string();
    }
    Ok(node)
}

fn prepare_completion_input(
    decoded: &DynamicImage,
    coordinate_extent: RasterExtent,
    points: &[OriginalBrushPoint],
) -> AnyResult<PreparedCompletionInput> {
    prepare_completion_input_with_crop_points(decoded, coordinate_extent, points, points)
}

fn prepare_completion_input_with_crop_points(
    decoded: &DynamicImage,
    coordinate_extent: RasterExtent,
    mask_points: &[OriginalBrushPoint],
    crop_points: &[OriginalBrushPoint],
) -> AnyResult<PreparedCompletionInput> {
    let base_placement = completion_placement(crop_points, coordinate_extent)?;
    let placement = if selection_fits_placement(mask_points, base_placement) {
        base_placement
    } else {
        completion_placement(mask_points, coordinate_extent)?
    };
    let mask_gray8 = rasterize_mask(mask_points, placement);
    prepare_completion_input_with_mask(decoded, coordinate_extent, placement, mask_gray8)
}

fn selection_fits_placement(
    points: &[OriginalBrushPoint],
    placement: ImageCompletionPlacement,
) -> bool {
    points.iter().filter(|point| !point.erase).all(|point| {
        (point.x - point.radius_x).max(0.0) >= placement.bounds_left.get()
            && (point.y - point.radius_y).max(0.0) >= placement.bounds_top.get()
            && (point.x + point.radius_x).min(1.0) <= placement.bounds_right.get()
            && (point.y + point.radius_y).min(1.0) <= placement.bounds_bottom.get()
    })
}

fn prepare_completion_refresh_input(
    decoded: &DynamicImage,
    coordinate_extent: RasterExtent,
    patch: &shadow_domain::ManagedImageCompletionPatch,
    store: &shadow_core::FilesystemDerivedRasterStore,
) -> AnyResult<PreparedCompletionInput> {
    let mut rgba8 = Vec::with_capacity(usize::try_from(patch.byte_len())?);
    store
        .open_recipe_completion_patch(patch)?
        .take(patch.byte_len().saturating_add(1))
        .read_to_end(&mut rgba8)?;
    if u64::try_from(rgba8.len())? != patch.byte_len() {
        bail!("AI completion refresh patch changed while reading");
    }
    let mask = if patch.linear_rgba_f32() {
        rgba8
            .chunks_exact(16)
            .map(|pixel| {
                let alpha = f32::from_le_bytes(pixel[12..16].try_into().expect("alpha sample"));
                if alpha > 0.0 { 255 } else { 0 }
            })
            .collect()
    } else {
        rgba8
            .chunks_exact(4)
            .map(|pixel| if pixel[3] == 0 { 0 } else { 255 })
            .collect()
    };
    let mask = GrayImage::from_raw(patch.raster_width(), patch.raster_height(), mask)
        .context("construct AI completion refresh mask")?;
    let mask_gray8 = image::imageops::resize(
        &mask,
        COMPLETION_MODEL_EDGE,
        COMPLETION_MODEL_EDGE,
        FilterType::Nearest,
    )
    .into_raw();
    prepare_completion_input_with_mask(
        decoded,
        coordinate_extent,
        ImageCompletionPlacement {
            bounds_left: patch.bounds_left(),
            bounds_top: patch.bounds_top(),
            bounds_right: patch.bounds_right(),
            bounds_bottom: patch.bounds_bottom(),
        },
        mask_gray8,
    )
}

fn prepare_completion_input_with_mask(
    decoded: &DynamicImage,
    coordinate_extent: RasterExtent,
    placement: ImageCompletionPlacement,
    mask_gray8: Vec<u8>,
) -> AnyResult<PreparedCompletionInput> {
    if decoded.width() != coordinate_extent.width || decoded.height() != coordinate_extent.height {
        bail!("AI completion input geometry changed during preparation");
    }
    let left = bounded_round_u32(
        placement.bounds_left.get() * f64::from(decoded.width()),
        decoded.width() - 1,
    );
    let top = bounded_round_u32(
        placement.bounds_top.get() * f64::from(decoded.height()),
        decoded.height() - 1,
    );
    let right = bounded_round_u32(
        placement.bounds_right.get() * f64::from(decoded.width()),
        decoded.width(),
    )
    .max(left + 1);
    let bottom = bounded_round_u32(
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

fn completion_placement(
    points: &[OriginalBrushPoint],
    extent: RasterExtent,
) -> AnyResult<ImageCompletionPlacement> {
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
    let width = f64::from(extent.width);
    let height = f64::from(extent.height);
    let selected_width = (right - left) * width;
    let selected_height = (bottom - top) * height;
    let selected_edge = selected_width.max(selected_height);
    let padding = (selected_edge * 0.75).max(f64::from(extent.width.min(extent.height)) * 0.06);
    let desired_edge = bounded_ceil_u32(
        (selected_edge + 2.0 * padding).max(f64::from(COMPLETION_MIN_CROP_EDGE)),
        extent.width.max(extent.height),
    );
    // For ordinary selections this is a square in source pixels, not a
    // rectangle stretched into the model's square input. A selection larger
    // than the short image edge still remains fully included.
    let crop_width = desired_edge.min(extent.width);
    let crop_height = desired_edge.min(extent.height);
    let crop_left = square_crop_origin(left * width, right * width, crop_width, extent.width);
    let crop_top = square_crop_origin(top * height, bottom * height, crop_height, extent.height);
    left = f64::from(crop_left) / width;
    top = f64::from(crop_top) / height;
    right = f64::from(crop_left + crop_width) / width;
    bottom = f64::from(crop_top + crop_height) / height;
    Ok(ImageCompletionPlacement {
        bounds_left: UnitInterval::new(left)?,
        bounds_top: UnitInterval::new(top)?,
        bounds_right: UnitInterval::new(right)?,
        bounds_bottom: UnitInterval::new(bottom)?,
    })
}

fn square_crop_origin(selection_start: f64, selection_end: f64, crop: u32, image: u32) -> u32 {
    let centered = ((selection_start + selection_end - f64::from(crop)) * 0.5).round();
    bounded_round_u32(centered, image - crop)
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

fn bounded_round_u32(value: f64, maximum: u32) -> u32 {
    let bounded = value.round().clamp(0.0, f64::from(maximum));
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
