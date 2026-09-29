//! Session-local reuse of immutable, decoded edit-preview sources.
//!
//! The render transaction owns cancellation and terminal publication. This
//! module owns only the bounded prepared-source lifecycle: exact request keys,
//! warm lookup, public-to-isolated preparation, concurrent cold-miss
//! convergence, and LRU eviction.

use std::{
    collections::VecDeque,
    path::Path,
    sync::{Arc, Mutex},
    time::Instant,
};

#[cfg(test)]
use std::path::PathBuf;

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_bridge::{
    OpticsSettings, PhotoEditPreviewSession, RawDevelopmentPlan, raw_development_plan_identity,
};
use shadow_catalog::{RepresentationFingerprint, ReviewItemRecord};
use shadow_domain::{RawWhiteBalance, RepresentationId};

use crate::{
    isolated_proxy::{
        configured_helper_path, snapshot_isolated_photo_metadata, stage_isolated_raw_frame,
    },
    photo_provider::isolated_edit_raster,
    preview_cache_identity::requested_raw_development_plan_cache_matches,
    raw_foundation_render_source::{
        RawFoundationRenderIdentity, RawFoundationRenderSelection, load_raw_foundation_for_render,
    },
    recipe_v1::ensure_foundation_development_receipt,
    session_photo_source::{catalog_native_path, ensure_native_decode_is_admitted},
};

#[cfg(test)]
use crate::recipe_v1::ensure_foundation_allows_rgb_fallback;

// Edit navigation retains the previous photo, current photo, and the next
// photo prepared in the background. Two slots made the next preheat evict the
// previous RAW and forced a fresh provider decode on a simple back navigation.
const MAX_WARM_EDIT_PREVIEW_SESSIONS: usize = 3;
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
    raw_foundation_amount_percent: Option<u8>,
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
            && self.raw_foundation_amount_percent == requested.raw_foundation_amount_percent
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

    /// The RAW picker samples the retained CFA using normalized source
    /// coordinates, not pixels from the rendered preview. It can therefore
    /// reuse a source prepared at another presentation edge, while every
    /// source-domain and development-stage input must still match.
    fn shares_raw_white_balance_picker_source(&self, requested: &Self) -> bool {
        let mut requested_fixed = requested.raw_development_plan;
        requested_fixed.white_balance = self.raw_development_plan.white_balance;
        self.representation_id == requested.representation_id
            && self.source == requested.source
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

/// Three-entry MRU of prepared edit-preview sources.
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
    /// Debug-only correlation for one interactive request. It is deliberately
    /// outside the cache key: tracing must never change reuse behavior.
    pub(crate) interactive_timing_token: Option<u64>,
}

impl WarmEditPreviewSessionCache {
    /// Exact-source lookup for transient tools; never prepares or rebinds a source.
    pub(crate) fn get_existing(
        &self,
        request: &WarmEditPreviewSourceRequest<'_>,
    ) -> AnyResult<Option<Arc<PhotoEditPreviewSession>>> {
        let key = warm_preview_session_key(request)?;
        let entries = self
            .entries
            .lock()
            .map_err(|_| anyhow!(CACHE_LOCK_POISONED))?;
        Ok(entries
            .iter()
            .find(|entry| entry.key.matches(&key))
            .map(|entry| Arc::clone(&entry.session)))
    }

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
            source_environment_cache_identity: _,
            raw_foundation,
            interactive_timing_token,
        } = *request;
        // RAW development is immutable prepared-source provenance, not a
        // Recipe color operation. Its requested identity must participate in
        // the key before any warm reuse decision.
        let key = warm_preview_session_key(request)?;

        self.get_or_prepare_with_timing(key, interactive_timing_token, || {
            // A warm hit intentionally returns before path resolution and
            // quarantine admission. Persisted crash evidence blocks reopening
            // the source; it does not invalidate already decoded pixels.
            let native_path = catalog_native_path(source)?;
            ensure_native_decode_is_admitted(runtime_cache_root, &native_path)?;
            match raw_foundation {
                Some(selection) => {
                    let loaded =
                        load_raw_foundation_for_render(selection, &native_path, source.source)?;
                    let prepared = match loaded.staging_manifest_path.as_deref() {
                        Some(staging_manifest) => {
                            PhotoEditPreviewSession::open_with_staged_raw_foundation(
                                &native_path,
                                staging_manifest,
                                max_edge,
                                raw_development_plan,
                                &loaded.foundation,
                                optics,
                            )
                        }
                        None => PhotoEditPreviewSession::open_with_raw_foundation(
                            &native_path,
                            max_edge,
                            raw_development_plan,
                            &loaded.foundation,
                            optics,
                        ),
                    }
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

    /// Samples only a retained source-domain RAW basis. This intentionally
    /// never prepares, stages, decodes, or rebinds a session from a picker
    /// click: the visible preview is the admission boundary for the tool.
    pub(crate) fn pick_raw_white_balance(
        &self,
        request: &WarmEditPreviewSourceRequest<'_>,
        normalized_x: f64,
        normalized_y: f64,
    ) -> AnyResult<Option<(u32, i16)>> {
        let key = warm_preview_session_key(request)?;
        let entries = self
            .entries
            .lock()
            .map_err(|_| anyhow!(CACHE_LOCK_POISONED))?;
        let Some(session) = entries
            .iter()
            .find(|entry| {
                entry.key.shares_raw_white_balance_picker_source(&key)
                    && entry.session.supports_raw_white_balance_picker()
            })
            .map(|entry| Arc::clone(&entry.session))
        else {
            return Ok(None);
        };
        Ok(session.pick_raw_white_balance(normalized_x, normalized_y))
    }

    /// Estimates from only an already-admitted source-domain RAW basis. Like
    /// the picker, the one-shot action is forbidden from preparing, staging,
    /// decoding, or rebinding a session on demand.
    pub(crate) fn auto_raw_white_balance(
        &self,
        request: &WarmEditPreviewSourceRequest<'_>,
    ) -> AnyResult<Option<(u32, i16)>> {
        let key = warm_preview_session_key(request)?;
        let entries = self
            .entries
            .lock()
            .map_err(|_| anyhow!(CACHE_LOCK_POISONED))?;
        let Some(session) = entries
            .iter()
            .find(|entry| {
                entry.key.shares_raw_white_balance_picker_source(&key)
                    && entry.session.supports_raw_white_balance_picker()
            })
            .map(|entry| Arc::clone(&entry.session))
        else {
            return Ok(None);
        };
        Ok(session.auto_raw_white_balance())
    }

    #[cfg(test)]
    fn get_or_prepare_with<Prepare>(
        &self,
        key: WarmEditPreviewSessionKey,
        prepare: Prepare,
    ) -> AnyResult<Arc<PhotoEditPreviewSession>>
    where
        Prepare: FnOnce() -> AnyResult<PhotoEditPreviewSession>,
    {
        self.get_or_prepare_with_timing(key, None, prepare)
    }

    fn get_or_prepare_with_timing<Prepare>(
        &self,
        key: WarmEditPreviewSessionKey,
        interactive_timing_token: Option<u64>,
        prepare: Prepare,
    ) -> AnyResult<Arc<PhotoEditPreviewSession>>
    where
        Prepare: FnOnce() -> AnyResult<PhotoEditPreviewSession>,
    {
        let timing_started = interactive_timing_token.map(|_| Instant::now());
        {
            let mut entries = self
                .entries
                .lock()
                .map_err(|_| anyhow!(CACHE_LOCK_POISONED))?;
            if let Some(session) = take_matching_session(&mut entries, &key)? {
                log_interactive_session_timing(
                    interactive_timing_token,
                    timing_started.as_ref(),
                    "exact-hit",
                );
                return Ok(session);
            }
            if let Some(source) = entries
                .iter()
                .find(|entry| {
                    entry.key.shares_rebindable_raw_source(&key)
                        && match key.raw_foundation_amount_percent {
                            Some(_) => entry.session.supports_raw_foundation_amount_rebinding(),
                            None => entry.session.supports_raw_development_rebinding(),
                        }
                })
                .map(|entry| Arc::clone(&entry.session))
            {
                drop(entries);
                let rebind_route = if key.raw_foundation_amount_percent.is_some() {
                    "raw-foundation-amount-rebind"
                } else {
                    "raw-white-balance-rebind"
                };
                let rebound = Arc::new(match key.raw_foundation_amount_percent {
                    Some(amount_percent) => source
                        .rebind_raw_foundation_amount(key.raw_development_plan, amount_percent)
                        .context("rebind warm preview AI foundation amount")?,
                    None => source
                        .rebind_raw_development_plan(key.raw_development_plan)
                        .context("rebind warm preview RAW white balance")?,
                });
                let mut entries = self
                    .entries
                    .lock()
                    .map_err(|_| anyhow!(CACHE_LOCK_POISONED))?;
                if let Some(session) = take_matching_session(&mut entries, &key)? {
                    log_interactive_session_timing(
                        interactive_timing_token,
                        timing_started.as_ref(),
                        "rebind-raced-exact-hit",
                    );
                    return Ok(session);
                }
                entries.push_front(WarmEditPreviewSessionEntry {
                    key,
                    session: Arc::clone(&rebound),
                });
                entries.truncate(MAX_WARM_EDIT_PREVIEW_SESSIONS);
                log_interactive_session_timing(
                    interactive_timing_token,
                    timing_started.as_ref(),
                    rebind_route,
                );
                return Ok(rebound);
            }
        }

        // Decoder work may be slow and may itself use process-wide provider
        // gates. Never serialize unrelated warm-cache reads behind this mutex.
        let prepared = match prepare() {
            Ok(prepared) => Arc::new(prepared),
            Err(error) => {
                log_interactive_session_timing(
                    interactive_timing_token,
                    timing_started.as_ref(),
                    "cold-prepare-error",
                );
                return Err(error);
            }
        };

        let mut entries = self
            .entries
            .lock()
            .map_err(|_| anyhow!(CACHE_LOCK_POISONED))?;
        if let Some(session) = take_matching_session(&mut entries, &key)? {
            log_interactive_session_timing(
                interactive_timing_token,
                timing_started.as_ref(),
                "cold-prepare-raced-exact-hit",
            );
            return Ok(session);
        }
        entries.push_front(WarmEditPreviewSessionEntry {
            key,
            session: Arc::clone(&prepared),
        });
        entries.truncate(MAX_WARM_EDIT_PREVIEW_SESSIONS);
        log_interactive_session_timing(
            interactive_timing_token,
            timing_started.as_ref(),
            "cold-prepare",
        );
        Ok(prepared)
    }
}

fn log_interactive_session_timing(
    interactive_timing_token: Option<u64>,
    started: Option<&Instant>,
    route: &str,
) {
    if let (Some(token), Some(started)) = (interactive_timing_token, started) {
        eprintln!(
            "shadow.interactive-timing token={token} component=warm-preview-session route={route} elapsed_ms={}",
            started.elapsed().as_millis()
        );
    }
}

fn warm_preview_session_key(
    request: &WarmEditPreviewSourceRequest<'_>,
) -> AnyResult<WarmEditPreviewSessionKey> {
    let requested_raw_development_plan_identity =
        raw_development_plan_identity(request.raw_development_plan)
            .context("build requested preview RAW-development cache identity")?;
    Ok(WarmEditPreviewSessionKey {
        representation_id: request.source.representation_id,
        source: request.source.source,
        max_edge: request.max_edge,
        source_environment_cache_identity: request.source_environment_cache_identity.to_owned(),
        requested_raw_development_plan_identity,
        raw_development_plan: request.raw_development_plan,
        optics: request.optics.clone(),
        raw_foundation: request
            .raw_foundation
            .map(|selection| selection.identity.clone()),
        raw_foundation_amount_percent: request
            .raw_foundation
            .map(RawFoundationRenderSelection::amount_percent),
    })
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
    let public_decoder_error =
        match PhotoEditPreviewSession::open_with_raw_development_plan_and_optics(
            native_path,
            max_edge,
            raw_development_plan,
            optics,
        ) {
            Ok(prepared) => {
                ensure_foundation_development_receipt(
                    raw_development_plan,
                    prepared.raw_pipeline_receipt(),
                )?;
                return Ok(prepared);
            }
            Err(error) => error,
        };

    // A provider-only RAW must acquire its rebindable camera-space basis when
    // the preview opens, not lazily on the first RAW-white-balance drag.  The
    // latter made the gesture block on Provider Host staging and left regular
    // Grade controls behind the same single preview request.  A persisted
    // manual white balance stages the canonical as-shot basis, then performs
    // the requested bind in memory so every subsequent slider value shares
    // that exact source.
    if let Some(helper_path) = configured_helper_path() {
        let staging_root = runtime_cache_root
            .join("decode-helper")
            .join("raw-frame-staging");
        let staged_plan =
            manual_white_balance_base_plan(raw_development_plan).unwrap_or(raw_development_plan);
        let staged_result = (|| -> AnyResult<PhotoEditPreviewSession> {
            // Metadata is cached by source and helper identity. It accompanies
            // the staged RawFrame into the bridge, so the desktop process never
            // reopens a provider-only source merely to compile its DCP and
            // optics preparation.
            let metadata =
                snapshot_isolated_photo_metadata(&helper_path, runtime_cache_root, native_path)
                    .context("snapshot isolated RAW metadata for staged preview preparation")?;
            let staging = stage_isolated_raw_frame(&helper_path, &staging_root, native_path)
                .with_context(|| {
                    format!(
                        "stage provider-neutral RawFrame after public decoder could not prepare {}: {public_decoder_error}",
                        native_path.display()
                    )
                })?;
            let staged =
                PhotoEditPreviewSession::open_with_staged_raw_development_plan_from_metadata(
                    &metadata.metadata,
                    staging.manifest_path(),
                    max_edge,
                    staged_plan,
                    optics,
                )
                .with_context(|| {
                    format!(
                        "prepare rebindable preview from the isolated RawFrame for {}",
                        native_path.display()
                    )
                })?;
            let prepared = if staged_plan == raw_development_plan {
                staged
            } else {
                staged
                    .rebind_raw_development_plan(raw_development_plan)
                    .context("rebind persisted manual Foundation RAW white balance from the staged as-shot source")?
            };
            ensure_foundation_development_receipt(
                raw_development_plan,
                prepared.raw_pipeline_receipt(),
            )?;
            Ok(prepared)
        })();
        match staged_result {
            Ok(prepared) => return Ok(prepared),
            Err(error) if !raw_development_plan.white_balance.is_as_shot() => return Err(error),
            Err(_) => {}
        }
    } else if !raw_development_plan.white_balance.is_as_shot() {
        return Err(anyhow!(
            "manual Foundation RAW white balance requires the isolated Provider Host RawFrame route; public decoder could not prepare {}: {public_decoder_error}",
            native_path.display()
        ));
    }

    // Private providers stay outside the desktop process. The isolated helper
    // produces a short-lived RGB JPEG for the camera-value compatibility case.
    let temporary_raster = isolated_edit_raster(runtime_cache_root, native_path, max_edge)?;
    let isolated_result: AnyResult<PhotoEditPreviewSession> = (|| {
        let prepared = PhotoEditPreviewSession::open_with_raw_development_plan_and_optics(
            &temporary_raster,
            max_edge,
            raw_development_plan,
            optics,
        )?;
        ensure_foundation_development_receipt(
            raw_development_plan,
            prepared.raw_pipeline_receipt(),
        )?;
        Ok(prepared)
    })();
    let _ = std::fs::remove_file(&temporary_raster);
    isolated_result.with_context(|| {
        format!(
            "public decoder could not prepare {}; isolated decoder fallback also failed: {public_decoder_error}",
            native_path.display()
        )
    })
}

fn manual_white_balance_base_plan(
    raw_development_plan: RawDevelopmentPlan,
) -> Option<RawDevelopmentPlan> {
    (!raw_development_plan.white_balance.is_as_shot())
        .then(|| raw_development_plan.with_white_balance(RawWhiteBalance::AsShot))
}

#[cfg(test)]
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
