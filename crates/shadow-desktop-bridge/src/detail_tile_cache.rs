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
