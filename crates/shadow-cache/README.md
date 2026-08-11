# shadow-cache

`shadow-cache` owns rebuildable, content-addressed blob storage for embedded previews and generated render proxies.

Start in [`src/lib.rs`](src/lib.rs) for blob identity, atomic publication,
verified reads, and corrupt-blob quarantine. Continue to
[`src/maintenance.rs`](src/maintenance.rs) for canonical-tree inventory,
unknown-entry classification, the publication grace period, and conservative
Catalog-guided garbage collection.

Identity is BLAKE3-256 over the exact payload bytes. Files use this layout and carry no source filename or extension:

```text
<cache-root>/blobs/b3/<first-two-hex>/<remaining-hex>
```

Writes use a same-directory temporary file followed by an atomic rename. Existing content is re-hashed before reuse; `read_verified` also hashes lazy reads before bytes reach an image consumer. A corrupt blob is never silently accepted. `quarantine_corrupt` re-verifies it and then moves the bytes to `quarantine/b3` rather than deleting them, freeing the canonical digest path for regeneration. The cache is not the source of truth and intentionally does not force an `fsync` for every rebuildable visual.

SQLite stores the digest, codec, dimensions, byte order, generator provenance, and source fingerprint. It never stores an absolute cache path. Garbage collection can therefore compare Catalog references with the digest tree independently of where the user places the cache root. Maintenance never assigns product meaning to files: the Catalog supplies the retained digest snapshot, while `shadow-cache` classifies only canonical paths and preserves unknown entries.

AI RAW foundations use a separate verified container contract in
[`src/foundation_artifact.rs`](src/foundation_artifact.rs). The reader validates the complete
`.shadowrawf` framing, fixed public RawNIND model/source-pixel contract, source and artifact
identities, stripe coverage and digests, and every finite float before exposing bounded row reads.
Its verification projection includes the zero-or-one top/left active-sensor crop needed to bind
canonical RGGB output back to the original RAW geometry; model paths and cache locations remain
outside portable artifact identity. Execution-profile admission lives in
[`src/foundation_artifact/execution_profile.rs`](src/foundation_artifact/execution_profile.rs):
the legacy ORT 1.24.4 implementation and the isolated ORT 1.27 experimental Build are separate
exact pairs, so neither can inherit the other's implementation or cache identity.
