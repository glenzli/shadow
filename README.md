# Shadow

Shadow is a local-first, AI-native photo catalog and non-destructive editor. The project is currently in its Mac-first foundation phase; the persistent core and image interfaces are designed to remain portable to Windows.

The first executable slices are intentionally small:

```text
folder scan → transactional catalog registration → stable reopen → statistics
RAW/DNG → metadata → embedded preview or bounded JPEG proxy → content cache
RAW/DNG → sensor mosaic → reference RGB correctness baseline
RAW/DNG → bounded scene-linear working proxy → ordered edit nodes → versioned JPEG preview
```

## Developer commands

```sh
cargo xtask check
cargo xtask test
cargo xtask doctor
cargo xtask native-check
cargo xtask desktop-build
cargo run --package shadow-cli -- init ./catalogs/demo.sqlite
cargo run --package shadow-cli -- scan ./catalogs/demo.sqlite /path/to/photos
cargo run --package shadow-cli -- scan-cache ./catalogs/demo.sqlite ./catalogs/cache /path/to/photos
cargo run --package shadow-cli -- cache-read ./catalogs/demo.sqlite ./catalogs/cache /path/to/input.dng
cargo run --package shadow-cli -- inspect-raw /path/to/input.dng
cargo run --package shadow-cli -- inspect-store ./catalogs/demo.sqlite ./catalogs/cache /path/to/input.dng
./build/native-dev/cpp/shadow-image/shadow-raw-probe /path/to/input.dng ./bench-results/raw-probe
```

`inspect-store` exercises the first complete background path: it registers one
RAW representation, decodes its provider-neutral capability snapshot away from
the caller and SQLite writer threads, then persists the result only if the file
size and modification time still match. The largest decodable embedded preview
is stored by BLAKE3 content identity under the explicit cache root. When a RAW
has no preview, the C++ kernel renders and JPEG-encodes a versioned 2048-edge
fallback without copying the full-size RGB buffer into Rust. Cache reads verify
the digest and byte length lazily. Snapshots from multiple decoder providers may
coexist for one representation.

`cache-read` exercises the recovery boundary used by the Review grid.
Missing or corrupt blobs conditionally invalidate only the exact Catalog record
that failed. Corrupt bytes are retained under `quarantine/b3`; the next folder
scan schedules the missing visual again without disturbing a concurrent newer
artifact.

The first Qt Quick Review and Precision application now lives in [`apps/desktop`](apps/desktop/README.md). Review selects a folder, runs the Rust scan/decode/cache pipeline off the UI thread, pages lightweight Catalog metadata, and lazily requests verified visuals. Precision opens the same Catalog-owned RAW source, prepares one bounded scene-linear working proxy, executes a real ordered adjustment layer, and saves or checks out immutable versions without exposing SQLite, LibRaw, or compressed image buffers to QML. Slider gestures have bounded session undo/redo; durable version rows show readable parent-relative Recipe changes instead of internal commit or parameter identifiers. Preview generations bind to immutable base commits, and version saves use an atomic expected-head check so stale work cannot overwrite a newer `working` ref.

The non-destructive edit foundation is also live below the UI. `shadow-domain` owns typed, stage-checked edit DAGs, adjustment layers and scopes, immutable shared-layer revisions, Git-like Recipe commits/branches/named versions, and deterministic structural diffs. Catalog schema v6 stores immutable commit JSON, normalized parent edges, content-addressed snapshot identities, movable refs, and integrity-checked append-only preference feedback with non-destructive forget facts; old heads and human evidence are never overwritten. The desktop bridge compiles the supported single-layer, single-chain Recipe subset into a bounded, dependency-ordered plan for the C++ reference executor. Exposure, contrast, the versioned unclipped Tone Curve, RGB channel gain, and saturation are executable. Precision still exposes only four slider groups; a persisted Tone Curve is replayed and preserved across slider saves, but has no Inspector editor yet.

`shadow-ai` adds model-independent manifests, resource/privacy admission, explainable group-relative scoring, a small deterministic preference head, and model-free display-luma observations (histogram, exact percentiles, clipping fractions, and explicitly scoped sharpness proxies). These observations do not produce Picks or Rejects, and their provenance prevents comparisons across incompatible proxy preprocessing. Shadow deliberately ships no pretend inference or unselected model weights.

The reusable C++ decoder contract is documented in [`cpp/shadow-image/README.md`](cpp/shadow-image/README.md); its Rust boundary is documented in [`crates/shadow-bridge/README.md`](crates/shadow-bridge/README.md), and cache semantics in [`crates/shadow-cache/README.md`](crates/shadow-cache/README.md). The probe intentionally uses LibRaw's reference RGB processing only as a correctness baseline; Shadow's own scene-linear color and adjustment pipeline will replace that stage. The local product and research material lives under `local-reference/` and is intentionally ignored by Git.
