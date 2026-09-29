# Shadow Library Sharing

This crate owns Shadow's Mac-first remote Library boundary. It shares a bounded
photo manifest and content-addressed previews without exposing the server's
filesystem, then materializes an exact original into a verified client-local
cache when editing or export needs it. The manifest carries a versioned,
provider-neutral metadata projection rather than arbitrary EXIF or MakerNote
payloads, and declares whether preview pixels still require their encoded
orientation transform.

Start at [`lib.rs`](src/lib.rs), then follow:

- `protocol` for the dated-revision request, response, capability, and manifest values;
- `catalog_source` for the read-only projection from a local Catalog and preview cache;
- `server` for authenticated admission and bounded request execution;
- `client` for the matching one-request-per-connection transport;
- `mirror` for progressive cursor-chain membership, client-local review state, and downloaded proxies;
- `materializer` for resumable, digest-verified original download and atomic publication;
- `presentation` for local adjusted-preview precedence over a remote browse proxy.

The wire contract never serializes `AssetLocation` or a native path. One
manifest row represents a logical Photo, names its RAW-preferred browse/edit
representation, and carries a bounded inventory of its currently shareable
original RAW/raster representations. Each representation reports source counts
and an optional exact whole-file BLAKE3 identity. `NotPrepared` is deliberate:
first preview sync never reads an entire RAW merely to deduplicate it. Preparing
an original records that verified identity in the server Catalog, so later
syncs can reconcile the same bytes across changed server addresses or different
servers.

Until that strong identity is available, a remote identity remains
`(server_id, photo_id, representation_id)`. Recipe history is not synchronized
by this first contract: the editing device owns its Recipe, and its current
local `RecipePreview` wins over the cached remote proxy.

Shadow-owned wire revisions use `YYYYMMDD.N`. The serialized integer is
`YYYYMMDDNN`, while diagnostics and documentation use the dotted form. A
compatible optional addition is advertised through `ServerCapabilities`; an
incompatible request/response shape advances `LIBRARY_PROTOCOL_REVISION`.
Debug peers must match exactly. A future stable channel may widen that policy
with explicit minimum-readable/minimum-writable revisions rather than inferring
compatibility from numeric ordering.
