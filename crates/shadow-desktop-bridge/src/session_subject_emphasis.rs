//! Local image evidence and bounded photographic policy for subject emphasis.
//! Qwen supplies descriptions, never Recipe parameters. The user chooses a
//! query; measured visible subject/background pixels determine one small edit.

use anyhow::{Context, Result, bail};
use image::ImageFormat;
use shadow_ai::RasterExtent;
use shadow_domain::PhotoGeometry;

use crate::{
    DesktopSession, ffi,
    session_subject_mask::subject_mask_terminal,
    subject_mask_runtime::geometry::project_gray8_mask_to_output,
    subject_mask_service::{PreparedSubjectMaskInput, SubjectMaskProposalPreview},
};

impl DesktopSession {
    pub(crate) fn analyze_subject_emphasis(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiSubjectMaskRequest,
    ) -> Result<ffi::FfiSubjectMaskResult> {
        let cancellation = self.subject_masks.cancellation(request.job_token)?;
        let token = self.begin_basic_edit_preview();
        if token == 0 {
            bail!("subject analysis preview registry is full");
        }
        if let Err(error) = self
            .subject_masks
            .attach_preview_render(request.job_token, token)
        {
            let _ = self.cancel_basic_edit_preview(token);
            return Err(error.into());
        }
        // Unlike SAM's original-space input, Qwen sees the current final crop.
        // The render reuses the editor's upstream foundation and warm caches.
        let input = self.render_subject_mask_input_preview(
            photo_id,
            source_path,
            &ffi::FfiEditPreviewRequest {
                base_commit_id: request.base_commit_id.clone(),
                settings: request.settings.clone(),
                render_token: token,
                max_edge: 1_024,
                jpeg_quality: 90,
                policy: ffi::FfiEditPreviewPolicy::Settled,
                use_working_recipe: true,
                mask_coverage_requested: false,
                mask_coverage_target_layer_index: 0,
                mask_coverage_component_requested: false,
                mask_coverage_target_component_index: 0,
                mask_selection_revision: 0,
            },
        )?;
        let mut result = subject_mask_terminal(
            request,
            ffi::FfiSubjectMaskTerminal::Cancelled,
            0,
            String::new(),
        );
        if input.terminal == ffi::FfiEditPreviewTerminal::Cancelled || cancellation.is_cancelled() {
            self.subject_masks.finish_job(request.job_token)?;
            return Ok(result);
        }
        if input.terminal != ffi::FfiEditPreviewTerminal::Completed || input.row_stride_bytes != 0 {
            bail!("subject analysis did not receive a normalized JPEG");
        }
        let revision = blake3::hash(&input.bytes).to_hex().to_string();
        let evidence = self
            .subject_mask_runtime
            .client()?
            .describe_image_cancellable(&input.bytes, &revision, &cancellation)?;
        self.subject_masks.finish_job(request.job_token)?;
        if let Some(evidence) = evidence.filter(|_| !cancellation.is_cancelled()) {
            if evidence.width != input.width || evidence.height != input.height {
                bail!("subject analysis image extent changed");
            }
            result.terminal = ffi::FfiSubjectMaskTerminal::AnalysisReady;
            result.description = evidence.analysis.short_caption.text;
            result.subject_queries = evidence
                .analysis
                .suggestions
                .into_iter()
                .map(|item| item.display_label)
                .collect();
            result.analysis_preview_jpeg = input.bytes;
            result.analysis_model = evidence.provenance.physical_model;
        }
        Ok(result)
    }
}

pub(crate) fn recommend_emphasis(
    input: &PreparedSubjectMaskInput,
    mask: &SubjectMaskProposalPreview,
    geometry: PhotoGeometry,
    result: &mut ffi::FfiSubjectMaskResult,
) -> Result<()> {
    let image = image::load_from_memory_with_format(&input.bytes, ImageFormat::Jpeg)?.to_rgb8();
    let extent = RasterExtent::new(image.width(), image.height())?;
    if extent != input.coordinate_extent || mask.coordinate_extent != extent {
        bail!("subject emphasis input and mask geometry disagree");
    }
    let output = RasterExtent::new(result.preview_width, result.preview_height)?;
    let mut light = Vec::with_capacity(image.as_raw().len() / 3);
    let mut color = Vec::with_capacity(light.capacity());
    for pixel in image.pixels() {
        // Display-referred measurements choose a small bounded edit, not an EV
        // reconstruction or an estimate of RAW highlight headroom.
        let [red, green, blue] = pixel.0.map(u32::from);
        light.push(
            u8::try_from((13_933 * red + 46_871 * green + 4_732 * blue + 32_768) / 65_536)
                .unwrap_or(u8::MAX),
        );
        let high = u16::from(*pixel.0.iter().max().unwrap());
        let low = u16::from(*pixel.0.iter().min().unwrap());
        let chroma = ((high - low) * 255 + high / 2)
            .checked_div(high)
            .unwrap_or(0);
        color.push(u8::try_from(chroma).unwrap_or(u8::MAX));
    }
    let light = project_gray8_mask_to_output(&light, extent, extent, geometry, output)
        .context("subject light projection failed")?;
    let color = project_gray8_mask_to_output(&color, extent, extent, geometry, output)
        .context("subject color projection failed")?;
    let plan = measured_plan(&light, &color, &result.preview_samples);
    result.emphasis_exposure = plan.exposure;
    result.emphasis_saturation = plan.saturation;
    result.emphasis_background = plan.background;
    result.emphasis_reason = plan.reason;
    Ok(())
}

#[derive(Debug, PartialEq)]
struct EmphasisPlan {
    exposure: f64,
    saturation: f64,
    background: bool,
    reason: u8,
}

fn measured_plan(light: &[u8], color: &[u8], mask: &[u8]) -> EmphasisPlan {
    let unchanged = |reason| EmphasisPlan {
        exposure: 0.0,
        saturation: 1.0,
        background: false,
        reason,
    };
    if light.is_empty() || light.len() != mask.len() || color.len() != mask.len() {
        return unchanged(4);
    }
    let mut weight = [0.0; 2];
    let mut brightness = [0.0; 2];
    let mut saturation = [0.0; 2];
    let mut highlights = 0.0;
    let mut certain = 0.0;
    for ((&l, &c), &m) in light.iter().zip(color).zip(mask) {
        let a = f64::from(m) / 255.0;
        certain += if m <= 32 || m >= 223 { 1.0 } else { 0.0 };
        highlights += if l >= 235 { a } else { 0.0 };
        for (i, w) in [a, 1.0 - a].into_iter().enumerate() {
            weight[i] += w;
            brightness[i] += w * f64::from(l) / 255.0;
            saturation[i] += w * f64::from(c) / 255.0;
        }
    }
    let sample_count = weight[0] + weight[1];
    let coverage = weight[0] / sample_count;
    if !(0.002..=0.85).contains(&coverage) || certain / sample_count < 0.7 {
        return unchanged(4);
    }
    for i in 0..2 {
        brightness[i] /= weight[i];
        saturation[i] /= weight[i];
    }
    let [subject, background] = brightness;
    // Preserve silhouettes and already strong tonal separation. Never infer
    // that a dark subject was an exposure mistake.
    if subject < 0.10 || (subject - background).abs() > 0.24 {
        return unchanged(3);
    }
    if background > 0.45
        && (subject > 0.72 || highlights / weight[0] > 0.04 || saturation[1] > saturation[0] + 0.15)
    {
        return EmphasisPlan {
            exposure: -0.12,
            saturation: if saturation[1] > 0.2 { 0.96 } else { 1.0 },
            background: true,
            reason: 2,
        };
    }
    if (0.14..0.68).contains(&subject)
        && subject - background < 0.12
        && highlights / weight[0] < 0.02
    {
        return EmphasisPlan {
            exposure: 0.18,
            saturation: 1.0,
            background: false,
            reason: 1,
        };
    }
    unchanged(3)
}

#[cfg(test)]
mod tests;
