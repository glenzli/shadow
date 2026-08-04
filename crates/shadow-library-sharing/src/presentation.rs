use shadow_catalog::CachedArtifactRecord;

use crate::CachedRemotePreview;

/// The visual selected for one mirrored Library row.
///
/// `local` must already be the Catalog's current source/Recipe-validated
/// preferred artifact. It wins unconditionally, so a freshly rendered local
/// Recipe preview immediately replaces the server-provided browse proxy.
#[derive(Debug, Copy, Clone)]
pub enum BrowseVisual<'a> {
    Local(&'a CachedArtifactRecord),
    Remote(&'a CachedRemotePreview),
    Unavailable,
}

#[must_use]
pub const fn select_browse_visual<'a>(
    local: Option<&'a CachedArtifactRecord>,
    remote: Option<&'a CachedRemotePreview>,
) -> BrowseVisual<'a> {
    match (local, remote) {
        (Some(local), _) => BrowseVisual::Local(local),
        (None, Some(remote)) => BrowseVisual::Remote(remote),
        (None, None) => BrowseVisual::Unavailable,
    }
}

#[cfg(test)]
mod tests;
