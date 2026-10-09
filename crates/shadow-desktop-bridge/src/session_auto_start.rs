//! Transient, exact-raster starting-point previews and one working-Recipe apply.
//! Candidate bytes never enter the durable preview cache. Only apply promotes
//! their move-only authority; cancel retains the original working head.
use crate::session_edit_history::WORKING_RECIPE_REF;
use crate::{
    DesktopSession,
    edit_preview::{EditPreviewPolicy, OwnedEditedPreview},
    ffi,
    recipe_v1::{
        ResolvedRecipeRender, decode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
        resolve_recipe_render,
    },
    session_subject_mask::append_subject_mask_component,
    subject_mask_service::SubjectMaskRefinement,
    wall_clock::current_time_ms,
};
use anyhow::{Context, Result, bail};
use shadow_ai::SoftMaskEncoding;
use shadow_bridge::{AdjustmentLocalMask, AdjustmentRasterMaskEncoding, AdjustmentRenderOperation};
use shadow_catalog::{CommitRecipe, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget};
use shadow_domain::{
    EntityId, MaskComponentOperation, PhotoId, RecipeCommit, RecipeCommitId, RecipeId,
};

fn validate_bindings(
    settings: &ffi::FfiEditSettings,
    masks: &[ffi::FfiAutoStartMask],
) -> Result<()> {
    if masks.len() > 4 {
        bail!("automatic starting point accepts at most four skin masks");
    }
    let mut ids = std::collections::HashSet::new();
    let mut tokens = std::collections::HashSet::new();
    for binding in masks {
        let node = settings
            .grade_nodes
            .iter()
            .find(|node| node.grade_node_id == binding.node_id)
            .context("candidate mask target is missing")?;
        if binding.proposal_token == 0
            || !ids.insert(&binding.node_id)
            || !tokens.insert(binding.proposal_token)
            || !node.local_mask_components.is_empty()
            || node.local_mask_invert
            || !node.shared_layer_id.is_empty()
            || !node.shared_revision_id.is_empty()
        {
            bail!("candidate masks require unique, new, unmasked nodes");
        }
    }
    Ok(())
}

impl DesktopSession {
    pub(crate) fn render_auto_start_preview(
        &self,
        photo: &str,
        source: &str,
        request: &ffi::FfiEditPreviewRequest,
        masks: &[ffi::FfiAutoStartMask],
    ) -> Result<ffi::FfiEditedPreview> {
        self.render_basic_edit_preview_owned_with_policy(
            photo,
            source,
            request,
            EditPreviewPolicy::SubjectMaskInput,
            masks,
        )
        .and_then(OwnedEditedPreview::into_materialized_projection)
    }

    pub(crate) fn resolve_auto_start_render(
        &self,
        photo: PhotoId,
        request: &ffi::FfiEditPreviewRequest,
        masks: &[ffi::FfiAutoStartMask],
    ) -> Result<ResolvedRecipeRender> {
        for binding in masks {
            self.subject_masks
                .validate_proposal_photo(binding.proposal_token, photo)?;
        }
        resolve_candidate_render(
            &self.catalog,
            &self.cache_root,
            photo,
            request,
            masks,
            |binding| {
                Ok(self
                    .subject_masks
                    .proposal_preview(binding.proposal_token)?)
            },
        )
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn apply_auto_start(
        &self,
        photo: &str,
        source: &str,
        base: &str,
        expected: &str,
        expected_variant: &str,
        settings: &ffi::FfiEditSettings,
        masks: &[ffi::FfiAutoStartMask],
    ) -> Result<ffi::FfiPhotoEditState> {
        let (photo_id, validated_source) = self.validated_photo_source(photo, source)?;
        self.require_active_photo_variant(photo_id, expected_variant)?;
        // Repeat the caller's captured expectation in the atomic publication;
        // reading the active Variant again would admit a same-head switch.
        let variant = expected_variant.parse()?;
        let expected = if expected.is_empty() {
            None
        } else {
            Some(expected.parse::<RecipeCommitId>()?)
        };
        if self
            .catalog
            .recipe_ref(photo_id, WORKING_RECIPE_REF)?
            .map(|head| head.commit_id)
            != expected
        {
            bail!("photo changed after automatic analysis; prepare a new suggestion");
        }
        let base_record = if base.is_empty() {
            None
        } else {
            Some(
                self.catalog
                    .recipe_commit(photo_id, base.parse()?)?
                    .context("automatic starting-point base is unavailable")?,
            )
        };
        validate_bindings(settings, masks)?;
        // Validate every candidate before consuming any move-only authority.
        for binding in masks {
            self.subject_masks
                .validate_proposal_photo(binding.proposal_token, photo_id)?;
            if self
                .subject_masks
                .proposal_preview(binding.proposal_token)?
                .generation
                != binding.generation
            {
                bail!("stale starting-point skin mask");
            }
        }
        let mut stack = decode_grade_stack_draft_recipe_v1(settings)?;
        for binding in masks {
            let mask = self.subject_masks.promote_proposal(
                binding.proposal_token,
                binding.generation,
                SubjectMaskRefinement {
                    expansion_percent: 0,
                    feather_percent: 0,
                    leaf_invert: false,
                    semantic_intent: None,
                },
            )?;
            let target = stack
                .grade_nodes
                .iter_mut()
                .find(|n| n.recipe_v1_identity.grade_node_id.to_string() == binding.node_id)
                .context("candidate apply target missing")?;
            append_subject_mask_component(target, mask, MaskComponentOperation::Base)?;
        }
        let snapshot = grade_stack_recipe_v1_snapshot(
            &stack,
            base_record.as_ref().map(|r| r.commit.snapshot()),
        )?;
        let (recipe_id, parents) = base_record.as_ref().map_or_else(
            || (RecipeId::new_v7(), Vec::new()),
            |r| (r.commit.recipe_id(), vec![r.commit.id()]),
        );
        // Unlike autosave rebasing, an AI proposal must never overwrite a newer
        // working head. All nodes and masks publish in one strict CAS transaction.
        self.catalog.commit_recipe_for_variant(
            &CommitRecipe {
                photo_id,
                commit: RecipeCommit::new(
                    RecipeCommitId::new_v7(),
                    recipe_id,
                    parents,
                    snapshot,
                    None,
                    current_time_ms()?,
                )?,
                update_refs: vec![RecipeRefTarget {
                    name: WORKING_RECIPE_REF.to_owned(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(
                        expected.map_or(RecipeRefExpectation::Missing, RecipeRefExpectation::At),
                    ),
                }],
            },
            variant,
        )?;
        self.photo_edit_state_for(photo_id, &validated_source.location.display_path)
    }
}

fn resolve_candidate_render(
    catalog: &shadow_catalog::CatalogHandle,
    cache_root: &std::path::Path,
    photo: PhotoId,
    request: &ffi::FfiEditPreviewRequest,
    masks: &[ffi::FfiAutoStartMask],
    mut load: impl FnMut(
        &ffi::FfiAutoStartMask,
    ) -> Result<crate::subject_mask_service::SubjectMaskProposalPreview>,
) -> Result<ResolvedRecipeRender> {
    validate_bindings(&request.settings, masks)?;
    let mut settings = request.settings.clone();
    // Force ordinary layer boundaries without inventing a persisted mask.
    // Restore exact authored opacity before execution below.
    for binding in masks {
        settings
            .grade_nodes
            .iter_mut()
            .find(|n| n.grade_node_id == binding.node_id)
            .context("candidate target disappeared")?
            .opacity = 0.999;
    }
    let mut recipe = resolve_recipe_render(
        catalog,
        cache_root,
        photo,
        &request.base_commit_id,
        &settings,
        true,
    )?;
    let mut identity = blake3::Hasher::new();
    identity.update(&recipe.snapshot_digest);
    identity.update(b"shadow-auto-start-preview-v1");
    for binding in masks {
        let preview = load(binding)?;
        if preview.generation != binding.generation {
            bail!("stale starting-point skin mask");
        }
        let boundary_id = format!("local-mask-layer-start:{}", binding.node_id);
        let boundary = recipe
            .plan
            .nodes
            .iter_mut()
            .find(|n| n.node_id == boundary_id)
            .context("candidate layer boundary missing")?;
        let AdjustmentRenderOperation::LocalMaskLayerStart { opacity, mask } =
            &mut boundary.operation
        else {
            bail!("candidate target is not a layer boundary");
        };
        *opacity = request
            .settings
            .grade_nodes
            .iter()
            .find(|n| n.grade_node_id == binding.node_id)
            .context("candidate target missing")?
            .opacity;
        identity.update(binding.node_id.as_bytes());
        identity.update(&opacity.to_le_bytes());
        identity.update(&preview.raster_extent.width.to_le_bytes());
        identity.update(&preview.raster_extent.height.to_le_bytes());
        identity.update(&preview.coordinate_extent.width.to_le_bytes());
        identity.update(&preview.coordinate_extent.height.to_le_bytes());
        identity.update(&[match preview.encoding {
            SoftMaskEncoding::Gray8Unorm => 0,
            SoftMaskEncoding::Gray16Float => 1,
        }]);
        identity.update(&preview.samples);
        *mask = Some(AdjustmentLocalMask::ManagedRaster {
            raster_width: preview.raster_extent.width,
            raster_height: preview.raster_extent.height,
            coordinate_width: preview.coordinate_extent.width,
            coordinate_height: preview.coordinate_extent.height,
            encoding: match preview.encoding {
                SoftMaskEncoding::Gray8Unorm => AdjustmentRasterMaskEncoding::Gray8,
                SoftMaskEncoding::Gray16Float => AdjustmentRasterMaskEncoding::Gray16Float,
            },
            samples: preview.samples,
            expansion: 0.0,
            feather: 0.0,
            invert: false,
        });
    }
    recipe.snapshot_digest = *identity.finalize().as_bytes();
    recipe.plan.validate()?;
    Ok(recipe)
}

#[cfg(test)]
mod tests;
