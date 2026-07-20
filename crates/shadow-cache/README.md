# shadow-cache

`shadow-cache` owns rebuildable, content-addressed blob storage for embedded previews and generated render proxies.

Identity is BLAKE3-256 over the exact payload bytes. Files use this layout and carry no source filename or extension:

```text
<cache-root>/blobs/b3/<first-two-hex>/<remaining-hex>
```

Writes use a same-directory temporary file followed by an atomic rename. Existing content is re-hashed before reuse; `read_verified` also hashes lazy reads before bytes reach an image consumer. A corrupt blob is reported rather than silently accepted. The cache is not the source of truth and intentionally does not force an `fsync` for every rebuildable visual.

SQLite stores the digest, codec, dimensions, byte order, generator provenance, and source fingerprint. It never stores an absolute cache path. Garbage collection can therefore compare Catalog references with the digest tree independently of where the user places the cache root.
