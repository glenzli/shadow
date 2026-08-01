//! Full-detail viewport rendering lifecycle for one desktop session.

use std::sync::{Arc, atomic::Ordering};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_ai::CancellationToken;
use shadow_bridge::{
    DetailSessionRequirements, OpticsSettings, PhotoEditDetailSession, RawDevelopmentPlan,
    photo_provider_version, raw_development_plan_identity,
};
use shadow_catalog::{RepresentationFingerprint, ReviewItemRecord};
use shadow_core::fingerprint_source;
use shadow_domain::RepresentationId;

use super::{
    DesktopSession,
    detail_tile_cache::{CachedDetailSource, EditDetailSessionLookup, cached_detail_tile},
    detail_viewport::{
        MAX_DETAIL_VIEWPORT_SIDE, detail_viewport_rects, validate_detail_viewport_request,
    },
    ffi,
    photo_provider::isolated_edit_raster,
    preview_cache_identity::current_source_environment_cache_identity,
    raw_foundation_render_source::{
        RawFoundationRenderIdentity, RawFoundationRenderSelection, load_raw_foundation_for_render,
        raw_foundation_ready_for_render,
    },
    recipe_v1::{
        ensure_foundation_allows_rgb_fallback, ensure_foundation_development_receipt,
        resolve_recipe_render,
    },
    session_photo_source::{catalog_native_path, ensure_native_decode_is_admitted},
};

#[derive(Debug)]
pub(super) struct CachedEditDetailSession {
    pub(super) representation_id: RepresentationId,
    pub(super) source: RepresentationFingerprint,
    pub(super) source_environment_cache_identity: String,
    pub(super) requested_raw_development_plan_identity: String,
    pub(super) optics: OpticsSettings,
    pub(super) raw_foundation: Option<RawFoundationRenderIdentity>,
    pub(super) session: Arc<CachedDetailSource>,
}

impl DesktopSession {
    pub(crate) fn render_basic_edit_detail_viewport(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditDetailViewportRequest,
    ) -> AnyResult<ffi::FfiEditedDetailViewport> {
        validate_detail_viewport_request(request)?;
        self.ensure_current_edit_detail_render(request.render_token)?;
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let recipe = resolve_recipe_render(
            &self.catalog,
            &self.cache_root,
            photo_id,
            &request.base_commit_id,
            &request.settings,
            request.use_working_recipe,
        )?;
        self.ensure_current_edit_detail_render(request.render_token)?;
        let raw_development_plan = recipe.foundation.detail_plan();
        let requirements = DetailSessionRequirements::for_render_plan(&recipe.plan);
        let native_path = catalog_native_path(&source)?;
        // Full-detail cancellation currently uses the viewport generation
        // gate. Keep the foundation token explicit so this route cannot
        // accidentally inherit preview cancellation or hide its present
        // non-cancellable source-preparation boundary.
        let foundation_cancellation = CancellationToken::default();
        let raw_foundation = raw_foundation_ready_for_render(
            &self.raw_foundations,
            &self.raw_foundation_runtime,
            &native_path,
            source.source,
            recipe.foundation.raw_ai_denoise(),
            &foundation_cancellation,
        )?;
        let session = self.edit_detail_session(
            &source,
            request.render_token,
            raw_development_plan,
            recipe.foundation.optics().clone(),
            raw_foundation.as_ref(),
            requirements,
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
            let rendered =
                cached_detail_tile(&session, &recipe.plan, recipe.snapshot_digest, rect)?;
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

    // Source fingerprinting, cache admission, decoder fallback, and cache
    // publication form one indivisible detail-session preparation transaction.
    #[allow(clippy::too_many_lines)]
    fn edit_detail_session(
        &self,
        source: &ReviewItemRecord,
        render_token: u64,
        raw_development_plan: RawDevelopmentPlan,
        optics: OpticsSettings,
        raw_foundation: Option<&RawFoundationRenderSelection>,
        requirements: DetailSessionRequirements,
    ) -> AnyResult<Arc<CachedDetailSource>> {
        const SOURCE_CHANGED: &str = "full detail source changed since Catalog registration";
        const SOURCE_METADATA_CONTEXT: &str = "read full detail source metadata";
        let native_path = catalog_native_path(source)?;
        let source_environment_cache_identity =
            current_source_environment_cache_identity(&photo_provider_version());
        let requested_raw_development_plan_identity =
            raw_development_plan_identity(raw_development_plan)
                .context("build requested detail RAW-development cache identity")?;
        let raw_foundation_identity = raw_foundation.map(|selection| selection.identity.clone());
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
        if let Some(session) = cached.get_with_requirements(&EditDetailSessionLookup {
            representation_id: source.representation_id,
            source: source.source,
            source_environment_cache_identity: &source_environment_cache_identity,
            requested_raw_development_plan_identity: &requested_raw_development_plan_identity,
            optics: &optics,
            raw_foundation: raw_foundation_identity.as_ref(),
            requirements,
        }) {
            return Ok(session);
        }
        ensure_native_decode_is_admitted(&self.cache_root, &native_path)?;
        let prepared_session = if let Some(selection) = raw_foundation {
            let loaded = load_raw_foundation_for_render(selection, &native_path, source.source)?;
            let prepared = match loaded.staging_manifest_path.as_deref() {
                Some(staging_manifest) => PhotoEditDetailSession::open_with_staged_raw_foundation(
                    &native_path,
                    staging_manifest,
                    raw_development_plan,
                    &loaded.foundation,
                    &optics,
                    requirements,
                ),
                None => PhotoEditDetailSession::open_with_raw_foundation(
                    &native_path,
                    raw_development_plan,
                    &loaded.foundation,
                    &optics,
                    requirements,
                ),
            }
            .context("prepare full detail from verified AI RAW foundation")?;
            ensure_foundation_development_receipt(
                raw_development_plan,
                prepared.raw_pipeline_receipt(),
            )?;
            prepared
        } else {
            match PhotoEditDetailSession::open_with_requirements(
                &native_path,
                raw_development_plan,
                &optics,
                requirements,
            ) {
                Ok(prepared) => {
                    ensure_foundation_development_receipt(
                        raw_development_plan,
                        prepared.raw_pipeline_receipt(),
                    )?;
                    prepared
                }
                Err(public_decoder_error) => {
                    if let Err(policy_error) =
                        ensure_foundation_allows_rgb_fallback(raw_development_plan)
                    {
                        return Err(anyhow!(
                            "{policy_error}; public decoder could not prepare detail for {}: {public_decoder_error}",
                            native_path.display()
                        ));
                    }
                    // Preserve the same safety contract as warm previews. This is an RGB fallback,
                    // so it may not provide native sensor-resolution detail, but it remains fully
                    // editable and never requires a private SDK in the desktop process.
                    let temporary_raster = isolated_edit_raster(
                        &self.cache_root,
                        &native_path,
                        MAX_DETAIL_VIEWPORT_SIDE,
                    )?;
                    let isolated_result = PhotoEditDetailSession::open_with_requirements(
                        &temporary_raster,
                        raw_development_plan,
                        &optics,
                        requirements,
                    );
                    let _ = std::fs::remove_file(&temporary_raster);
                    let prepared = isolated_result.with_context(|| {
                        format!(
                            "public decoder could not prepare detail for {}; isolated decoder fallback also failed: {public_decoder_error}",
                            native_path.display()
                        )
                    })?;
                    ensure_foundation_development_receipt(
                        raw_development_plan,
                        prepared.raw_pipeline_receipt(),
                    )?;
                    prepared
                }
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
            raw_foundation: raw_foundation_identity,
            session: Arc::clone(&prepared),
        });
        Ok(prepared)
    }
}
