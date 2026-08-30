//! DesktopSession orchestration for adaptive semantic Shadow Recipe import.

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_domain::ShadowRecipeDocument;

use crate::{
    DesktopSession, ffi,
    recipe_import_plan::{RecipeImportItemId, RecipeImportPlan, SemanticLeafResolution},
    recipe_import_service::{
        RecipeImportExecutionContext, RecipeImportItemSnapshot, RecipeImportItemTerminal,
        RecipeImportPlanSnapshot, RecipeImportProposalPreview, RecipeImportTargetIdentity,
    },
    recipe_v1::{decode_grade_stack_draft_recipe_v1, encode_grade_stack_draft_recipe_v1},
    subject_mask_service::SubjectMaskRefinement,
};

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
        let plan = RecipeImportPlan::from_document(&document)?;
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
        let result = self.execute_subject_mask_job(
            &context.target.photo_id,
            &context.target.source_path,
            &semantic_subject_mask_request(&context)?,
        )?;
        let (terminal, detail, proposal_token, preview) = match result.terminal {
            ffi::FfiSubjectMaskTerminal::Staged => (
                RecipeImportItemTerminal::Staged,
                String::new(),
                Some(result.proposal_token),
                Some(RecipeImportProposalPreview {
                    width: result.preview_width,
                    height: result.preview_height,
                    samples: result.preview_samples,
                }),
            ),
            ffi::FfiSubjectMaskTerminal::Unavailable => (
                RecipeImportItemTerminal::Unavailable,
                result.detail,
                None,
                None,
            ),
            ffi::FfiSubjectMaskTerminal::Cancelled => (
                RecipeImportItemTerminal::Cancelled,
                result.detail,
                None,
                None,
            ),
            ffi::FfiSubjectMaskTerminal::Failed
                if result.detail == "semantic query did not match a visible region" =>
            {
                (
                    RecipeImportItemTerminal::NotFound,
                    result.detail,
                    None,
                    None,
                )
            }
            ffi::FfiSubjectMaskTerminal::Failed => {
                (RecipeImportItemTerminal::Failed, result.detail, None, None)
            }
            ffi::FfiSubjectMaskTerminal::PeopleReady => {
                bail!("semantic Recipe import received an invalid people result")
            }
            _ => bail!("semantic Recipe import received an unsupported subject-mask result"),
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

fn semantic_subject_mask_request(
    context: &RecipeImportExecutionContext,
) -> AnyResult<ffi::FfiSubjectMaskRequest> {
    let settings = encode_grade_stack_draft_recipe_v1(context.target.settings.clone())?;
    let target_grade_node_id = settings
        .grade_nodes
        .first()
        .ok_or_else(|| anyhow!("semantic Recipe import destination has no Grade Node"))?
        .grade_node_id
        .clone();
    Ok(ffi::FfiSubjectMaskRequest {
        input_session_token: context.input_session_token,
        job_token: context.job_token,
        generation: context.generation,
        base_commit_id: context.target.base_commit_id.clone(),
        settings,
        target_grade_node_index: 0,
        target_grade_node_id,
        kind: ffi::FfiSubjectMaskKind::SemanticQuery,
        person_index: 0,
        face_region_mask: 0,
        semantic_query: context.item.intent.query().to_owned(),
        semantic_maximum_regions: context.item.intent.maximum_regions(),
        semantic_score_threshold_percent: context.item.intent.score_threshold_percent(),
        points: Vec::new(),
    })
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
