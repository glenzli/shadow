# Shadow Library Sharing

This crate owns Shadow's Mac-first remote Library boundary. It shares a bounded
photo manifest and content-addressed previews without exposing the server's
filesystem, then materializes an exact original into a verified client-local
cache only when editing needs it.

Start at [`lib.rs`](src/lib.rs), then follow:

- `protocol` for the versioned request, response, capability, and manifest values;
- `catalog_source` for the read-only projection from a local Catalog and preview cache;
- `server` for authenticated admission and bounded request execution;
- `client` for the matching one-request-per-connection transport;
- `mirror` for the client-local remote manifest and downloaded proxy state;
- `materializer` for resumable, digest-verified original download and atomic publication;
- `presentation` for local adjusted-preview precedence over a remote browse proxy.

The wire contract never serializes `AssetLocation` or a native path. A remote
identity remains `(server_id, photo_id, representation_id)` until a verified
original is deliberately registered in the client Catalog. Recipe history is
not synchronized by this first contract: the editing device owns its Recipe,
and its current local `RecipePreview` wins over the cached remote proxy.
