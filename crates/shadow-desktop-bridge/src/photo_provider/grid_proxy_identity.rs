//! Bounded persistent identity for generated Library grid proxies.
//!
//! RAW development plans and isolated helper graphs both have complete,
//! independently evolving identities. The Catalog needs both inputs for exact
//! cache invalidation, while Review evidence carries the resulting variant key
//! through a bounded identifier field. This owner preserves the complete
//! identities through full BLAKE3 digests instead of embedding their unbounded
//! diagnostic representations.

const GRID_PROXY_IDENTITY_VERSION: &str = "v2";

pub(super) fn grid_proxy_variant_key(
    max_edge: u32,
    jpeg_quality: u8,
    raw_development_plan_identity: &str,
    isolated_helper_graph_identity: Option<&str>,
) -> String {
    let raw_plan_digest = blake3::hash(raw_development_plan_identity.as_bytes()).to_hex();
    let mut identity = format!(
        "shadow-photo-router:grid-jpeg-{max_edge}-q{jpeg_quality}-444-\
         {GRID_PROXY_IDENTITY_VERSION};raw-plan-b3={raw_plan_digest}"
    );
    if let Some(helper_graph_identity) = isolated_helper_graph_identity {
        let helper_graph_digest = blake3::hash(helper_graph_identity.as_bytes()).to_hex();
        identity.push_str(";isolated-graph-b3=");
        identity.push_str(helper_graph_digest.as_str());
    }

    // Even the widest integer spellings plus both full digests remain below
    // Review evidence's 256-byte identifier boundary. Keep the assertion next
    // to the grammar so future fields cannot silently recreate an
    // unpersistable Catalog artifact.
    debug_assert!(identity.len() <= 256);
    identity
}

#[cfg(test)]
mod tests;
