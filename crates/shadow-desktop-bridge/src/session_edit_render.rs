//! Interactive preview and detail rendering lifecycle for one desktop session.

use std::sync::{Arc, atomic::Ordering};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_bridge::{
    AdjustmentRenderPlan, CancellableEditPreview, EditPreviewExecutionReceipt, OpticsSettings,
    PhotoEditDetailSession, PhotoEditPreviewSession, RawDevelopmentPlan, RawPipelineReceipt,
    edit_preview_generator_implementation_identity, photo_provider_version,
    raw_development_plan_identity,
};
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, RecordCachedArtifact, RepresentationFingerprint,
    ReviewItemRecord,
};
use shadow_core::fingerprint_source;
use shadow_domain::{
    PhotoId, PreviewByteOrder, PreviewCodec, ProxyPayload, RecipeCommitId, RepresentationId,
};

use super::{
    DesktopSession, catalog_native_path, current_time_ms,
    detail_tile_cache::{CachedDetailSource, cached_detail_tile},
    detail_viewport::{
        MAX_DETAIL_VIEWPORT_SIDE, detail_viewport_rects, validate_detail_viewport_request,
    },
    encode_hex, ensure_native_decode_is_admitted, ffi,
    photo_provider::isolated_edit_raster,
    preview_cache_identity::{
        EDIT_PREVIEW_GENERATOR_ID, current_source_environment_cache_identity,
        edit_preview_generator_version, prepared_edit_execution_cache_identity,
        prepared_raw_pipeline_cache_identity,
    },
    preview_render_registry::{PreviewAdmission, PreviewRenderRegistryError, PreviewTerminalClaim},
    recipe_v1::{
        bridge_optics_settings, compile_recipe_render_plan, grade_stack_recipe_v1_snapshot,
        preview_grade_stack_draft_recipe_v1,
    },
};

#[derive(Debug)]
pub(super) struct CachedEditPreviewSession {
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    max_edge: u32,
    source_environment_cache_identity: String,
    /// The plan the caller asked for, used to find a reusable session before a decode. The
    /// provider's effective plan remains on the prepared session's immutable receipt: do not
    /// use it as this cache key, because it belongs to a potentially different request.
    requested_raw_development_plan_identity: String,
    optics: OpticsSettings,
    session: Arc<PhotoEditPreviewSession>,
}

#[derive(Debug)]
pub(super) struct CachedEditDetailSession {
    pub(super) representation_id: RepresentationId,
    pub(super) source: RepresentationFingerprint,
    pub(super) source_environment_cache_identity: String,
    pub(super) requested_raw_development_plan_identity: String,
    pub(super) optics: OpticsSettings,
    pub(super) session: Arc<CachedDetailSource>,
}

struct RecipePreviewCacheRequest<'a> {
    recipe_snapshot_digest: [u8; 32],
    max_edge: u32,
    jpeg_quality: u8,
    raw_pipeline_receipt: &'a RawPipelineReceipt,
    edit_execution_receipt: &'a EditPreviewExecutionReceipt,
    source_environment_cache_identity: &'a str,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) enum EditPreviewPolicy {
    Interactive,
    Settled,
    NeutralBefore,
}

impl EditPreviewPolicy {
    pub(crate) fn from_ffi(policy: ffi::FfiEditPreviewPolicy) -> AnyResult<Self> {
        match policy {
            ffi::FfiEditPreviewPolicy::Interactive => Ok(Self::Interactive),
            ffi::FfiEditPreviewPolicy::Settled => Ok(Self::Settled),
            ffi::FfiEditPreviewPolicy::NeutralBefore => Ok(Self::NeutralBefore),
            _ => bail!("unknown edit-preview policy"),
        }
    }

    pub(crate) const fn uses_working_recipe(self) -> bool {
        !matches!(self, Self::NeutralBefore)
    }

    pub(crate) const fn requires_analysis(self) -> bool {
        !matches!(self, Self::Interactive)
    }

    pub(crate) const fn admits_durable_cache(self) -> bool {
        matches!(self, Self::Settled)
    }

    pub(crate) const fn returns_sensor_diagnostics(self) -> bool {
        !matches!(self, Self::Interactive)
    }
}

fn preview_registry_error(error: PreviewRenderRegistryError, token: u64) -> anyhow::Error {
    anyhow!("edit preview render token {token} is invalid: {error:?}")
}

pub(crate) const fn admits_recipe_preview_cache(
    policy: EditPreviewPolicy,
    terminal: PreviewTerminalClaim,
) -> bool {
    policy.admits_durable_cache() && matches!(terminal, PreviewTerminalClaim::Completed)
}

fn cancelled_edited_preview() -> ffi::FfiEditedPreview {
    ffi::FfiEditedPreview {
        terminal: ffi::FfiEditPreviewTerminal::Cancelled,
        width: 0,
        height: 0,
        bytes: Vec::new(),
        sensor_clipping_available: false,
        sensor_clipping_width: 0,
        sensor_clipping_height: 0,
        sensor_clipping_mask: Vec::new(),
        sensor_highlight_clipped_pixels: 0,
        sensor_shadow_clipped_pixels: 0,
        analysis_available: false,
        analysis_version: String::new(),
        analysis_width: 0,
        analysis_height: 0,
        red_histogram: Vec::new(),
        green_histogram: Vec::new(),
        blue_histogram: Vec::new(),
        luma_histogram: Vec::new(),
        below_zero_samples: Vec::new(),
        above_one_samples: Vec::new(),
        hdr_headroom_bins: Vec::new(),
        hdr_headroom_pixels: 0,
        hdr_peak_headroom_ev: 0.0,
        pixel_count: 0,
        shadow_clipped_pixels: 0,
        highlight_clipped_pixels: 0,
        optics_status: String::new(),
        optics_provider_id: String::new(),
        optics_provider_version: String::new(),
        optics_camera_profile: String::new(),
        optics_lens_profile: String::new(),
        optics_distortion_available: false,
        optics_tca_available: false,
        optics_vignetting_available: false,
        optics_applied_distortion: false,
        optics_applied_tca: false,
        optics_applied_vignetting: false,
        optics_vignetting_used_distance_fallback: false,
        optics_applied_scaling: false,
    }
}

// A prepared session owns an immutable receipt for one particular user request. Even if a
// provider adjusted that request to the same effective source plan as a later request, this
// session cannot stand in for the later request without rewriting its provenance. Reusing only
// an identical request keeps plan negotiation and audit history exact.
pub(crate) fn requested_raw_development_plan_cache_matches(
    cached_requested_identity: &str,
    requested_identity: &str,
) -> bool {
    cached_requested_identity == requested_identity
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
            let (plan, recipe_identity) = self.basic_edit_render_plan_with_identity(
                photo_id,
                &request.base_commit_id,
                &request.settings,
                request.use_working_recipe,
            )?;
            let session = self.edit_preview_session(
                &source,
                request.max_edge,
                bridge_optics_settings(&request.settings.optics),
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
                        &plan,
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
                        &plan,
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
                if let Err(error) = self.cache_rendered_recipe_preview(
                    &source,
                    &proxy,
                    RecipePreviewCacheRequest {
                        recipe_snapshot_digest: recipe_identity,
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
            let optics = session.optics_receipt();
            let sensor_clipping = session.sensor_clipping_mask();
            let return_sensor_diagnostics = policy.returns_sensor_diagnostics();
            let analysis_available = analysis.is_some();
            debug_assert_eq!(analysis_available, policy.requires_analysis());
            Ok(ffi::FfiEditedPreview {
                terminal: ffi::FfiEditPreviewTerminal::Completed,
                width: proxy.dimensions.width,
                height: proxy.dimensions.height,
                bytes: proxy.bytes,
                sensor_clipping_available: return_sensor_diagnostics && sensor_clipping.available,
                sensor_clipping_width: if return_sensor_diagnostics {
                    sensor_clipping.dimensions.width
                } else {
                    0
                },
                sensor_clipping_height: if return_sensor_diagnostics {
                    sensor_clipping.dimensions.height
                } else {
                    0
                },
                sensor_clipping_mask: if return_sensor_diagnostics {
                    sensor_clipping.samples.clone()
                } else {
                    Vec::new()
                },
                sensor_highlight_clipped_pixels: if return_sensor_diagnostics {
                    sensor_clipping.highlight_pixel_count
                } else {
                    0
                },
                sensor_shadow_clipped_pixels: if return_sensor_diagnostics {
                    sensor_clipping.shadow_pixel_count
                } else {
                    0
                },
                analysis_available,
                analysis_version: analysis
                    .as_ref()
                    .map_or_else(String::new, |value| value.version.clone()),
                analysis_width: analysis
                    .as_ref()
                    .map_or(0, |value| value.sample_dimensions.width),
                analysis_height: analysis
                    .as_ref()
                    .map_or(0, |value| value.sample_dimensions.height),
                red_histogram: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.red.to_vec()),
                green_histogram: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.green.to_vec()),
                blue_histogram: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.blue.to_vec()),
                luma_histogram: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.luma.to_vec()),
                below_zero_samples: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.below_zero_samples.to_vec()),
                above_one_samples: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.above_one_samples.to_vec()),
                hdr_headroom_bins: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.hdr_headroom_bins.to_vec()),
                hdr_headroom_pixels: analysis
                    .as_ref()
                    .map_or(0, |value| value.hdr_headroom_pixels),
                hdr_peak_headroom_ev: analysis
                    .as_ref()
                    .map_or(0.0, |value| value.hdr_peak_headroom_ev),
                pixel_count: analysis.as_ref().map_or(0, |value| value.pixel_count),
                shadow_clipped_pixels: analysis
                    .as_ref()
                    .map_or(0, |value| value.shadow_clipped_pixels),
                highlight_clipped_pixels: analysis
                    .as_ref()
                    .map_or(0, |value| value.highlight_clipped_pixels),
                optics_status: optics.status.clone(),
                optics_provider_id: optics.provider_id.clone(),
                optics_provider_version: optics.provider_version.clone(),
                optics_camera_profile: optics.camera_profile.clone(),
                optics_lens_profile: optics.lens_profile.clone(),
                optics_distortion_available: optics.distortion_available,
                optics_tca_available: optics.tca_available,
                optics_vignetting_available: optics.vignetting_available,
                optics_applied_distortion: optics.applied_distortion,
                optics_applied_tca: optics.applied_tca,
                optics_applied_vignetting: optics.applied_vignetting,
                optics_vignetting_used_distance_fallback: optics.vignetting_used_distance_fallback,
                optics_applied_scaling: optics.applied_scaling,
            })
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

    pub(crate) fn render_basic_edit_detail_viewport(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditDetailViewportRequest,
    ) -> AnyResult<ffi::FfiEditedDetailViewport> {
        validate_detail_viewport_request(request)?;
        self.ensure_current_edit_detail_render(request.render_token)?;
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let (plan, recipe_identity) = self.basic_edit_render_plan_with_identity(
            photo_id,
            &request.base_commit_id,
            &request.settings,
            request.use_working_recipe,
        )?;
        self.ensure_current_edit_detail_render(request.render_token)?;
        let session = self.edit_detail_session(
            &source,
            request.render_token,
            bridge_optics_settings(&request.settings.optics),
        )?;
        self.ensure_current_edit_detail_render(request.render_token)?;
        let full_dimensions = session.session.dimensions();
        let rects = detail_viewport_rects(
            full_dimensions,
            request.center_x,
            request.center_y,
            request.viewport_width,
            request.viewport_height,
            request.tile_side,
        )?;
        let mut tiles = Vec::with_capacity(rects.len());
        for rect in rects {
            self.ensure_current_edit_detail_render(request.render_token)?;
            let rendered = cached_detail_tile(&session, &plan, recipe_identity, rect)?;
            self.ensure_current_edit_detail_render(request.render_token)?;
            tiles.push(rendered);
        }
        Ok(ffi::FfiEditedDetailViewport {
            full_width: full_dimensions.width,
            full_height: full_dimensions.height,
            retained_bytes: session.session.retained_bytes(),
            tiles,
        })
    }

    pub(crate) fn begin_basic_edit_detail(&self) -> u64 {
        // A 64-bit process-lifetime counter cannot wrap in any realistic UI
        // session. SeqCst keeps the cross-language cancellation contract easy
        // to audit: every later request is visible to every tile worker.
        self.edit_detail_render_token.fetch_add(1, Ordering::SeqCst) + 1
    }

    pub(crate) fn ensure_current_edit_detail_render(&self, render_token: u64) -> AnyResult<()> {
        if render_token == 0 || self.edit_detail_render_token.load(Ordering::SeqCst) != render_token
        {
            bail!("full detail render was superseded by a newer viewport or Recipe");
        }
        Ok(())
    }

    pub(crate) fn basic_edit_render_plan_with_identity(
        &self,
        photo_id: PhotoId,
        base_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        use_working_recipe: bool,
    ) -> AnyResult<(AdjustmentRenderPlan, [u8; 32])> {
        let grade_stack = preview_grade_stack_draft_recipe_v1(settings, use_working_recipe)?;
        // Sliders and their immutable base commit travel as one render
        // generation. Never resolve the movable working ref here: it may have
        // advanced while this worker was queued, which would create a hybrid
        // Recipe that never existed in version history.
        let working_commit = if use_working_recipe && !base_commit_id.is_empty() {
            let commit_id: RecipeCommitId = base_commit_id
                .parse()
                .with_context(|| format!("parse preview base commit id {base_commit_id}"))?;
            Some(
                self.catalog
                    .recipe_commit(photo_id, commit_id)?
                    .ok_or_else(|| {
                        anyhow!("preview base Recipe commit {commit_id} is unavailable")
                    })?,
            )
        } else {
            None
        };
        let template = working_commit
            .as_ref()
            .map(|record| record.commit.snapshot());
        let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, template)?;
        let identity = shadow_domain::canonical_recipe_snapshot_digest(&snapshot)
            .context("serialize exact detail Recipe cache identity")?;
        Ok((compile_recipe_render_plan(&snapshot)?, identity))
    }

    fn edit_preview_session(
        &self,
        source: &ReviewItemRecord,
        max_edge: u32,
        optics: OpticsSettings,
        source_environment_cache_identity: &str,
    ) -> AnyResult<Arc<PhotoEditPreviewSession>> {
        // The plan is source-development provenance, not a color node. Include its canonical
        // identity in the in-memory key before deciding an immutable warm proxy is reusable.
        // This prevents a later fast/high-quality or DNG-policy choice from silently sharing a
        // raster produced under today's canonical preview plan.
        let raw_development_plan = RawDevelopmentPlan::preview();
        let requested_raw_development_plan_identity =
            raw_development_plan_identity(raw_development_plan)
                .context("build requested preview RAW-development cache identity")?;
        {
            let mut sessions = self
                .edit_preview_sessions
                .lock()
                .map_err(|_| anyhow!("edit preview session cache lock is poisoned"))?;
            if let Some(index) = sessions.iter().position(|entry| {
                entry.representation_id == source.representation_id
                    && entry.source == source.source
                    && entry.max_edge == max_edge
                    && entry.source_environment_cache_identity
                        == source_environment_cache_identity
                    // A provider may later adjust request A to effective plan B. The prepared
                    // pixels could be reusable for a separate request B, but its receipt would
                    // still describe A; returning it here would lie about the user's request.
                    // Keep session reuse keyed by the requested plan until source pixels and
                    // per-request provenance are independently cacheable objects.
                    && requested_raw_development_plan_cache_matches(
                        &entry.requested_raw_development_plan_identity,
                        &requested_raw_development_plan_identity,
                    )
                    && entry.optics == optics
            }) {
                let entry = sessions
                    .remove(index)
                    .ok_or_else(|| anyhow!("matched edit preview session disappeared"))?;
                let session = Arc::clone(&entry.session);
                sessions.push_front(entry);
                return Ok(session);
            }
        }

        let native_path = catalog_native_path(source)?;
        ensure_native_decode_is_admitted(&self.cache_root, &native_path)?;
        let prepared = match PhotoEditPreviewSession::open_with_raw_development_plan_and_optics(
            &native_path,
            max_edge,
            raw_development_plan,
            &optics,
        ) {
            Ok(prepared) => prepared,
            Err(public_decoder_error) => {
                // The desktop host deliberately runs only public decoders in-process. If one
                // cannot prepare this source, give the isolated helper a chance to use an
                // installed private provider, then feed the resulting RGB JPEG back through
                // the normal public raster edit pipeline. The helper owns the risky native
                // boundary; the parent still owns every adjustment and never loads that SDK.
                let temporary_raster =
                    isolated_edit_raster(&self.cache_root, &native_path, max_edge)?;
                let isolated_result =
                    PhotoEditPreviewSession::open_with_raw_development_plan_and_optics(
                        &temporary_raster,
                        max_edge,
                        raw_development_plan,
                        &optics,
                    );
                let _ = std::fs::remove_file(&temporary_raster);
                isolated_result.with_context(|| {
                    format!(
                        "public decoder could not prepare {}; isolated decoder fallback also failed: {public_decoder_error}",
                        native_path.display()
                    )
                })?
            }
        };
        let prepared = Arc::new(prepared);
        let mut sessions = self
            .edit_preview_sessions
            .lock()
            .map_err(|_| anyhow!("edit preview session cache lock is poisoned"))?;
        if let Some(entry) = sessions.iter().find(|entry| {
            entry.representation_id == source.representation_id
                && entry.source == source.source
                && entry.max_edge == max_edge
                && entry.source_environment_cache_identity == source_environment_cache_identity
                && requested_raw_development_plan_cache_matches(
                    &entry.requested_raw_development_plan_identity,
                    &requested_raw_development_plan_identity,
                )
                && entry.optics == optics
        }) {
            return Ok(Arc::clone(&entry.session));
        }
        sessions.push_front(CachedEditPreviewSession {
            representation_id: source.representation_id,
            source: source.source,
            max_edge,
            source_environment_cache_identity: source_environment_cache_identity.to_owned(),
            requested_raw_development_plan_identity,
            optics,
            session: Arc::clone(&prepared),
        });
        sessions.truncate(2);
        Ok(prepared)
    }

    /// Persists a rendered edit preview with both source and Recipe
    /// provenance. A gallery query will use it only when the current working
    /// ref names this exact snapshot; a stale slider task can therefore leave
    /// an unused blob but can never paint an old grade over a newer edit.
    fn cache_rendered_recipe_preview(
        &self,
        source: &ReviewItemRecord,
        proxy: &ProxyPayload,
        request: RecipePreviewCacheRequest<'_>,
    ) -> AnyResult<()> {
        let raw_pipeline = prepared_raw_pipeline_cache_identity(request.raw_pipeline_receipt)
            .context("identify prepared Recipe-preview source pipeline")?;
        let edit_execution = prepared_edit_execution_cache_identity(request.edit_execution_receipt)
            .context("identify completed Recipe-preview edit/display execution")?;
        let raw_plan_identity = raw_development_plan_identity(RawDevelopmentPlan::preview())
            .context("build Recipe-preview RAW-development cache identity")?;
        let variant_key = format!(
            "shadow-recipe-preview:jpeg-{}-q{}-444-v1;{raw_plan_identity};\
             pipeline={};execution={};recipe={}",
            request.max_edge,
            request.jpeg_quality,
            raw_pipeline.component(),
            edit_execution.component(),
            encode_hex(&request.recipe_snapshot_digest)
        );
        let blob = self
            .loader
            .store_bytes(&proxy.bytes)
            .context("store rendered Recipe preview blob")?;
        self.catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: source.representation_id,
                expected_source: source.source,
                artifact: CachedArtifact {
                    role: CachedArtifactRole::RecipePreview,
                    variant_key,
                    generator_id: EDIT_PREVIEW_GENERATOR_ID.to_owned(),
                    generator_version: edit_preview_generator_version(
                        request.source_environment_cache_identity,
                        &edit_preview_generator_implementation_identity(),
                    ),
                    recipe_snapshot_digest: Some(request.recipe_snapshot_digest),
                    provider_preview_id: None,
                    blob_algorithm: blob.digest.algorithm().to_owned(),
                    blob_digest: *blob.digest.as_bytes(),
                    blob_byte_len: blob.byte_len,
                    codec: PreviewCodec::Jpeg,
                    byte_order: PreviewByteOrder::NotApplicable,
                    dimensions: proxy.dimensions,
                    bits_per_channel: proxy.bits_per_channel,
                    channels: proxy.channels,
                    created_at_ms: current_time_ms()?,
                },
            })
            .context("record rendered Recipe preview provenance")?;
        Ok(())
    }

    fn edit_detail_session(
        &self,
        source: &ReviewItemRecord,
        render_token: u64,
        optics: OpticsSettings,
    ) -> AnyResult<Arc<CachedDetailSource>> {
        const SOURCE_CHANGED: &str = "full detail source changed since Catalog registration";
        const SOURCE_METADATA_CONTEXT: &str = "read full detail source metadata";
        let native_path = catalog_native_path(source)?;
        let raw_development_plan = RawDevelopmentPlan::detail();
        let source_environment_cache_identity =
            current_source_environment_cache_identity(&photo_provider_version());
        let requested_raw_development_plan_identity =
            raw_development_plan_identity(raw_development_plan)
                .context("build requested detail RAW-development cache identity")?;
        let current_source = fingerprint_source(&native_path).context(SOURCE_METADATA_CONTEXT)?;
        if current_source != source.source {
            bail!(SOURCE_CHANGED);
        }
        // One mutex is also the full-decode admission gate. Holding it across
        // preparation prevents concurrent cold requests from materializing
        // multiple hundreds-of-MiB sources. Completed sources enter a bounded
        // LRU so switching among recently edited photos does not decode again.
        let mut cached = self
            .edit_detail_sessions
            .lock()
            .map_err(|_| anyhow!("edit detail session cache lock is poisoned"))?;
        // A newer request may have arrived while this worker waited for the
        // single cold-decode gate. Refuse stale work before opening the source router.
        self.ensure_current_edit_detail_render(render_token)?;
        if let Some(session) = cached.get(
            source.representation_id,
            source.source,
            &source_environment_cache_identity,
            &requested_raw_development_plan_identity,
            &optics,
        ) {
            return Ok(session);
        }
        ensure_native_decode_is_admitted(&self.cache_root, &native_path)?;
        let prepared_session =
            match PhotoEditDetailSession::open_with_raw_development_plan_and_optics(
                &native_path,
                raw_development_plan,
                &optics,
            ) {
                Ok(prepared) => prepared,
                Err(public_decoder_error) => {
                    // Preserve the same safety contract as warm previews. This is an RGB fallback,
                    // so it may not provide native sensor-resolution detail, but it remains fully
                    // editable and never requires a private SDK in the desktop process.
                    let temporary_raster = isolated_edit_raster(
                        &self.cache_root,
                        &native_path,
                        MAX_DETAIL_VIEWPORT_SIDE,
                    )?;
                    let isolated_result =
                        PhotoEditDetailSession::open_with_raw_development_plan_and_optics(
                            &temporary_raster,
                            raw_development_plan,
                            &optics,
                        );
                    let _ = std::fs::remove_file(&temporary_raster);
                    isolated_result.with_context(|| {
                    format!(
                        "public decoder could not prepare detail for {}; isolated decoder fallback also failed: {public_decoder_error}",
                        native_path.display()
                    )
                })?
                }
            };
        let prepared = Arc::new(CachedDetailSource::new(prepared_session));
        let decoded_source = fingerprint_source(&native_path).context(SOURCE_METADATA_CONTEXT)?;
        if decoded_source != source.source {
            bail!(SOURCE_CHANGED);
        }
        cached.insert(CachedEditDetailSession {
            representation_id: source.representation_id,
            source: source.source,
            source_environment_cache_identity,
            requested_raw_development_plan_identity,
            optics,
            session: Arc::clone(&prepared),
        });
        Ok(prepared)
    }
}
