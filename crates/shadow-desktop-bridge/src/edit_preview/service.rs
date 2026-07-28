//! Complete interactive edit-preview transaction.
//!
//! This owner keeps admission, native cancellation, Recipe and prepared-source
//! acquisition, render policy, the unique terminal claim, optional durable
//! publication, and the final FFI projection in one auditable lifecycle.

use anyhow::{Result as AnyResult, anyhow, bail};
use shadow_bridge::{CancellableEditPreview, photo_provider_version};

use super::{
    RecipePreviewStoreRequest, cancelled_edited_preview, completed_edited_preview,
    store_recipe_preview,
};
use crate::{
    DesktopSession, ffi,
    preview_cache_identity::current_source_environment_cache_identity,
    preview_render_registry::{PreviewAdmission, PreviewRenderRegistryError, PreviewTerminalClaim},
    recipe_v1::{bridge_optics_settings, resolve_recipe_render},
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

fn preview_registry_error(error: PreviewRenderRegistryError, token: u64) -> anyhow::Error {
    anyhow!("edit preview render token {token} is invalid: {error:?}")
}

impl DesktopSession {
    pub(crate) fn render_basic_edit_preview(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditPreviewRequest,
    ) -> AnyResult<ffi::FfiEditedPreview> {
        let render = (|| -> AnyResult<ffi::FfiEditedPreview> {
            match self
                .edit_preview_render_tokens
                .admission(request.render_token)
                .map_err(|error| preview_registry_error(error, request.render_token))?
            {
                PreviewAdmission::Active => {}
                PreviewAdmission::Cancelled => {
                    self.edit_preview_render_tokens
                        .claim_terminal(request.render_token)
                        .map_err(|error| preview_registry_error(error, request.render_token))?;
                    return Ok(cancelled_edited_preview());
                }
            }
            let native_cancellation = self
                .edit_preview_render_tokens
                .cancellation(request.render_token)
                .map_err(|error| preview_registry_error(error, request.render_token))?;

            let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
            let policy = EditPreviewPolicy::from_ffi(request.policy)?;
            if request.use_working_recipe != policy.uses_working_recipe() {
                bail!(
                    "edit-preview policy and Recipe source disagree: policy={policy:?}, use_working_recipe={}",
                    request.use_working_recipe
                );
            }
            let source_environment_cache_identity =
                current_source_environment_cache_identity(&photo_provider_version());
            let recipe = resolve_recipe_render(
                &self.catalog,
                photo_id,
                &request.base_commit_id,
                &request.settings,
                request.use_working_recipe,
            )?;
            let session = self.warm_edit_preview_sessions.get_or_prepare(
                &self.cache_root,
                &source,
                request.max_edge,
                &bridge_optics_settings(&request.settings.optics),
                &source_environment_cache_identity,
            )?;
            if self
                .edit_preview_render_tokens
                .admission(request.render_token)
                .map_err(|error| preview_registry_error(error, request.render_token))?
                == PreviewAdmission::Cancelled
            {
                self.edit_preview_render_tokens
                    .claim_terminal(request.render_token)
                    .map_err(|error| preview_registry_error(error, request.render_token))?;
                return Ok(cancelled_edited_preview());
            }
            let rendered = match policy {
                EditPreviewPolicy::Interactive => {
                    match session.render_plan_cancellable(
                        &recipe.plan,
                        request.jpeg_quality,
                        &native_cancellation,
                    )? {
                        CancellableEditPreview::Completed(proxy) => {
                            CancellableEditPreview::Completed((proxy, None, None))
                        }
                        CancellableEditPreview::Cancelled => CancellableEditPreview::Cancelled,
                    }
                }
                EditPreviewPolicy::Settled | EditPreviewPolicy::NeutralBefore => {
                    match session.render_plan_with_analysis_cancellable(
                        &recipe.plan,
                        request.jpeg_quality,
                        &native_cancellation,
                    )? {
                        CancellableEditPreview::Completed(rendered) => {
                            CancellableEditPreview::Completed((
                                rendered.proxy,
                                Some(rendered.analysis),
                                Some(rendered.execution),
                            ))
                        }
                        CancellableEditPreview::Cancelled => CancellableEditPreview::Cancelled,
                    }
                }
            };
            let (proxy, analysis, execution) = match rendered {
                CancellableEditPreview::Completed(rendered) => rendered,
                CancellableEditPreview::Cancelled => {
                    // Native cancellation is only reachable through the
                    // registry-owned handle. Therefore the host cancellation
                    // must already own the unique terminal claim.
                    match self
                        .edit_preview_render_tokens
                        .claim_terminal(request.render_token)
                        .map_err(|error| preview_registry_error(error, request.render_token))?
                    {
                        PreviewTerminalClaim::Cancelled => {
                            return Ok(cancelled_edited_preview());
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
                .map_err(|error| preview_registry_error(error, request.render_token))?;
            if terminal == PreviewTerminalClaim::Cancelled {
                return Ok(cancelled_edited_preview());
            }

            if admits_recipe_preview_cache(policy, terminal) {
                let execution = execution
                    .as_ref()
                    .ok_or_else(|| anyhow!("settled edit preview has no execution receipt"))?;
                // The on-screen result remains responsive if disk caching is
                // temporarily unavailable. Only a completed settled current
                // Recipe may enter the durable Gallery cache.
                if let Err(error) = store_recipe_preview(
                    &self.catalog,
                    &self.loader,
                    RecipePreviewStoreRequest {
                        representation_id: source.representation_id,
                        expected_source: source.source,
                        proxy: &proxy,
                        recipe_snapshot_digest: recipe.snapshot_digest,
                        max_edge: request.max_edge,
                        jpeg_quality: request.jpeg_quality,
                        raw_pipeline_receipt: session.raw_pipeline_receipt(),
                        edit_execution_receipt: execution,
                        source_environment_cache_identity: &source_environment_cache_identity,
                    },
                ) {
                    eprintln!("Shadow: could not cache edited preview: {error:#}");
                }
            }
            Ok(completed_edited_preview(
                proxy,
                analysis.as_ref(),
                session.optics_receipt(),
                session.sensor_clipping_mask(),
                policy,
            ))
        })();

        match render {
            Ok(preview) => Ok(preview),
            Err(error) => match self
                .edit_preview_render_tokens
                .claim_terminal(request.render_token)
            {
                Ok(PreviewTerminalClaim::Cancelled) => Ok(cancelled_edited_preview()),
                Ok(PreviewTerminalClaim::Completed)
                | Err(PreviewRenderRegistryError::TerminalAlreadyClaimed) => Err(error),
                Err(registry_error) => {
                    Err(error.context(preview_registry_error(registry_error, request.render_token)))
                }
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
            .map_err(|error| preview_registry_error(error, render_token))?
        {
            PreviewTerminalClaim::Completed => Ok(ffi::FfiEditPreviewTerminal::Completed),
            PreviewTerminalClaim::Cancelled => Ok(ffi::FfiEditPreviewTerminal::Cancelled),
        }
    }
}

#[cfg(test)]
mod tests;
