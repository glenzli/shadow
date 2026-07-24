//! Bounded, Recipe-aware full-detail tiles layered over one prepared source.

use super::*;

// Keep the decoded full-resolution source and the processed display tiles as
// two distinct caches. The source is expensive RAW development state; the
// tiles are bounded, Recipe-specific RGB8 results that make panning over an
// already inspected region immediate without pinning an entire developed
// image for every photo.
#[derive(Debug)]
pub(super) struct CachedDetailSource {
    pub(super) session: PhotoEditDetailSession,
    tiles: Mutex<DetailTileCache>,
}

impl CachedDetailSource {
    pub(super) fn new(session: PhotoEditDetailSession) -> Self {
        Self {
            session,
            tiles: Mutex::new(DetailTileCache::default()),
        }
    }

    fn resident_bytes(&self) -> u64 {
        let source_bytes = self.session.retained_bytes();
        let tile_bytes = self
            .tiles
            .lock()
            .map_or(0, |cache| u64::try_from(cache.bytes).unwrap_or(u64::MAX));
        source_bytes.saturating_add(tile_bytes)
    }
}

/// Memory-budgeted LRU of recently developed full-resolution edit sources.
///
/// Entries are keyed by source identity, requested development plan, and
/// optics. Recipe changes reuse the same sensor-domain source while their
/// processed RGB tiles remain independently recipe-aware.
#[derive(Debug)]
pub(super) struct EditDetailSessionCache {
    entries: VecDeque<CachedEditDetailSession>,
    resident_budget_bytes: u64,
    max_entries: usize,
}

const DEFAULT_DETAIL_SESSION_BUDGET_BYTES: u64 = 1_024 * 1_024 * 1_024;
const DEFAULT_DETAIL_SESSION_MAX_ENTRIES: usize = 4;

impl Default for EditDetailSessionCache {
    fn default() -> Self {
        Self {
            entries: VecDeque::new(),
            resident_budget_bytes: DEFAULT_DETAIL_SESSION_BUDGET_BYTES,
            max_entries: DEFAULT_DETAIL_SESSION_MAX_ENTRIES,
        }
    }
}

impl EditDetailSessionCache {
    pub(super) fn get(
        &mut self,
        representation_id: RepresentationId,
        source: RepresentationFingerprint,
        source_environment_cache_identity: &str,
        requested_raw_development_plan_identity: &str,
        optics: &OpticsSettings,
    ) -> Option<Arc<CachedDetailSource>> {
        let position = self.entries.iter().position(|entry| {
            entry.representation_id == representation_id
                && entry.source == source
                && entry.source_environment_cache_identity == source_environment_cache_identity
                && requested_raw_development_plan_cache_matches(
                    &entry.requested_raw_development_plan_identity,
                    requested_raw_development_plan_identity,
                )
                && &entry.optics == optics
        })?;
        let entry = self.entries.remove(position)?;
        let session = Arc::clone(&entry.session);
        self.entries.push_back(entry);
        self.trim();
        Some(session)
    }

    pub(super) fn insert(&mut self, entry: CachedEditDetailSession) {
        self.entries.retain(|candidate| {
            candidate.representation_id != entry.representation_id
                || candidate.source != entry.source
                || candidate.source_environment_cache_identity
                    != entry.source_environment_cache_identity
                || candidate.requested_raw_development_plan_identity
                    != entry.requested_raw_development_plan_identity
                || candidate.optics != entry.optics
        });
        self.entries.push_back(entry);
        self.trim();
    }

    fn trim(&mut self) {
        // Always retain the most recently requested source even when one very
        // large frame exceeds the nominal budget by itself. Older Arc users
        // may finish safely after eviction; they simply stop counting as
        // reusable cache entries.
        while self.entries.len() > 1
            && (self.entries.len() > self.max_entries
                || self.resident_bytes() > self.resident_budget_bytes)
        {
            self.entries.pop_front();
        }
    }

    fn resident_bytes(&self) -> u64 {
        self.entries.iter().fold(0_u64, |total, entry| {
            total.saturating_add(entry.session.resident_bytes())
        })
    }
}

#[derive(Debug, Clone, Copy, Eq, Hash, PartialEq)]
struct DetailTileCacheKey {
    x: u32,
    y: u32,
    width: u32,
    height: u32,
}

impl From<DetailTileRect> for DetailTileCacheKey {
    fn from(rect: DetailTileRect) -> Self {
        Self {
            x: rect.x,
            y: rect.y,
            width: rect.width,
            height: rect.height,
        }
    }
}

#[derive(Debug, Clone)]
struct CachedDetailTile {
    row_stride_bytes: u32,
    bytes: Vec<u8>,
}

#[derive(Debug, Default)]
struct DetailTileCache {
    recipe_identity: Option<[u8; 32]>,
    bytes: usize,
    entries: HashMap<DetailTileCacheKey, CachedDetailTile>,
    least_recently_used: VecDeque<DetailTileCacheKey>,
}

const MAX_CACHED_DETAIL_TILE_BYTES: usize = 96 * 1_024 * 1_024;

pub(super) fn cached_detail_tile(
    source: &CachedDetailSource,
    plan: &AdjustmentRenderPlan,
    recipe_identity: [u8; 32],
    rect: DetailTileRect,
) -> AnyResult<ffi::FfiEditedDetailTile> {
    let key = DetailTileCacheKey::from(rect);
    {
        let mut cache = source
            .tiles
            .lock()
            .map_err(|_| anyhow!("full-detail tile cache lock is poisoned"))?;
        if cache.recipe_identity != Some(recipe_identity) {
            *cache = DetailTileCache {
                recipe_identity: Some(recipe_identity),
                ..DetailTileCache::default()
            };
        }
        if let Some(tile) = cache.entries.get(&key).cloned() {
            cache
                .least_recently_used
                .retain(|candidate| candidate != &key);
            cache.least_recently_used.push_back(key);
            return Ok(ffi::FfiEditedDetailTile {
                x: key.x,
                y: key.y,
                width: key.width,
                height: key.height,
                row_stride_bytes: tile.row_stride_bytes,
                bytes: tile.bytes,
            });
        }
    }

    let rendered = source
        .session
        .render_plan_tile(plan, DetailTileRequest { rect })?;
    let tile = CachedDetailTile {
        row_stride_bytes: rendered.row_stride_bytes,
        bytes: rendered.bytes,
    };
    let tile_bytes = tile.bytes.len();
    let mut cache = source
        .tiles
        .lock()
        .map_err(|_| anyhow!("full-detail tile cache lock is poisoned"))?;
    if cache.recipe_identity != Some(recipe_identity) {
        // A newer Recipe may have reached the same prepared source while this
        // tile was being calculated. Do not leak its pixels across Recipe
        // identities; return this request's result without admitting it.
        return Ok(ffi::FfiEditedDetailTile {
            x: key.x,
            y: key.y,
            width: key.width,
            height: key.height,
            row_stride_bytes: tile.row_stride_bytes,
            bytes: tile.bytes,
        });
    }
    while cache.bytes.saturating_add(tile_bytes) > MAX_CACHED_DETAIL_TILE_BYTES {
        let Some(evicted_key) = cache.least_recently_used.pop_front() else {
            break;
        };
        if let Some(evicted) = cache.entries.remove(&evicted_key) {
            cache.bytes = cache.bytes.saturating_sub(evicted.bytes.len());
        }
    }
    cache.bytes = cache.bytes.saturating_add(tile_bytes);
    cache.entries.insert(key, tile.clone());
    cache
        .least_recently_used
        .retain(|candidate| candidate != &key);
    cache.least_recently_used.push_back(key);
    Ok(ffi::FfiEditedDetailTile {
        x: key.x,
        y: key.y,
        width: key.width,
        height: key.height,
        row_stride_bytes: tile.row_stride_bytes,
        bytes: tile.bytes,
    })
}
