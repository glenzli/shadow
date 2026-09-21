//! Complete interactive edit-preview transaction.
//!
//! This owner keeps admission, native cancellation, Recipe and prepared-source
//! acquisition, render policy, the unique terminal claim, optional durable
//! publication, and the final FFI projection in one auditable lifecycle.

use std::time::Instant;

use anyhow::{Result as AnyResult, anyhow, bail};
use shadow_bridge::{
    AnalyzedEditPreview, CancellableEditPreview, EditPreviewMaskCoverageRequest,
    OwnedInteractivePreviewFrame, RawPipelineReceipt, photo_provider_version,
};

use super::{
    OwnedEditedPreview, RecipePreviewStoreJob, WarmEditPreviewSourceRequest,
    cancelled_edited_preview, completed_edited_preview, defer_recipe_preview_store,
};
use crate::{
    DesktopSession, ffi,
    preview_cache_identity::current_source_environment_cache_identity,
    preview_render_registry::{PreviewAdmission, PreviewRenderRegistryError, PreviewTerminalClaim},
    raw_foundation_render_source::raw_foundation_ready_for_render,
    recipe_v1::{resolve_composition_before_render, resolve_recipe_render},
    session_photo_source::catalog_native_path,
};

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) enum EditPreviewPolicy {
    Interactive,
    SubjectMaskInput,
    Settled,
    PresentationCommit,
    NeutralBefore,
}

impl EditPreviewPolicy {
    fn from_ffi(policy: ffi::FfiEditPreviewPolicy) -> AnyResult<Self> {
        match policy {
            ffi::FfiEditPreviewPolicy::Interactive => Ok(Self::Interactive),
            ffi::FfiEditPreviewPolicy::Settled => Ok(Self::Settled),
            ffi::FfiEditPreviewPolicy::PresentationCommit => Ok(Self::PresentationCommit),
            ffi::FfiEditPreviewPolicy::NeutralBefore => Ok(Self::NeutralBefore),
            _ => bail!("unknown edit-preview policy"),
        }
    }

    const fn uses_working_recipe(self) -> bool {
        !matches!(self, Self::NeutralBefore)
    }

    pub(super) const fn requires_analysis(self) -> bool {
        !matches!(self, Self::Interactive | Self::SubjectMaskInput)
    }

    const fn admits_durable_cache(self) -> bool {
        matches!(self, Self::Settled | Self::PresentationCommit)
    }

    pub(super) const fn returns_sensor_diagnostics(self) -> bool {
        !matches!(self, Self::Interactive | Self::SubjectMaskInput)
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

fn interactive_timing_enabled() -> bool {
    std::env::var("SHADOW_INTERACTIVE_TIMING").is_ok_and(|value| value == "1")
}

fn log_interactive_bridge_timing(token: u64, started: &Instant, stage: &str) {
    eprintln!(
        "shadow.interactive-timing token={token} component=bridge stage={stage} elapsed_ms={}",
        started.elapsed().as_millis()
    );
}

/// Records only route provenance while the explicitly enabled interactive timing trace is active.
///
/// This deliberately omits source identities, cache keys, fallback diagnostics, and image data.
/// It lets a local debug session establish whether a gesture reused a staged `RawFrame`, a public
/// decoded raster, or a provider-processed compatibility source before we alter RAW semantics.
fn log_interactive_raw_route(token: u64, receipt: &RawPipelineReceipt) {
    eprintln!(
        "shadow.interactive-raw-route token={token} path={:?} requested_highlight={:?} effective_highlight={:?} raw_frame_schema_version={} raw_developer_version={} provider_id={:?} provider_version={:?}",
        receipt.path,
        receipt.requested_plan.highlight_recovery,
        receipt.effective_plan.highlight_recovery,
        receipt.raw_frame_schema_version,
        receipt.raw_developer_version,
        receipt.source_provider_id,
        receipt.source_provider_version,
    );
}

fn mask_coverage_request(
    requested: bool,
    target_layer_index: u32,
    component_requested: bool,
    target_component_index: u32,
    mask_selection_revision: u64,
    grade_node_count: usize,
    target_component_count: usize,
) -> AnyResult<Option<EditPreviewMaskCoverageRequest>> {
    if !requested {
        if target_layer_index != 0
            || component_requested
            || target_component_index != 0
            || mask_selection_revision != 0
        {
            bail!("unrequested mask coverage must use zero target and revision sentinels");
        }
        return Ok(None);
    }
    let target = usize::try_from(target_layer_index)
        .map_err(|_| anyhow!("mask coverage target does not fit the host address space"))?;
    if target >= grade_node_count {
        bail!("mask coverage target is outside the authored Grade Stack");
    }
    if target_component_count == 0 {
        return Ok(None);
    }
    let target_component_index = if component_requested {
        let component = usize::try_from(target_component_index)
            .map_err(|_| anyhow!("mask coverage component does not fit the host address space"))?;
        if component >= target_component_count {
            bail!("mask coverage component is outside the authored node mask");
        }
        Some(target_component_index)
    } else {
        if target_component_index != 0 {
            bail!("final mask coverage must use the zero component sentinel");
        }
        None
    };
    Ok(Some(EditPreviewMaskCoverageRequest {
        target_layer_index,
        target_component_index,
        mask_selection_revision,
    }))
}

enum CompletedEditPreview {
    Interactive(OwnedInteractivePreviewFrame),
    Encoded(shadow_domain::ProxyPayload),
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

    /// Resolves a RAW neutral from the exact warm source already admitted for
    /// the visible edit preview. A picker click is never allowed to reopen a
    /// RAW, stage a Provider Host frame, or start a fresh preview render.
    pub(crate) fn pick_raw_white_balance(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditPreviewRequest,
        normalized_x: f64,
        normalized_y: f64,
    ) -> AnyResult<ffi::FfiRawWhiteBalancePickerResult> {
        let unavailable = || ffi::FfiRawWhiteBalancePickerResult {
            available: false,
            temperature_kelvin: 5_500,
            tint: 0,
        };
        if !(0.0..=1.0).contains(&normalized_x) || !(0.0..=1.0).contains(&normalized_y) {
            return Ok(unavailable());
        }
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
            &self.cache_root,
            photo_id,
            &request.base_commit_id,
            &request.settings,
            request.use_working_recipe,
        )?;
        // Foundation camera-RGB bases intentionally do not claim to be a CFA
        // picker source. The cache lookup therefore uses the same explicit
        // `None` representation the ordinary RAW preview was admitted with.
        if recipe.foundation.raw_ai_denoise().is_enabled() {
            return Ok(unavailable());
        }
        let result = self.warm_edit_preview_sessions.pick_raw_white_balance(
            &WarmEditPreviewSourceRequest {
                runtime_cache_root: &self.cache_root,
                source: &source,
                max_edge: request.max_edge,
                raw_development_plan: recipe.foundation.preview_plan(),
                optics: recipe.foundation.optics(),
                source_environment_cache_identity: &source_environment_cache_identity,
                raw_foundation: None,
                interactive_timing_token: None,
            },
            normalized_x,
            normalized_y,
        )?;
        Ok(match result {
            Some((temperature_kelvin, tint)) => ffi::FfiRawWhiteBalancePickerResult {
                available: true,
                temperature_kelvin,
                tint,
            },
            None => unavailable(),
        })
    }

    /// Runs one bounded neutral estimate over the exact warm CFA source used
    /// by the visible preview. It never opens a source or persists an Auto
    /// mode; the caller receives one ordinary temperature/tint value.
    pub(crate) fn auto_raw_white_balance(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditPreviewRequest,
    ) -> AnyResult<ffi::FfiRawWhiteBalancePickerResult> {
        let unavailable = || ffi::FfiRawWhiteBalancePickerResult {
            available: false,
            temperature_kelvin: 5_500,
            tint: 0,
        };
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
            &self.cache_root,
            photo_id,
            &request.base_commit_id,
            &request.settings,
            request.use_working_recipe,
        )?;
        if recipe.foundation.raw_ai_denoise().is_enabled() {
            return Ok(unavailable());
        }
        let result = self.warm_edit_preview_sessions.auto_raw_white_balance(
            &WarmEditPreviewSourceRequest {
                runtime_cache_root: &self.cache_root,
                source: &source,
                max_edge: request.max_edge,
                raw_development_plan: recipe.foundation.preview_plan(),
                optics: recipe.foundation.optics(),
                source_environment_cache_identity: &source_environment_cache_identity,
                raw_foundation: None,
                interactive_timing_token: None,
            },
        )?;
        Ok(match result {
            Some((temperature_kelvin, tint)) => ffi::FfiRawWhiteBalancePickerResult {
                available: true,
                temperature_kelvin,
                tint,
            },
            None => unavailable(),
        })
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
        let policy = EditPreviewPolicy::from_ffi(request.policy)?;
        self.render_basic_edit_preview_owned_with_policy(
            photo_id,
            source_path,
            request,
            policy,
            &[],
        )
    }

    /// Produces the exact identity-geometry JPEG consumed by the local
    /// subject-mask runtime without settled-only analysis or durable Recipe
    /// preview admission.
    pub(crate) fn render_subject_mask_input_preview(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditPreviewRequest,
    ) -> AnyResult<ffi::FfiEditedPreview> {
        self.render_basic_edit_preview_owned_with_policy(
            photo_id,
            source_path,
            request,
            EditPreviewPolicy::SubjectMaskInput,
            &[],
        )
        .and_then(OwnedEditedPreview::into_materialized_projection)
    }

    pub(crate) fn render_basic_edit_preview_owned_with_policy(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditPreviewRequest,
        policy: EditPreviewPolicy,
        candidate_masks: &[ffi::FfiAutoStartMask],
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
            let foundation_cancellation = self
                .edit_preview_render_tokens
                .foundation_cancellation(request.render_token)
                .map_err(|error| preview_registry_error(&error, request.render_token))?;

            let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
            let interactive_timing =
                matches!(policy, EditPreviewPolicy::Interactive) && interactive_timing_enabled();
            let interactive_started = interactive_timing.then(Instant::now);
            if request.use_working_recipe != policy.uses_working_recipe() {
                bail!(
                    "edit-preview policy and Recipe source disagree: policy={policy:?}, use_working_recipe={}",
                    request.use_working_recipe
                );
            }
            let mask_coverage = mask_coverage_request(
                request.mask_coverage_requested,
                request.mask_coverage_target_layer_index,
                request.mask_coverage_component_requested,
                request.mask_coverage_target_component_index,
                request.mask_selection_revision,
                request.settings.grade_nodes.len(),
                usize::try_from(request.mask_coverage_target_layer_index)
                    .ok()
                    .and_then(|target| request.settings.grade_nodes.get(target))
                    .map_or(0, |grade_node| grade_node.local_mask_components.len()),
            )?;
            let source_environment_cache_identity =
                current_source_environment_cache_identity(&photo_provider_version());
            let recipe = if !candidate_masks.is_empty() {
                if policy != EditPreviewPolicy::SubjectMaskInput {
                    bail!("candidate masks require a transient preview");
                }
                self.resolve_auto_start_render(photo_id, request, candidate_masks)?
            } else if matches!(policy, EditPreviewPolicy::NeutralBefore) {
                resolve_composition_before_render(
                    &self.catalog,
                    &self.cache_root,
                    photo_id,
                    &request.settings,
                )?
            } else {
                resolve_recipe_render(
                    &self.catalog,
                    &self.cache_root,
                    photo_id,
                    &request.base_commit_id,
                    &request.settings,
                    request.use_working_recipe,
                )?
            };
            let raw_development_plan = recipe.foundation.preview_plan();
            let native_path = catalog_native_path(&source)?;
            let raw_foundation = raw_foundation_ready_for_render(
                &self.raw_foundations,
                &self.raw_foundation_runtime,
                &native_path,
                source.source,
                recipe.foundation.raw_ai_denoise(),
                &foundation_cancellation,
            )?;
            if let Some(started) = interactive_started.as_ref() {
                log_interactive_bridge_timing(request.render_token, started, "source-ready");
            }
            // A superseded RAW-white-balance gesture must not begin a new
            // rebind/session build after its upstream admission work is done.
            // Native rebinding itself remains the declared cancellation seam;
            // this guard eliminates queued stale requests before they cross it.
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
            let session =
                self.warm_edit_preview_sessions
                    .get_or_prepare(&WarmEditPreviewSourceRequest {
                        runtime_cache_root: &self.cache_root,
                        source: &source,
                        max_edge: request.max_edge,
                        raw_development_plan,
                        optics: recipe.foundation.optics(),
                        source_environment_cache_identity: &source_environment_cache_identity,
                        raw_foundation: raw_foundation.as_ref(),
                        interactive_timing_token: interactive_timing
                            .then_some(request.render_token),
                    })?;
            let level_zero_output_dimensions = recipe
                .plan
                .geometry
                .output_dimensions(session.level_zero_dimensions())?;
            if let Some(started) = interactive_started.as_ref() {
                log_interactive_bridge_timing(request.render_token, started, "session-ready");
                log_interactive_raw_route(request.render_token, session.raw_pipeline_receipt());
            }
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
                EditPreviewPolicy::SubjectMaskInput => {
                    match session.render_plan_cancellable(
                        &recipe.plan,
                        request.jpeg_quality,
                        &native_cancellation,
                    )? {
                        CancellableEditPreview::Completed(proxy) => {
                            CancellableEditPreview::Completed(CompletedEditPreview::Encoded(proxy))
                        }
                        CancellableEditPreview::Cancelled => CancellableEditPreview::Cancelled,
                    }
                }
                EditPreviewPolicy::Settled
                | EditPreviewPolicy::PresentationCommit
                | EditPreviewPolicy::NeutralBefore => {
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
            if let Some(started) = interactive_started.as_ref() {
                log_interactive_bridge_timing(request.render_token, started, "native-frame-ready");
            }

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
                // Ordinary completed frames are already authoritative on
                // screen, so their rebuildable Gallery cache work stays in
                // the bounded background queue. PresentationCommit is the
                // explicit exception handled below.
                let store_job = RecipePreviewStoreJob {
                    catalog: self.catalog.clone(),
                    loader: self.loader.clone(),
                    representation_id: source.representation_id,
                    expected_source: source.source,
                    proxy: rendered.proxy.clone(),
                    recipe_snapshot_digest: recipe.snapshot_digest,
                    max_edge: request.max_edge,
                    jpeg_quality: request.jpeg_quality,
                    raw_development_plan,
                    raw_pipeline_receipt: session.raw_pipeline_receipt().clone(),
                    edit_execution_receipt: rendered.execution.clone(),
                    source_environment_cache_identity,
                };
                if matches!(policy, EditPreviewPolicy::PresentationCommit) {
                    // Returning from Precision is a presentation boundary, not
                    // ordinary rebuildable cache maintenance. This render is
                    // already running off the Qt thread, so complete the exact
                    // blob + Catalog transaction before the desktop refreshes
                    // Library and makes the new working Recipe observable.
                    store_job.store()?;
                } else {
                    let _ = defer_recipe_preview_store(store_job);
                }
            }
            match rendered {
                CompletedEditPreview::Interactive(frame) => Ok(OwnedEditedPreview::interactive(
                    frame,
                    level_zero_output_dimensions,
                    session.optics_receipt(),
                )),
                CompletedEditPreview::Encoded(proxy) => {
                    Ok(OwnedEditedPreview::materialized(completed_edited_preview(
                        proxy,
                        level_zero_output_dimensions,
                        None,
                        None,
                        session.optics_receipt(),
                        session.sensor_clipping_mask(),
                        policy,
                    )))
                }
                CompletedEditPreview::Materialized(rendered) => {
                    let rendered = *rendered;
                    Ok(OwnedEditedPreview::materialized(completed_edited_preview(
                        rendered.proxy,
                        level_zero_output_dimensions,
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
