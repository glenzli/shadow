# Shadow

Shadow is a local-first, AI-native photo catalog and non-destructive editor. The project is currently in its Mac-first foundation phase; the persistent core and image interfaces are designed to remain portable to Windows.

## Repository map

Use the source tree as the architecture index. Start with the narrowest entry below, then follow
its module declarations and local README; avoid reconstructing a feature by searching the whole
workspace.

| Area | Stable entry | Responsibility |
| --- | --- | --- |
| Domain contracts | [`shadow-domain`](crates/shadow-domain/src/lib.rs) | Pure identities, edit graphs, Recipes, review state, and persisted contract types |
| Catalog | [`shadow-catalog`](crates/shadow-catalog/src/lib.rs) | SQLite ownership, repositories, immutable ledgers, and projections |
| Cache | [`shadow-cache`](crates/shadow-cache/README.md) | Content-addressed storage, verification, quarantine, and cache records |
| Core workflows | [`shadow-core`](crates/shadow-core/src/lib.rs) | Scanning, source inspection, cache orchestration, and bounded workers |
| AI evidence | [`shadow-ai`](crates/shadow-ai/README.md) | Technical observations, explicit feedback, admission, and model-independent scoring |
| Rust image boundary | [`shadow-bridge`](crates/shadow-bridge/README.md) | Safe Rust API over the C++ decoder and render kernel |
| Desktop services | [`shadow-desktop-bridge`](crates/shadow-desktop-bridge/README.md) | Long-lived Library, Review, Precision, export, and CXX-facing application services |
| Native image kernel | [`shadow-image`](cpp/shadow-image/README.md) | Decoder providers, RAW development, adjustment execution, and native image buffers |
| Qt application | [`apps/desktop`](apps/desktop/README.md) | QML presentation, Qt controllers, image providers, and desktop lifecycle |
| CLI and validation | [`shadow-cli`](apps/shadow-cli/src/main.rs), [`xtask`](xtask/src/main.rs) | Operator commands and repository-level checks |

The nearest code-owned README or module documentation is authoritative for navigation within a
subsystem. When a responsibility moves, update that local index in the same change.

## Pre-release contract policy

Until Shadow makes its first compatibility promise, every Shadow-owned persisted
schema, render contract, cache identity, and implementation revision remains v1.
Contract changes replace v1 and may invalidate local development data or
rebuildable caches; they do not create migration ladders. Variants and execution
routes use explicit types or feature identities, never inflated version numbers.
External standards and dependencies retain their real upstream versions.

## License and upstream provenance

Shadow is free software under the GNU General Public License, version 3 or later
(`GPL-3.0-or-later`); [`LICENSE`](LICENSE) contains Shadow's license notice and
links to the canonical GPLv3 text. Release archives that contain object code
also include the complete license text.

That choice deliberately keeps the project compatible with direct, attributed
integration of GPL-compatible open-source photography work, including selected
algorithms and public calibration data from projects such as darktable and
RawTherapee. It does **not** grant a right to copy or redistribute Adobe,
Nikon, Canon, Sony, or other vendor profiles merely because they are installed
on a developer's machine. Every imported third-party source file or profile
must be recorded with its upstream origin, revision, applicable license, and
local changes as described in [`THIRD_PARTY.md`](THIRD_PARTY.md).

The public Shadow distribution contains no vendor SDK, vendor profile, or
private decoder-provider implementation. Such a provider may communicate with
Shadow only through its documented provider protocol and remains a separately
installed local component.

The first executable slices are intentionally small:

```text
folder scan → live durable registration/progress → first-page visibility → cancel/stable reopen
RAW/DNG → metadata → embedded preview or bounded JPEG proxy → content cache
cached JPEG visual → bounded display-luma → version-bound technical observation → Review detail
two exact cached visuals → verified Compare RGBA frames → explicit outcome → append-only feedback / forget fact
Review flag/rating → expected-head CAS → immutable decision event → current projection / inverse-event undo
RAW/DNG → sensor mosaic → reference RGB correctness baseline
RAW/DNG → bounded processed linear-light RGB proxy (sRGB primaries) → ordered edit nodes → versioned JPEG preview
RAW/DNG → bounded full-size u16 reference session → exact level-zero RGB8 detail viewport
```

## Developer commands

```sh
cargo xtask check
cargo xtask test
cargo xtask doctor
cargo xtask native-check
cargo xtask desktop-build
cargo xtask raw-smoke ./local-reference/sample-assets/raw
cargo run --package shadow-cli -- init ./catalogs/demo.sqlite
cargo run --package shadow-cli -- scan ./catalogs/demo.sqlite /path/to/photos
cargo run --package shadow-cli -- scan-cache ./catalogs/demo.sqlite ./catalogs/cache /path/to/photos
cargo run --package shadow-cli -- cache-read ./catalogs/demo.sqlite ./catalogs/cache /path/to/input.dng
cargo run --package shadow-cli -- backup ./catalogs/demo.sqlite ./backups/demo-20260725.sqlite
cargo run --package shadow-cli -- verify-backup ./backups/demo-20260725.sqlite
cargo run --package shadow-cli -- inspect-raw /path/to/input.dng
cargo run --package shadow-cli -- inspect-store ./catalogs/demo.sqlite ./catalogs/cache /path/to/input.dng
./build/native-dev/cpp/shadow-image/shadow-raw-probe /path/to/input.dng ./bench-results/raw-probe
```

`raw-smoke` recursively discovers common camera RAW extensions and exercises metadata,
embedded-preview extraction, a bounded reference render, and optical-profile discovery for every
local fixture. With no directory argument it uses the ignored
`local-reference/sample-assets/raw/` directory, so proprietary sample files never enter Git.
Recognized formats that cannot expose mosaic/RGB pixels pass only when the provider reports that
limitation truthfully and a valid embedded preview remains available; the matrix reports them as
`preview-only` instead of hiding them behind the first decoder failure.
The non-Release desktop app also scans that fixture directory on startup, making newly added
local RAW samples available in the persistent development Library without manual folder import.

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

`backup` uses SQLite's Online Backup API against a separate read-only source
connection, then reopens the completed copy for an integrity and foreign-key
restore drill before publishing it. It never overwrites an existing destination.
`verify-backup` repeats that non-mutating restore drill for an existing backup;
both commands print the catalog's schema, page accounting, and durable entity
counts so recovery can be checked without launching the desktop app.

The first Qt Quick Review and Precision application now lives in [`apps/desktop`](apps/desktop/README.md). Review opens the existing global local Library immediately, then treats Add Folder as a separate cooperative import job. The Rust scanner publishes monotonic durable-registration snapshots while it runs; Qt polls them without callbacks across the FFI boundary, reconciles the first 96 rows by stable representation identity, and can show/open the first RAWs in Precision before enumeration and queued preview work finish. Scanning never exposes an unstable pagination cursor, performs one final stable-prefix refresh before re-enabling paging, and can be cancelled without deleting already registered assets: enumeration stops, queued provider work observes the same token, and the current provider call may finish. The durable import journal describes enumeration/catalog registration: cancellation during that phase records `cancelled`; a stop arriving only during `PreparingPreviews` can leave that journal `completed` while the desktop job still terminates `cancelled`. Unexpected exit/failed sessions retain the existing idempotent root-rescan recovery boundary. Review lazily requests verified visuals; its selected-photo sidebar shows factual display-proxy luma percentiles, near-black/near-white fractions, and two scale-sensitive detail proxies when a current observation exists. These values are not badges, rankings, or RAW exposure measurements. Review now supports independent human Pick/Reject flags and zero-through-five-star ratings for the selected grid photo through Inspector controls or keyboard shortcuts. Every real state change is an immutable, integrity-checked ledger event; the small current table is only a forward-moving projection pointer. Writes compare-and-swap the expected photo head, and session undo appends the inverse transition only while the exact state and head still match, so another writer cannot be silently overwritten. Review also has a deliberately narrow Compare Evidence path: it places two existing cached visuals side by side and records one of five explicit human outcomes—left preferred, right preferred, keep both, keep neither, or cannot compare—as append-only `Global` feedback. A session-authenticated grid handle freezes each exact `CachedArtifactRecord`; Compare creates dedicated request tickets, reads those exact content-addressed bytes without reselecting the current preferred proxy, and records the SHA-256 identity of the normalized RGBA8888 frame returned by Qt's decoder. The event stores both full encoded-artifact provenance and the decoded-frame contract, so a later preferred-artifact change cannot rewrite what the user saw. Compare undo appends a forget fact instead of deleting the source event, and the visible evidence count is scoped to the current application session. Neither manual review decisions nor Compare evidence currently train a model or change automatic ranking. Precision opens the same Catalog-owned RAW source, prepares one bounded processed linear-light RGB working proxy in sRGB primaries, and executes an ordered stack of one through sixteen `PHOTO`-scope Basic adjustment layers without exposing SQLite, LibRaw, or compressed image buffers to QML. The layer list supports add, duplicate, delete, reorder, selection, and lossless enabled/bypassed state; the four slider groups and point-curve Inspector edit the selected layer. The complete stack—order, every parameter and optional Tone Curve, bypass state, and stable layer/node identity—is the atomic value consumed by preview, dirty comparison, gesture-coalesced session undo/redo, immutable save, reopen, and checkout. Each accepted warm preview also atomically publishes generation-matched transient 256-bin RGB histograms, an encoded-luma histogram, and strict pre-clamp working-RGB shadow/highlight counts; the panel explicitly describes the complete display-encoded sRGB proxy, not RAW exposure or durable evidence. Durable version rows show readable parent-relative Recipe changes, while preview generations bind to immutable base commits and saves compare-and-swap the expected `working` head. FIT remains a bounded proxy, while 100%+ lazily retains one capped full-size u16 source and applies the identical Recipe to the visible level-zero tiles. Qt receives one uncompressed RGB8 viewport guarded by photo/edit/viewport generations; these transient pixels are never written to Catalog.

The non-destructive edit foundation is also live below the UI. `shadow-domain` owns typed, stage-checked edit DAGs, adjustment layers and scopes, immutable shared-layer revisions, Git-like Recipe commits/branches/named versions, deterministic structural diffs, and validated human review-decision transitions. The unstable Catalog remains schema v1: incompatible development databases are deliberately discarded instead of accumulating migration or compatibility code before the format is released. It stores immutable commit JSON, normalized parent edges, content-addressed snapshot identities, movable refs, integrity-checked append-only preference feedback with non-destructive forget facts, optional exact presented-visual provenance, rebuildable technical observations bound to the complete source-artifact and algorithm/preprocessing revision, and the append-only per-photo decision ledger. Corrupt observation payloads degrade to an absent Review summary and are treated as missing by the next decode/backfill pass; old heads and human evidence are never overwritten. The desktop bridge compiles the supported one-to-sixteen-layer Recipe subset in vector order into a bounded, dependency-ordered plan for the C++ reference executor. Every layer remains a fixed Exposure → Contrast → optional Tone Curve → RGB Channel Gain → Saturation chain; its persisted enabled flag bypasses all of its nodes without deleting their data. Layer and node identities are stack-wide unique and retained identities are checked against the immutable base before save. During this unstable phase, obsolete Recipe shapes are replaced rather than migrated. Each layer's point-curve editor losslessly loads valid persisted curves with 2 through 256 points; active editing remains intentionally narrower at 32 points, y in `[0, 1]`, and an x gap of at least `1/4096`. This is an ordered fixed-layer surface with a transient warm-proxy histogram panel, not arbitrary DAG authoring, masks, blend/opacity controls, full-resolution scopes, or Lightroom feature parity.

`shadow-ai` adds model-independent manifests, resource/privacy admission, explainable group-relative scoring, a small deterministic preference head, and model-free display-luma observations (histogram, exact percentiles, clipping fractions, and explicitly scoped sharpness proxies). The application can now persist explicit human pairwise outcomes together with exact encoded-artifact and decoded-frame provenance, plus a separate manual Pick/Reject/rating ledger. It still has no active feature extractor or preference model: AI does not write the manual ledger, automatically compare or rank photos, or produce automatic Picks or Rejects. Manual ledger events are not yet converted into preference-model examples. Compare presents each photo's available technical observations only as parallel facts; different embedded-preview and generated-proxy upstream processing may make even matching metrics unsuitable for direct comparison. Those technical values have their own current Catalog provenance, but the feedback event does not yet snapshot the separate technical-observation payload that accompanied the visual. The frame receipt stops at Qt's normalized decoded image and does not claim to reproduce display ICC, GPU interpolation, or physical screen pixels. Shadow deliberately ships no pretend inference or unselected model weights.

The reusable C++ decoder contract is documented in [`cpp/shadow-image/README.md`](cpp/shadow-image/README.md); its Rust boundary is documented in [`crates/shadow-bridge/README.md`](crates/shadow-bridge/README.md), and cache semantics in [`crates/shadow-cache/README.md`](crates/shadow-cache/README.md). LibRaw and compatible private providers now enter one provider-neutral `RawFrame` source-development path when they can expose supported sensor data; every provider-RGB compatibility fallback is explicit in the pipeline receipt and cache identity. Native-size Bayer development has a bounded macOS Metal backend with an exact CPU fallback and distinct cache provenance; area-integrated catalog previews intentionally retain the CPU reference while their geometry is made GPU-safe. The local product and research material lives under `local-reference/` and is intentionally ignored by Git.
