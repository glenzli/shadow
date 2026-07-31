//! Session-local reuse of immutable, decoded edit-preview sources.
//!
//! The render transaction owns cancellation and terminal publication. This
//! module owns only the bounded prepared-source lifecycle: exact request keys,
//! warm lookup, public-to-isolated preparation, concurrent cold-miss
//! convergence, and LRU eviction.

use std::{
    collections::VecDeque,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_bridge::{
    OpticsSettings, PhotoEditPreviewSession, RawDevelopmentPlan, raw_development_plan_identity,
};
use shadow_catalog::{RepresentationFingerprint, ReviewItemRecord};
use shadow_domain::RepresentationId;

use crate::{
    photo_provider::isolated_edit_raster,
    preview_cache_identity::requested_raw_development_plan_cache_matches,
    raw_foundation_render_source::{
        RawFoundationRenderIdentity, RawFoundationRenderSelection, load_raw_foundation_for_render,
    },
    recipe_v1::{ensure_foundation_allows_rgb_fallback, ensure_foundation_development_receipt},
    session_photo_source::{catalog_native_path, ensure_native_decode_is_admitted},
};

const MAX_WARM_EDIT_PREVIEW_SESSIONS: usize = 2;
const CACHE_LOCK_POISONED: &str = "edit preview session cache lock is poisoned";

#[derive(Debug, Clone)]
struct WarmEditPreviewSessionKey {
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    max_edge: u32,
    source_environment_cache_identity: String,
    /// This is the caller's requested plan, not the provider's effective plan.
    /// Reusing an effective-plan match would attach the earlier request's
    /// immutable provenance receipt to a different request.
    requested_raw_development_plan_identity: String,
    raw_development_plan: RawDevelopmentPlan,
    optics: OpticsSettings,
    raw_foundation: Option<RawFoundationRenderIdentity>,
}

impl WarmEditPreviewSessionKey {
    fn matches(&self, requested: &Self) -> bool {
        self.representation_id == requested.representation_id
            && self.source == requested.source
            && self.max_edge == requested.max_edge
            && self.source_environment_cache_identity == requested.source_environment_cache_identity
            && requested_raw_development_plan_cache_matches(
                &self.requested_raw_development_plan_identity,
                &requested.requested_raw_development_plan_identity,
            )
            && self.raw_development_plan == requested.raw_development_plan
            && self.optics == requested.optics
            && self.raw_foundation == requested.raw_foundation
    }

    fn shares_rebindable_raw_source(&self, requested: &Self) -> bool {
        let mut requested_fixed = requested.raw_development_plan;
        requested_fixed.white_balance = self.raw_development_plan.white_balance;
        self.representation_id == requested.representation_id
            && self.source == requested.source
            && self.max_edge == requested.max_edge
            && self.source_environment_cache_identity == requested.source_environment_cache_identity
            && self.raw_development_plan == requested_fixed
            && self.optics == requested.optics
            && self.raw_foundation == requested.raw_foundation
    }
}

#[derive(Debug)]
struct WarmEditPreviewSessionEntry {
    key: WarmEditPreviewSessionKey,
    session: Arc<PhotoEditPreviewSession>,
}

/// Two-entry MRU of prepared edit-preview sources.
///
/// Preparation deliberately runs without the container mutex. A second exact
/// lookup after preparation makes concurrent cold misses converge on the one
/// session admitted to the cache.
#[derive(Debug, Default)]
pub(crate) struct WarmEditPreviewSessionCache {
    entries: Mutex<VecDeque<WarmEditPreviewSessionEntry>>,
}

#[derive(Debug)]
pub(crate) struct WarmEditPreviewSourceRequest<'request> {
    pub(crate) runtime_cache_root: &'request Path,
    pub(crate) source: &'request ReviewItemRecord,
    pub(crate) max_edge: u32,
    pub(crate) raw_development_plan: RawDevelopmentPlan,
    pub(crate) optics: &'request OpticsSettings,
    pub(crate) source_environment_cache_identity: &'request str,
    pub(crate) raw_foundation: Option<&'request RawFoundationRenderSelection>,
}

impl WarmEditPreviewSessionCache {
    pub(crate) fn get_or_prepare(
        &self,
        request: &WarmEditPreviewSourceRequest<'_>,
    ) -> AnyResult<Arc<PhotoEditPreviewSession>> {
        let WarmEditPreviewSourceRequest {
            runtime_cache_root,
            source,
            max_edge,
            raw_development_plan,
            optics,
            source_environment_cache_identity,
            raw_foundation,
        } = *request;
        // RAW development is immutable prepared-source provenance, not a
        // Recipe color operation. Its requested identity must participate in
        // the key before any warm reuse decision.
        let requested_raw_development_plan_identity =
            raw_development_plan_identity(raw_development_plan)
                .context("build requested preview RAW-development cache identity")?;
        let key = WarmEditPreviewSessionKey {
            representation_id: source.representation_id,
            source: source.source,
            max_edge,
            source_environment_cache_identity: source_environment_cache_identity.to_owned(),
            requested_raw_development_plan_identity,
            raw_development_plan,
            optics: optics.clone(),
            raw_foundation: raw_foundation.map(|selection| selection.identity.clone()),
        };

        self.get_or_prepare_with(key, || {
            // A warm hit intentionally returns before path resolution and
            // quarantine admission. Persisted crash evidence blocks reopening
            // the source; it does not invalidate already decoded pixels.
            let native_path = catalog_native_path(source)?;
            ensure_native_decode_is_admitted(runtime_cache_root, &native_path)?;
            match raw_foundation {
                Some(selection) => {
                    let loaded =
                        load_raw_foundation_for_render(selection, &native_path, source.source)?;
                    let prepared = PhotoEditPreviewSession::open_with_raw_foundation(
                        &native_path,
                        max_edge,
                        raw_development_plan,
                        &loaded.foundation,
                        optics,
                    )
                    .context("prepare preview from verified AI RAW foundation")?;
                    ensure_foundation_development_receipt(
                        raw_development_plan,
                        prepared.raw_pipeline_receipt(),
                    )?;
                    Ok(prepared)
                }
                None => prepare_preview_session(
                    runtime_cache_root,
                    &native_path,
                    max_edge,
                    raw_development_plan,
                    optics,
                ),
            }
        })
    }

    fn get_or_prepare_with<Prepare>(
        &self,
        key: WarmEditPreviewSessionKey,
        prepare: Prepare,
    ) -> AnyResult<Arc<PhotoEditPreviewSession>>
    where
        Prepare: FnOnce() -> AnyResult<PhotoEditPreviewSession>,
    {
        {
            let mut entries = self
                .entries
                .lock()
                .map_err(|_| anyhow!(CACHE_LOCK_POISONED))?;
            if let Some(session) = take_matching_session(&mut entries, &key)? {
                return Ok(session);
            }
            if let Some(source) = entries
                .iter()
                .find(|entry| {
                    entry.key.shares_rebindable_raw_source(&key)
                        && entry.session.supports_raw_development_rebinding()
                })
                .map(|entry| Arc::clone(&entry.session))
            {
                drop(entries);
                let rebound = Arc::new(
                    source
                        .rebind_raw_development_plan(key.raw_development_plan)
                        .context("rebind warm preview RAW white balance")?,
                );
                let mut entries = self
                    .entries
                    .lock()
                    .map_err(|_| anyhow!(CACHE_LOCK_POISONED))?;
                if let Some(session) = take_matching_session(&mut entries, &key)? {
                    return Ok(session);
                }
                entries.push_front(WarmEditPreviewSessionEntry {
                    key,
                    session: Arc::clone(&rebound),
                });
                entries.truncate(MAX_WARM_EDIT_PREVIEW_SESSIONS);
                return Ok(rebound);
            }
        }

        // Decoder work may be slow and may itself use process-wide provider
        // gates. Never serialize unrelated warm-cache reads behind this mutex.
        let prepared = Arc::new(prepare()?);

        let mut entries = self
            .entries
            .lock()
            .map_err(|_| anyhow!(CACHE_LOCK_POISONED))?;
        if let Some(session) = take_matching_session(&mut entries, &key)? {
            return Ok(session);
        }
        entries.push_front(WarmEditPreviewSessionEntry {
            key,
            session: Arc::clone(&prepared),
        });
        entries.truncate(MAX_WARM_EDIT_PREVIEW_SESSIONS);
        Ok(prepared)
    }
}

fn take_matching_session(
    entries: &mut VecDeque<WarmEditPreviewSessionEntry>,
    key: &WarmEditPreviewSessionKey,
) -> AnyResult<Option<Arc<PhotoEditPreviewSession>>> {
    let Some(index) = entries.iter().position(|entry| entry.key.matches(key)) else {
        return Ok(None);
    };
    let entry = entries
        .remove(index)
        .ok_or_else(|| anyhow!("matched edit preview session disappeared"))?;
    let session = Arc::clone(&entry.session);
    entries.push_front(entry);
    Ok(Some(session))
}

fn prepare_preview_session(
    runtime_cache_root: &Path,
    native_path: &Path,
    max_edge: u32,
    raw_development_plan: RawDevelopmentPlan,
    optics: &OpticsSettings,
) -> AnyResult<PhotoEditPreviewSession> {
    prepare_preview_session_with_routes(
        runtime_cache_root,
        native_path,
        max_edge,
        raw_development_plan,
        optics,
        |path, max_edge, raw_development_plan, optics| {
            PhotoEditPreviewSession::open_with_raw_development_plan_and_optics(
                path,
                max_edge,
                raw_development_plan,
                optics,
            )
            .map_err(Into::into)
        },
        isolated_edit_raster,
    )
}

#[allow(clippy::too_many_arguments)]
fn prepare_preview_session_with_routes<Open, Isolate>(
    runtime_cache_root: &Path,
    native_path: &Path,
    max_edge: u32,
    raw_development_plan: RawDevelopmentPlan,
    optics: &OpticsSettings,
    mut open: Open,
    isolate: Isolate,
) -> AnyResult<PhotoEditPreviewSession>
where
    Open: FnMut(
        &Path,
        u32,
        RawDevelopmentPlan,
        &OpticsSettings,
    ) -> AnyResult<PhotoEditPreviewSession>,
    Isolate: FnOnce(&Path, &Path, u32) -> AnyResult<PathBuf>,
{
    let public_decoder_error = match open(native_path, max_edge, raw_development_plan, optics) {
        Ok(prepared) => {
            ensure_foundation_development_receipt(
                raw_development_plan,
                prepared.raw_pipeline_receipt(),
            )?;
            return Ok(prepared);
        }
        Err(error) => error,
    };
    if let Err(policy_error) = ensure_foundation_allows_rgb_fallback(raw_development_plan) {
        return Err(anyhow!(
            "{policy_error}; public decoder could not prepare {}: {public_decoder_error}",
            native_path.display()
        ));
    }

    // Private providers stay outside the desktop process. The isolated helper
    // produces a short-lived RGB JPEG which re-enters the normal public raster
    // edit path, so all adjustments continue to execute in the parent.
    let temporary_raster = isolate(runtime_cache_root, native_path, max_edge)?;
    let isolated_result =
        open(&temporary_raster, max_edge, raw_development_plan, optics).and_then(|prepared| {
            ensure_foundation_development_receipt(
                raw_development_plan,
                prepared.raw_pipeline_receipt(),
            )?;
            Ok(prepared)
        });
    // Prepared sessions retain decoded pixels rather than an open descriptor.
    // Cleanup is best-effort but happens after both successful and failed
    // public-raster opens.
    let _ = std::fs::remove_file(&temporary_raster);
    isolated_result.with_context(|| {
        format!(
            "public decoder could not prepare {}; isolated decoder fallback also failed: {public_decoder_error}",
            native_path.display()
        )
    })
}

#[cfg(test)]
mod tests;
