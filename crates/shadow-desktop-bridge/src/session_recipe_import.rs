//! DesktopSession orchestration for adaptive semantic Shadow Recipe import.

use crate::recipe_lut_resources::install_lut_resources;
use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_ai::{RasterExtent, SoftMaskEncoding};
use shadow_domain::ShadowRecipeDocument;

use crate::{
    DesktopSession, ffi,
    recipe_import_plan::{RecipeImportItemId, RecipeImportPlan, SemanticLeafResolution},
    recipe_import_service::{
        RecipeImportExecutionContext, RecipeImportItemSnapshot, RecipeImportItemTerminal,
        RecipeImportPlanSnapshot, RecipeImportProposalPreview, RecipeImportTargetIdentity,
    },
    recipe_v1::{decode_grade_stack_draft_recipe_v1, encode_grade_stack_draft_recipe_v1},
    subject_mask_runtime::{
        SemanticMaskInvocation, SemanticMaskStageOutcome, geometry::project_gray8_mask_to_output,
    },
    subject_mask_service::SubjectMaskRefinement,
    subject_mask_service::{
        SubjectMaskCompletion, SubjectMaskInputAdmission, SubjectMaskRenderInputIdentity,
    },
};

const RECIPE_IMPORT_INPUT_MAX_EDGE: u32 = 1_024;
const RECIPE_IMPORT_INPUT_JPEG_QUALITY: u8 = 95;
const RECIPE_IMPORT_PREVIEW_EDGE: u32 = 256;

impl DesktopSession {
    pub(crate) fn prepare_semantic_recipe_import(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        document_bytes: &[u8],
    ) -> AnyResult<ffi::FfiRecipeImportPlan> {
        self.validated_photo_source(photo_id, source_path)?;
        let document = ShadowRecipeDocument::from_json(document_bytes)
            .context("decode semantic Shadow Recipe import document")?;
        let installed = install_lut_resources(&document, &self.recipe_lut_store()?)?;
        let plan = RecipeImportPlan::from_document_with_luts(&document, &installed)?;
        let target = recipe_import_target(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            settings,
        )?;
        let input_session_token = self.subject_masks.begin_input_session()?;
        match self
            .recipe_imports
            .insert(target, plan, input_session_token)
        {
            Ok(snapshot) => recipe_import_plan_ffi(snapshot),
            Err(error) => {
                let _ = self.subject_masks.finish_input_session(input_session_token);
                Err(error)
            }
        }
    }

    pub(crate) fn semantic_recipe_import_plan(
        &self,
        plan_token: u64,
    ) -> AnyResult<ffi::FfiRecipeImportPlan> {
        recipe_import_plan_ffi(self.recipe_imports.snapshot(plan_token)?)
    }

    pub(crate) fn begin_semantic_recipe_import_item(
        &self,
        plan_token: u64,
        item_id: &str,
    ) -> AnyResult<u64> {
        let job_token = self.subject_masks.begin_job()?;
        if let Err(error) = self
            .recipe_imports
            .begin_item(plan_token, item_id, job_token)
        {
            let _ = self.subject_masks.finish_job(job_token);
            return Err(error);
        }
        Ok(job_token)
    }

    pub(crate) fn execute_semantic_recipe_import_item(
        &self,
        plan_token: u64,
        item_id: &str,
        job_token: u64,
    ) -> AnyResult<ffi::FfiRecipeImportItem> {
        let context = self
            .recipe_imports
            .execution_context(plan_token, item_id, job_token)?;
        let result = self.execute_semantic_recipe_stage(&context)?;
        let (terminal, detail, proposal_token, preview) = match result {
            SemanticRecipeExecution::Staged {
                proposal_token,
                preview,
            } => (
                RecipeImportItemTerminal::Staged,
                String::new(),
                Some(proposal_token),
                Some(preview),
            ),
            SemanticRecipeExecution::NotFound => (
                RecipeImportItemTerminal::NotFound,
                String::new(),
                None,
                None,
            ),
            SemanticRecipeExecution::Unavailable(detail) => {
                (RecipeImportItemTerminal::Unavailable, detail, None, None)
            }
            SemanticRecipeExecution::Cancelled => (
                RecipeImportItemTerminal::Cancelled,
                String::new(),
                None,
                None,
            ),
            SemanticRecipeExecution::Failed(detail) => {
                (RecipeImportItemTerminal::Failed, detail, None, None)
            }
        };
        if !self.recipe_imports.complete_item(
            &context,
            terminal,
            detail,
            proposal_token,
            preview,
        )? {
            if let Some(token) = proposal_token {
                let _ = self.subject_masks.discard_proposal(token);
            }
            bail!("semantic Recipe import result became stale before publication");
        }
        recipe_import_item_ffi(
            self.recipe_imports
                .snapshot(plan_token)?
                .items
                .into_iter()
                .find(|item| item.leaf.item_id.to_string() == item_id)
                .ok_or_else(|| anyhow!("semantic Recipe import item is unavailable"))?,
        )
    }

    fn execute_semantic_recipe_stage(
        &self,
        context: &RecipeImportExecutionContext,
    ) -> AnyResult<SemanticRecipeExecution> {
        match self.execute_semantic_recipe_stage_inner(context) {
            Ok(result) => Ok(result),
            Err(error) => {
                let _ = self.subject_masks.finish_job(context.job_token);
                Err(error)
            }
        }
    }

    fn execute_semantic_recipe_stage_inner(
        &self,
        context: &RecipeImportExecutionContext,
    ) -> AnyResult<SemanticRecipeExecution> {
        let cancellation = self.subject_masks.cancellation(context.job_token)?;
        if cancellation.is_cancelled() {
            self.subject_masks.finish_job(context.job_token)?;
            return Ok(SemanticRecipeExecution::Cancelled);
        }
        let input_identity = SubjectMaskRenderInputIdentity {
            photo_id: context.target.photo_id.clone(),
            source_path: context.target.source_path.clone(),
            base_commit_id: context.target.base_commit_id.clone(),
            grade_stack: context.target.settings.clone(),
        };
        let prepared_input = match self
            .subject_masks
            .admit_original_space_input(context.input_session_token, &input_identity)?
        {
            SubjectMaskInputAdmission::Reuse(input) => input,
            SubjectMaskInputAdmission::Prepare => {
                let prepared = (|| -> AnyResult<Option<_>> {
                    let render_token = self.begin_basic_edit_preview();
                    if render_token == 0 {
                        bail!("semantic Recipe import input preview registry is full");
                    }
                    if let Err(error) = self
                        .subject_masks
                        .attach_preview_render(context.job_token, render_token)
                    {
                        let _ = self.cancel_basic_edit_preview(render_token);
                        return Err(error.into());
                    }
                    let mut settings =
                        encode_grade_stack_draft_recipe_v1(context.target.settings.clone())?;
                    settings.geometry = identity_recipe_import_geometry();
                    let preview = self.render_subject_mask_input_preview(
                        &context.target.photo_id,
                        &context.target.source_path,
                        &ffi::FfiEditPreviewRequest {
                            base_commit_id: context.target.base_commit_id.clone(),
                            settings,
                            render_token,
                            max_edge: RECIPE_IMPORT_INPUT_MAX_EDGE,
                            jpeg_quality: RECIPE_IMPORT_INPUT_JPEG_QUALITY,
                            policy: ffi::FfiEditPreviewPolicy::Settled,
                            use_working_recipe: true,
                            mask_coverage_requested: false,
                            mask_coverage_target_layer_index: 0,
                            mask_coverage_component_requested: false,
                            mask_coverage_target_component_index: 0,
                            mask_selection_revision: 0,
                        },
                    )?;
                    if preview.terminal == ffi::FfiEditPreviewTerminal::Cancelled
                        || cancellation.is_cancelled()
                    {
                        return Ok(None);
                    }
                    if preview.terminal != ffi::FfiEditPreviewTerminal::Completed
                        || preview.row_stride_bytes != 0
                    {
                        bail!("semantic Recipe import input preview returned an invalid payload");
                    }
                    let extent = RasterExtent::new(preview.width, preview.height)
                        .context("semantic Recipe import input dimensions are invalid")?;
                    Ok(Some(
                        self.subject_masks
                            .complete_original_space_input_preparation(
                                context.input_session_token,
                                &input_identity,
                                preview.bytes,
                                extent,
                            )?,
                    ))
                })();
                match prepared {
                    Ok(Some(input)) => input,
                    Ok(None) => {
                        let _ = self.subject_masks.abort_original_space_input_preparation(
                            context.input_session_token,
                            &input_identity,
                        );
                        self.subject_masks.finish_job(context.job_token)?;
                        return Ok(SemanticRecipeExecution::Cancelled);
                    }
                    Err(error) => {
                        let _ = self.subject_masks.abort_original_space_input_preparation(
                            context.input_session_token,
                            &input_identity,
                        );
                        return Err(error);
                    }
                }
            }
        };

        let request_id = format!(
            "desktop-semantic-recipe-{}-{}",
            context.job_token, context.generation
        );
        let outcome = self.subject_mask_runtime.stage_semantic_intent(
            self.subject_masks.store(),
            SemanticMaskInvocation {
                request_id: request_id.clone(),
                promotion_id: format!(
                    "photo:{}:recipe-import:{}:{}",
                    context.target.photo_id, context.plan_token, context.item.component_id
                ),
                generation: context.generation,
                photo_id: context.target.photo_id.clone(),
                original_space_input: prepared_input,
                intent: context.item.intent.clone(),
            },
            &cancellation,
        );
        let receipt = match semantic_stage_receipt(outcome) {
            Ok(receipt) => receipt,
            Err(terminal) => {
                self.subject_masks.finish_job(context.job_token)?;
                return Ok(terminal);
            }
        };
        let completion = self
            .subject_masks
            .complete_job(context.job_token, receipt)?;
        match completion {
            SubjectMaskCompletion::Staged {
                request_id: actual_request_id,
                generation,
                proposal_token,
            } => {
                if actual_request_id != request_id || generation != context.generation {
                    let _ = self.subject_masks.discard_proposal(proposal_token);
                    bail!("semantic Recipe import runtime returned a stale identity");
                }
                let preview = self.subject_masks.proposal_preview(proposal_token)?;
                if preview.generation != generation
                    || preview.encoding != SoftMaskEncoding::Gray8Unorm
                {
                    let _ = self.subject_masks.discard_proposal(proposal_token);
                    bail!("semantic Recipe import proposal preview is incompatible");
                }
                let output_extent =
                    RasterExtent::new(RECIPE_IMPORT_PREVIEW_EDGE, RECIPE_IMPORT_PREVIEW_EDGE)?;
                let samples = project_gray8_mask_to_output(
                    &preview.samples,
                    preview.raster_extent,
                    preview.coordinate_extent,
                    context.target.settings.canvas.effective_geometry(),
                    output_extent,
                )
                .context("semantic Recipe import proposal projection is invalid")?;
                Ok(SemanticRecipeExecution::Staged {
                    proposal_token,
                    preview: RecipeImportProposalPreview {
                        width: output_extent.width,
                        height: output_extent.height,
                        samples,
                    },
                })
            }
            SubjectMaskCompletion::Unavailable { reason } => {
                Ok(SemanticRecipeExecution::Unavailable(format!("{reason:?}")))
            }
            SubjectMaskCompletion::Cancelled => Ok(SemanticRecipeExecution::Cancelled),
            SubjectMaskCompletion::Failed { failure } => {
                Ok(SemanticRecipeExecution::Failed(format!("{failure:?}")))
            }
        }
    }

    pub(crate) fn cancel_semantic_recipe_import_item(
        &self,
        plan_token: u64,
        item_id: &str,
        job_token: u64,
    ) -> AnyResult<ffi::FfiRecipeImportItem> {
        self.subject_masks.cancel_job(job_token)?;
        let snapshot = self
            .recipe_imports
            .cancel_item(plan_token, item_id, job_token)?;
        let _ = self.subject_masks.finish_job(job_token);
        recipe_import_item_ffi(snapshot)
    }

    pub(crate) fn accept_semantic_recipe_import_item(
        &self,
        plan_token: u64,
        item_id: &str,
        proposal_token: u64,
        generation: u64,
    ) -> AnyResult<ffi::FfiRecipeImportItem> {
        let leaf =
            self.recipe_imports
                .staged_item(plan_token, item_id, proposal_token, generation)?;
        let definition = self.subject_masks.promote_proposal(
            proposal_token,
            generation,
            SubjectMaskRefinement {
                expansion_percent: leaf.expansion_percent,
                feather_percent: leaf.feather_percent,
                leaf_invert: leaf.leaf_invert,
                semantic_intent: Some(leaf.intent.clone()),
            },
        )?;
        recipe_import_item_ffi(self.recipe_imports.resolve_item(
            plan_token,
            item_id,
            proposal_token,
            generation,
            SemanticLeafResolution {
                item_id: RecipeImportItemId::parse(item_id)?,
                grade_node_id: leaf.grade_node_id,
                component_id: leaf.component_id,
                definition,
            },
        )?)
    }

    pub(crate) fn exclude_semantic_recipe_import_node(
        &self,
        plan_token: u64,
        grade_node_id: &str,
    ) -> AnyResult<ffi::FfiRecipeImportPlan> {
        let (snapshot, jobs, proposals) = self
            .recipe_imports
            .exclude_node(plan_token, grade_node_id)?;
        for job in jobs {
            let _ = self.cancel_subject_mask_job(job);
            let _ = self.subject_masks.finish_job(job);
        }
        for proposal in proposals {
            let _ = self.subject_masks.discard_proposal(proposal);
        }
        recipe_import_plan_ffi(snapshot)
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn finalize_semantic_recipe_import(
        &self,
        plan_token: u64,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        settings: &ffi::FfiEditSettings,
    ) -> AnyResult<ffi::FfiEditSettings> {
        self.validated_photo_source(photo_id, source_path)?;
        let target = recipe_import_target(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            settings,
        )?;
        encode_grade_stack_draft_recipe_v1(self.recipe_imports.finalize(plan_token, &target)?)
    }

    pub(crate) fn close_semantic_recipe_import(&self, plan_token: u64) -> AnyResult<()> {
        let resources = self.recipe_imports.close(plan_token)?;
        for job in resources.job_tokens {
            let _ = self.cancel_subject_mask_job(job);
            let _ = self.subject_masks.finish_job(job);
        }
        for proposal in resources.proposal_tokens {
            let _ = self.subject_masks.discard_proposal(proposal);
        }
        let _ = self
            .subject_masks
            .finish_input_session(resources.input_session_token);
        Ok(())
    }
}

fn recipe_import_target(
    photo_id: &str,
    source_path: &str,
    base_commit_id: &str,
    expected_working_commit_id: &str,
    settings: &ffi::FfiEditSettings,
) -> AnyResult<RecipeImportTargetIdentity> {
    if photo_id.is_empty() || source_path.is_empty() {
        bail!("semantic Recipe import destination is incomplete");
    }
    Ok(RecipeImportTargetIdentity {
        photo_id: photo_id.to_owned(),
        source_path: source_path.to_owned(),
        base_commit_id: base_commit_id.to_owned(),
        expected_working_commit_id: expected_working_commit_id.to_owned(),
        settings: decode_grade_stack_draft_recipe_v1(settings)?,
    })
}

#[derive(Debug)]
enum SemanticRecipeExecution {
    Staged {
        proposal_token: u64,
        preview: RecipeImportProposalPreview,
    },
    NotFound,
    Unavailable(String),
    Cancelled,
    Failed(String),
}

fn semantic_stage_receipt(
    outcome: SemanticMaskStageOutcome,
) -> Result<shadow_core::DerivedRasterStageReceipt, SemanticRecipeExecution> {
    match outcome {
        SemanticMaskStageOutcome::Staged(receipt)
        | SemanticMaskStageOutcome::ProviderUnavailable(receipt)
        | SemanticMaskStageOutcome::ProviderFailed(receipt) => Ok(receipt),
        SemanticMaskStageOutcome::NotFound => Err(SemanticRecipeExecution::NotFound),
        SemanticMaskStageOutcome::Cancelled => Err(SemanticRecipeExecution::Cancelled),
        SemanticMaskStageOutcome::Unavailable(error) => {
            Err(SemanticRecipeExecution::Unavailable(error.to_string()))
        }
    }
}

const fn identity_recipe_import_geometry() -> ffi::FfiPhotoGeometry {
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

fn recipe_import_plan_ffi(
    snapshot: RecipeImportPlanSnapshot,
) -> AnyResult<ffi::FfiRecipeImportPlan> {
    Ok(ffi::FfiRecipeImportPlan {
        plan_token: snapshot.plan_token,
        generation: snapshot.generation,
        label: snapshot.label,
        items: snapshot
            .items
            .into_iter()
            .map(recipe_import_item_ffi)
            .collect::<AnyResult<Vec<_>>>()?,
        nodes: snapshot
            .nodes
            .into_iter()
            .map(|node| ffi::FfiRecipeImportNode {
                grade_node_id: node.grade_node_id.to_string(),
                label: node.label,
                semantic_leaf_count: node.semantic_leaf_count,
                unsupported_managed_leaf_count: node.unsupported_managed_leaf_count,
                excluded: node.excluded,
            })
            .collect(),
    })
}

fn recipe_import_item_ffi(item: RecipeImportItemSnapshot) -> AnyResult<ffi::FfiRecipeImportItem> {
    let (preview_width, preview_height, preview_samples) = item.preview.map_or_else(
        || (0, 0, Vec::new()),
        |preview| (preview.width, preview.height, preview.samples),
    );
    Ok(ffi::FfiRecipeImportItem {
        item_id: item.leaf.item_id.to_string(),
        grade_node_id: item.leaf.grade_node_id.to_string(),
        component_id: item.leaf.component_id.to_string(),
        operation: item.leaf.operation as u8,
        enabled: item.leaf.enabled,
        expansion_percent: item.leaf.expansion_percent,
        feather_percent: item.leaf.feather_percent,
        leaf_invert: item.leaf.leaf_invert,
        semantic_query: item.leaf.intent.query().to_owned(),
        semantic_maximum_regions: item.leaf.intent.maximum_regions(),
        semantic_score_threshold_percent: item.leaf.intent.score_threshold_percent(),
        terminal: match item.terminal {
            RecipeImportItemTerminal::Pending => ffi::FfiRecipeImportItemTerminal::Pending,
            RecipeImportItemTerminal::Running => ffi::FfiRecipeImportItemTerminal::Running,
            RecipeImportItemTerminal::Staged => ffi::FfiRecipeImportItemTerminal::Staged,
            RecipeImportItemTerminal::Completed => ffi::FfiRecipeImportItemTerminal::Completed,
            RecipeImportItemTerminal::NotFound => ffi::FfiRecipeImportItemTerminal::NotFound,
            RecipeImportItemTerminal::Unavailable => ffi::FfiRecipeImportItemTerminal::Unavailable,
            RecipeImportItemTerminal::Cancelled => ffi::FfiRecipeImportItemTerminal::Cancelled,
            RecipeImportItemTerminal::Failed => ffi::FfiRecipeImportItemTerminal::Failed,
        },
        generation: item.generation,
        progress_percent: item.progress_percent,
        detail: item.detail,
        proposal_token: item.proposal_token.unwrap_or_default(),
        preview_width,
        preview_height,
        preview_samples,
    })
}

#[cfg(test)]
mod tests;
