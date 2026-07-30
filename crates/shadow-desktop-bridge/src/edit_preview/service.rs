//! Complete interactive edit-preview transaction.
//!
//! This owner keeps admission, native cancellation, Recipe and prepared-source
//! acquisition, render policy, the unique terminal claim, optional durable
//! publication, and the final FFI projection in one auditable lifecycle.

use anyhow::{Result as AnyResult, anyhow, bail};
use shadow_bridge::{
    AnalyzedEditPreview, CancellableEditPreview, EditPreviewMaskCoverageRequest,
    OwnedInteractivePreviewFrame, photo_provider_version,
};

use super::{
    OwnedEditedPreview, RecipePreviewStoreRequest, WarmEditPreviewSourceRequest,
    cancelled_edited_preview, completed_edited_preview, store_recipe_preview,
};
use crate::{
    DesktopSession, ffi,
    preview_cache_identity::current_source_environment_cache_identity,
    preview_render_registry::{PreviewAdmission, PreviewRenderRegistryError, PreviewTerminalClaim},
    raw_foundation_render_source::raw_foundation_ready_for_render,
    recipe_v1::{
        bridge_optics_settings, preview_foundation_development_plan, resolve_recipe_render,
    },
    session_photo_source::catalog_native_path,
};

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) enum EditPreviewPolicy {
    Interactive,
    Settled,
    NeutralBefore,
}

impl EditPreviewPolicy {
    fn from_ffi(policy: ffi::FfiEditPreviewPolicy) -> AnyResult<Self> {
        match policy {
            ffi::FfiEditPreviewPolicy::Interactive => Ok(Self::Interactive),
            ffi::FfiEditPreviewPolicy::Settled => Ok(Self::Settled),
            ffi::FfiEditPreviewPolicy::NeutralBefore => Ok(Self::NeutralBefore),
            _ => bail!("unknown edit-preview policy"),
        }
    }

    const fn uses_working_recipe(self) -> bool {
        !matches!(self, Self::NeutralBefore)
    }

    pub(super) const fn requires_analysis(self) -> bool {
        !matches!(self, Self::Interactive)
    }

    const fn admits_durable_cache(self) -> bool {
        matches!(self, Self::Settled)
    }

    pub(super) const fn returns_sensor_diagnostics(self) -> bool {
        !matches!(self, Self::Interactive)
    }
}

const fn admits_recipe_preview_cache(
    policy: EditPreviewPolicy,
    terminal: PreviewTerminalClaim,
) -> bool {
    policy.admits_durable_cache() && matches!(terminal, PreviewTerminalClaim::Completed)
}

fn preview_registry_error(error: &PreviewRenderRegistryError, token: u64) -> anyhow::Error {
    anyhow!("edit preview render token {token} is invalid: {error:?}")
}

fn mask_coverage_request(
    requested: bool,
    target_layer_index: u32,
    mask_selection_revision: u64,
    grade_node_count: usize,
    target_has_mask: bool,
) -> AnyResult<Option<EditPreviewMaskCoverageRequest>> {
    if !requested {
        if target_layer_index != 0 || mask_selection_revision != 0 {
            bail!("unrequested mask coverage must use zero target and revision sentinels");
        }
        return Ok(None);
    }
    let target = usize::try_from(target_layer_index)
        .map_err(|_| anyhow!("mask coverage target does not fit the host address space"))?;
    if target >= grade_node_count {
        bail!("mask coverage target is outside the authored Grade Stack");
    }
    if !target_has_mask {
        return Ok(None);
    }
    Ok(Some(EditPreviewMaskCoverageRequest {
        target_layer_index,
        mask_selection_revision,
    }))
}

enum CompletedEditPreview {
    Interactive(OwnedInteractivePreviewFrame),
    Materialized(Box<AnalyzedEditPreview>),
}

fn cancelled_owned_edited_preview() -> Box<OwnedEditedPreview> {
    OwnedEditedPreview::materialized(cancelled_edited_preview())
}

impl DesktopSession {
    /// Explicit materializing compatibility route.
    ///
    /// The Qt interactive path retains [`OwnedEditedPreview`] instead. Existing
    /// Rust callers and settled-preview consumers may keep using this method
    /// while migration is in progress.
    pub(crate) fn render_basic_edit_preview(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditPreviewRequest,
    ) -> AnyResult<ffi::FfiEditedPreview> {
        self.render_basic_edit_preview_owned(photo_id, source_path, request)
            .and_then(OwnedEditedPreview::into_materialized_projection)
    }

    // Admission, native cancellation, the terminal claim, and publication form
    // one linearized transaction. Splitting that sequence would hide the race
    // invariant this function exists to make auditable.
    #[allow(clippy::too_many_lines)]
    pub(crate) fn render_basic_edit_preview_owned(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditPreviewRequest,
    ) -> AnyResult<Box<OwnedEditedPreview>> {
        let render = (|| -> AnyResult<Box<OwnedEditedPreview>> {
            match self
                .edit_preview_render_tokens
                .admission(request.render_token)
                .map_err(|error| preview_registry_error(&error, request.render_token))?
            {
                PreviewAdmission::Active => {}
                PreviewAdmission::Cancelled => {
                    self.edit_preview_render_tokens
                        .claim_terminal(request.render_token)
                        .map_err(|error| preview_registry_error(&error, request.render_token))?;
                    return Ok(cancelled_owned_edited_preview());
                }
            }
            let native_cancellation = self
                .edit_preview_render_tokens
                .cancellation(request.render_token)
                .map_err(|error| preview_registry_error(&error, request.render_token))?;

            let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
            let policy = EditPreviewPolicy::from_ffi(request.policy)?;
            if request.use_working_recipe != policy.uses_working_recipe() {
                bail!(
                    "edit-preview policy and Recipe source disagree: policy={policy:?}, use_working_recipe={}",
                    request.use_working_recipe
                );
            }
            let mask_coverage = mask_coverage_request(
                request.mask_coverage_requested,
                request.mask_coverage_target_layer_index,
                request.mask_selection_revision,
                request.settings.grade_nodes.len(),
                usize::try_from(request.mask_coverage_target_layer_index)
                    .ok()
                    .and_then(|target| request.settings.grade_nodes.get(target))
                    .is_some_and(|grade_node| grade_node.local_mask_kind != 0),
            )?;
            let source_environment_cache_identity =
                current_source_environment_cache_identity(&photo_provider_version());
            let recipe = resolve_recipe_render(
                &self.catalog,
                &self.cache_root,
                photo_id,
                &request.base_commit_id,
                &request.settings,
                request.use_working_recipe,
            )?;
            let raw_development_plan =
                preview_foundation_development_plan(recipe.raw_white_balance);
            let native_path = catalog_native_path(&source)?;
            let raw_foundation = raw_foundation_ready_for_render(
                &self.raw_foundations,
                &self.raw_foundation_runtime,
                &native_path,
                source.source,
                recipe.raw_ai_denoise,
            )?;
            let optics = bridge_optics_settings(&request.settings.foundation.optics);
            let session =
                self.warm_edit_preview_sessions
                    .get_or_prepare(&WarmEditPreviewSourceRequest {
                        runtime_cache_root: &self.cache_root,
                        source: &source,
                        max_edge: request.max_edge,
                        raw_development_plan,
                        optics: &optics,
                        source_environment_cache_identity: &source_environment_cache_identity,
                        raw_foundation: raw_foundation.as_ref(),
                    })?;
            if self
                .edit_preview_render_tokens
                .admission(request.render_token)
                .map_err(|error| preview_registry_error(&error, request.render_token))?
                == PreviewAdmission::Cancelled
            {
                self.edit_preview_render_tokens
                    .claim_terminal(request.render_token)
                    .map_err(|error| preview_registry_error(&error, request.render_token))?;
                return Ok(cancelled_owned_edited_preview());
            }
            let rendered = match policy {
                EditPreviewPolicy::Interactive => {
                    match session.render_plan_interactive_frame_cancellable(
                        &recipe.plan,
                        mask_coverage,
                        &native_cancellation,
                    )? {
                        CancellableEditPreview::Completed(frame) => {
                            CancellableEditPreview::Completed(CompletedEditPreview::Interactive(
                                frame,
                            ))
                        }
                        CancellableEditPreview::Cancelled => CancellableEditPreview::Cancelled,
                    }
                }
                EditPreviewPolicy::Settled | EditPreviewPolicy::NeutralBefore => {
                    match session.render_plan_with_analysis_and_mask_coverage_cancellable(
                        &recipe.plan,
                        request.jpeg_quality,
                        mask_coverage,
                        &native_cancellation,
                    )? {
                        CancellableEditPreview::Completed(rendered) => {
                            CancellableEditPreview::Completed(CompletedEditPreview::Materialized(
                                Box::new(rendered),
                            ))
                        }
                        CancellableEditPreview::Cancelled => CancellableEditPreview::Cancelled,
                    }
                }
            };
            let rendered = match rendered {
                CancellableEditPreview::Completed(rendered) => rendered,
                CancellableEditPreview::Cancelled => {
                    // Native cancellation is only reachable through the
                    // registry-owned handle. Therefore the host cancellation
                    // must already own the unique terminal claim.
                    match self
                        .edit_preview_render_tokens
                        .claim_terminal(request.render_token)
                        .map_err(|error| preview_registry_error(&error, request.render_token))?
                    {
                        PreviewTerminalClaim::Cancelled => {
                            return Ok(cancelled_owned_edited_preview());
                        }
                        PreviewTerminalClaim::Completed => {
                            bail!(
                                "native edit preview reported cancellation before host cancellation owned render token {}",
                                request.render_token
                            );
                        }
                    }
                }
            };

            // This remains the publication linearization point. Native
            // checkpoints may have completed normally just before a host
            // cancellation wins; the host outcome still suppresses payload
            // inspection, durable caching, and UI publication below.
            let terminal = self
                .edit_preview_render_tokens
                .claim_terminal(request.render_token)
                .map_err(|error| preview_registry_error(&error, request.render_token))?;
            if terminal == PreviewTerminalClaim::Cancelled {
                return Ok(cancelled_owned_edited_preview());
            }

            if admits_recipe_preview_cache(policy, terminal) {
                let CompletedEditPreview::Materialized(rendered) = &rendered else {
                    bail!("settled edit preview has no materialized execution receipt");
                };
                // The on-screen result remains responsive if disk caching is
                // temporarily unavailable. Only a completed settled current
                // Recipe may enter the durable Gallery cache.
                if let Err(error) = store_recipe_preview(
                    &self.catalog,
                    &self.loader,
                    RecipePreviewStoreRequest {
                        representation_id: source.representation_id,
                        expected_source: source.source,
                        proxy: &rendered.proxy,
                        recipe_snapshot_digest: recipe.snapshot_digest,
                        max_edge: request.max_edge,
                        jpeg_quality: request.jpeg_quality,
                        raw_development_plan,
                        raw_pipeline_receipt: session.raw_pipeline_receipt(),
                        edit_execution_receipt: &rendered.execution,
                        source_environment_cache_identity: &source_environment_cache_identity,
                    },
                ) {
                    eprintln!("Shadow: could not cache edited preview: {error:#}");
                }
            }
            match rendered {
                CompletedEditPreview::Interactive(frame) => Ok(OwnedEditedPreview::interactive(
                    frame,
                    session.optics_receipt(),
                )),
                CompletedEditPreview::Materialized(rendered) => {
                    let rendered = *rendered;
                    Ok(OwnedEditedPreview::materialized(completed_edited_preview(
                        rendered.proxy,
                        Some(&rendered.analysis),
                        rendered.mask_coverage,
                        session.optics_receipt(),
                        session.sensor_clipping_mask(),
                        policy,
                    )))
                }
            }
        })();

        match render {
            Ok(preview) => Ok(preview),
            Err(error) => match self
                .edit_preview_render_tokens
                .claim_terminal(request.render_token)
            {
                Ok(PreviewTerminalClaim::Cancelled) => Ok(cancelled_owned_edited_preview()),
                Ok(PreviewTerminalClaim::Completed)
                | Err(PreviewRenderRegistryError::TerminalAlreadyClaimed) => Err(error),
                Err(registry_error) => Err(error.context(preview_registry_error(
                    &registry_error,
                    request.render_token,
                ))),
            },
        }
    }

    pub(crate) fn begin_basic_edit_preview(&self) -> u64 {
        self.edit_preview_render_tokens.begin().unwrap_or(0)
    }

    pub(crate) fn cancel_basic_edit_preview(&self, render_token: u64) -> bool {
        self.edit_preview_render_tokens.cancel(render_token)
    }

    pub(crate) fn claim_basic_edit_preview_terminal(
        &self,
        render_token: u64,
    ) -> AnyResult<ffi::FfiEditPreviewTerminal> {
        match self
            .edit_preview_render_tokens
            .claim_terminal(render_token)
            .map_err(|error| preview_registry_error(&error, render_token))?
        {
            PreviewTerminalClaim::Completed => Ok(ffi::FfiEditPreviewTerminal::Completed),
            PreviewTerminalClaim::Cancelled => Ok(ffi::FfiEditPreviewTerminal::Cancelled),
        }
    }
}

#[cfg(test)]
mod tests;
