# Shadow

Shadow is a local-first, AI-native photo catalog and non-destructive editor. The project is currently in its Mac-first foundation phase; the persistent core and image interfaces are designed to remain portable to Windows.

The first executable slices are intentionally small:

```text
folder scan → transactional catalog registration → stable reopen → statistics
RAW/DNG → metadata → embedded preview or bounded JPEG proxy → content cache
cached JPEG visual → bounded display-luma → version-bound technical observation → Review detail
two cached visuals → explicit pairwise outcome → append-only Global feedback / forget fact
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
artifact. A successful JPEG visual also queues one bounded background technical
observation. The worker verifies the cache blob again and commits only while the
exact representation fingerprint and preferred artifact are still current; a
stale job is discarded rather than attached to newer pixels.

The first Qt Quick Review and Precision application now lives in [`apps/desktop`](apps/desktop/README.md). Review selects a folder, runs the Rust scan/decode/cache/observation pipeline off the UI thread, pages lightweight Catalog metadata, and lazily requests verified visuals. Its selected-photo sidebar shows factual display-proxy luma percentiles, near-black/near-white fractions, and two scale-sensitive detail proxies when a current observation exists; these values are not badges, rankings, or RAW exposure measurements. Review also has a deliberately narrow Compare Evidence path: it places two existing cached visuals side by side and records one of five explicit human outcomes—left preferred, right preferred, keep both, keep neither, or cannot compare—as append-only `Global` feedback. Undo appends a forget fact instead of deleting the source event, and the visible evidence count is scoped to the current application session. This first slice has no feature snapshot, active model, or group identity, does not train the preference head or change automatic ranking, and does not yet persist complete provenance for the exact visual artifacts that were presented. Precision opens the same Catalog-owned RAW source, prepares one bounded scene-linear working proxy, executes a real ordered adjustment layer, and saves or checks out immutable versions without exposing SQLite, LibRaw, or compressed image buffers to QML. Four slider groups, a global point-curve Inspector, and the `PHOTO`-scope Basic Adjustments layer's enabled/bypassed state now form one complete settings value; preview, dirty comparison, gesture-coalesced session undo/redo, save, reopen, and checkout all consume or restore that value atomically. Bypassing the layer only skips it during rendering: its sliders, optional Tone Curve payload, and stable layer/node identities remain unchanged for lossless re-enabling. Durable version rows show readable parent-relative Recipe changes, including `Tone Curve` and layer-state changes, instead of internal commit or parameter identifiers. Preview generations bind to immutable base commits, and version saves use an atomic expected-head check so stale work cannot overwrite a newer `working` ref.

The non-destructive edit foundation is also live below the UI. `shadow-domain` owns typed, stage-checked edit DAGs, adjustment layers and scopes, immutable shared-layer revisions, Git-like Recipe commits/branches/named versions, and deterministic structural diffs. Catalog schema v7 stores immutable commit JSON, normalized parent edges, content-addressed snapshot identities, movable refs, integrity-checked append-only preference feedback with non-destructive forget facts, and rebuildable technical observations bound to the complete source-artifact and algorithm/preprocessing revision. Corrupt observation payloads degrade to an absent Review summary and are treated as missing by the next decode/backfill pass; old heads and human evidence are never overwritten. The desktop bridge compiles the supported single-layer, single-chain Recipe subset into a bounded, dependency-ordered plan for the C++ reference executor. The layer's persisted enabled flag gates execution of Exposure, contrast, the versioned unclipped Tone Curve, RGB channel gain, and saturation without deleting or rewriting any of those nodes. The point-curve Inspector losslessly loads valid persisted curves with 2 through 256 points. Active editing is intentionally narrower: at most 32 points, y constrained to `[0, 1]`, and neighboring x positions separated by at least `1/4096`. Extended-range or oversized persisted curves remain exact and view-only, with an explicit reset path. Adding or resetting the optional curve canonically changes the supported Recipe between four and five nodes; edits preserve its node identity. This is one global piecewise-linear point curve inside one fixed Basic Adjustments layer, not a completed free-form multi-layer system, parameter curves, per-channel curves, a histogram tool, or Lightroom feature parity.

`shadow-ai` adds model-independent manifests, resource/privacy admission, explainable group-relative scoring, a small deterministic preference head, and model-free display-luma observations (histogram, exact percentiles, clipping fractions, and explicitly scoped sharpness proxies). The application can now persist explicit human pairwise outcomes, but it still has no active feature extractor or preference model and does not automatically compare or rank photos, produce Picks or Rejects, or claim that differently generated/resized/sharpened proxies are comparable. Compare presents each photo's available technical observations only as parallel facts; different embedded-preview and generated-proxy upstream processing may make even matching metrics unsuitable for direct comparison. Observation provenance prevents stale results from being reused as current, while complete provenance for the exact pair of displayed visual artifacts remains future work. Shadow deliberately ships no pretend inference or unselected model weights.

The reusable C++ decoder contract is documented in [`cpp/shadow-image/README.md`](cpp/shadow-image/README.md); its Rust boundary is documented in [`crates/shadow-bridge/README.md`](crates/shadow-bridge/README.md), and cache semantics in [`crates/shadow-cache/README.md`](crates/shadow-cache/README.md). The probe intentionally uses LibRaw's reference RGB processing only as a correctness baseline; Shadow's own scene-linear color and adjustment pipeline will replace that stage. The local product and research material lives under `local-reference/` and is intentionally ignored by Git.
