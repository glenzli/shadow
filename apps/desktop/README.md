# Shadow desktop

The first macOS Review slice is a native Qt Quick application backed by the existing Rust and C++ core:

```text
startup Catalog page / Add Folder / QML Review grid
  → ReviewController (Qt UI thread + polling timer + QtConcurrent job)
  → shadow-desktop-bridge (long-lived CXX session)
  → shadow-core controlled scan / cancellable decode workers
  → shadow-catalog single writer
  → embedded preview or generated proxy cache
  → bounded display-luma observation worker → Catalog v1 summary

two signed exact-artifact handles / explicit outcome
  → compare-only request tickets → verified cache bytes
  → Qt decoded RGBA frame receipt → ReviewController evidence write
  → Catalog v1 append-only Global feedback / forget fact

Pick / Reject / 0–5 rating command
  → full-state expected-head CAS → immutable human decision event
  → forward-only current projection → append-only inverse-event undo
```

QML never opens SQLite, calls LibRaw, or interprets blob paths. The global local Library loads its existing first page at startup; Add Folder starts a separate import job and no longer clears already visible photos. The Rust bridge returns bounded Review metadata pages using a stable path/representation cursor and exposes a generation-bound progress snapshot for Qt to poll. While import is changing sort order, each live first-page snapshot is reconciled as a prefix: matching rows move or update, new rows insert, and every already loaded key outside that prefix remains in its existing tail. No pagination cursor is exposed in this phase. At terminal state Qt pages again from the stable origin until the rebuilt sorted prefix contains every still-present loaded representation, then atomically publishes that exact boundary and re-enables pagination. Compressed visuals are not stored in the Qt model: a forced-asynchronous `QQuickImageProvider` requests a verified cache blob only when Qt needs that image and decodes only the requested display size. Every image URL carries the current model generation, so a late result from an obsolete Library presentation is discarded.

## Desktop source index

`EditController` is the stable QObject/QML facade, with implementation grouped by responsibility:

- [`src/edit_controller.cpp`](src/edit_controller.cpp) owns adjustment interaction, Grade Node
  composition, geometry, and session-local edit history.
- [`src/edit_local_mask_controller.cpp`](src/edit_local_mask_controller.cpp) owns local-mask
  presentation, asset persistence, clipboard semantics, geometry validation, and brush strokes.
- [`src/edit_retouch_controller.cpp`](src/edit_retouch_controller.cpp) owns photo-level repair and
  clone picker state, continuous strokes, legacy spots, and source-offset editing.
- [`src/edit_persistence_coordinator.cpp`](src/edit_persistence_coordinator.cpp) owns photo
  open/close, autosave, version operations, and durable state transitions.
- [`src/edit_render_coordinator.cpp`](src/edit_render_coordinator.cpp) owns preview/detail
  scheduling, cancellation, analysis publication, and render presentation state.

Add a new edit workflow to its semantic owner and wire only its stable QML contract through
`edit_controller.hpp`; do not rebuild a monolithic controller implementation.

Import progress is intentionally absolute rather than a fabricated percentage: the scanner does not perform a separate counting walk. During active scanning the UI reports discovered/catalogued files and queued preview checks; exact completed, decode-failure, preview-failure, and cancelled-job counts are terminal summaries. The first catalogued batch can appear while enumeration and preview checks are still active, and those rows may already be opened in Precision. Manual decisions, Compare writes, Add Folder, and pagination remain disabled through the terminal stable-prefix refresh. Stop Import uses one cooperative token across enumeration and queued decode jobs; already registered assets remain durable, queued jobs skip provider work, and one in-flight provider call may finish. If cancellation reaches enumeration/catalog registration, that journal ends as `cancelled`; if it arrives only during the `PreparingPreviews` tail, enumeration may already be journaled `completed` while the desktop/FFI job still terminates `cancelled` and queued preview work stops.

When the preferred cached visual is JPEG, the core also queues a maximum-512-edge
display-luma observation on a separate bounded single-worker actor. Catalog commits
it only against the exact current source/artifact and algorithm/preprocessing
revision. The selected-photo sidebar shows mean and percentile luma,
near-black/near-white fractions, two scale-sensitive detail proxies, and the input
and analyzer revisions. These are single-photo display-proxy facts: the grid does
not turn them into quality badges, sorting, Picks, or Rejects, and the application
does not automatically compare, rank, or infer across differently
authored/preprocessed proxies.

## Review Decision Ledger vertical slice

Review exposes two independent manual fields for each logical photo: an
`Unflagged` / `Picked` / `Rejected` flag and a zero-through-five rating. `P`, `U`,
and `X` set the flag; `0` through `5` set the rating. Grid badges and stars are
projections of Catalog state, not inferred quality labels. Multiple loaded
representations of the same photo update together.

Each real full-state transition appends one immutable `Human` event with a global
sequence, UUIDv7 identity, timestamp, before state, and after state. Canonical
JSON, a BLAKE3 digest, indexed columns, foreign keys, and update/delete triggers
protect the ledger. The current table stores only each photo's forward-moving
head pointer. A write compares both the expected head and complete before state;
stale or no-op commands append nothing.

Undo is another transition to the prior state, never a deletion or head rewind.
The desktop session enables it only while the authoritative flag, rating, and
head sequence still equal the original command's after state. An intervening
external `Picked → Rejected → Picked` history therefore remains visible and
cannot be overwritten merely because the final values happen to match.

This ledger is deliberately separate from AI feedback. The current UI records
human state only; it does not create feature snapshots, train the preference
head, synthesize pairwise examples, or grant a model write access to Pick,
Reject, or rating. Color labels, bulk mutation, decision filtering, XMP
round-trip, and durable cross-session command undo remain follow-up work.

## Review Compare Evidence vertical slice

Review can place two already cached visuals side by side and ask for one explicit
`PairwiseOutcome`: `LeftPreferred`, `RightPreferred`, `KeepBoth`, `KeepNeither`,
or `CannotCompare`. Recording an outcome appends one human-feedback event in the
`Global` learning scope. It does not mutate either photo, assign a Pick/Reject,
or silently infer a label from merely opening or leaving the comparison.

Undo is deliberately non-destructive. It appends a forget fact targeting the
source feedback event; it does not update or delete that event. The evidence
counter shown by this first UI covers active, not-forgotten events from the
current application session, not a lifetime evidence total or a count of
model-training examples.

This is an evidence-capture surface, not a learned recommender. The presentation
records neither a group nor an active model, and both candidates currently carry
no feature snapshot. There is therefore no input with which to train the existing
preference head, no personal score, and no automatic ranking change.

Visual identity is no longer inferred from a photo or representation at click
time. Each Review row receives a session-authenticated opaque handle containing
the exact Catalog artifact selected for that page. Entering Compare creates two
purpose-specific request tickets. The image provider loads the frozen records'
verified content-addressed bytes, normalizes successful Qt decodes to
non-premultiplied RGBA8888, hashes only `width × 4` bytes from each row with
SHA-256, and returns those same frames to Qt Quick. Both frame receipts and QML
`Image.Ready` must agree with the pending presentation before an outcome can be
recorded. The feedback event freezes the full source/artifact identity, decoder
and surface revision, requested and decoded dimensions, pixel format, and frame
hash. It never re-queries the preferred visual during load or commit, so a later
artifact A → B change cannot rewrite historical evidence.

This provenance boundary ends at the normalized decoded frame. It does not claim
to preserve display ICC, GPU scaling, the window's final raster, or physical
screen pixels. The pre-release catalog has one supported development schema, v1.
Schema changes replace that shape and require resetting old local development
data; Shadow does not accumulate migration or compatibility chains before its
first stability promise.

Available display-luma observations may appear alongside each photo as parallel
technical facts. The UI does not subtract them, name a winner, or use them to
justify the human outcome. Camera-embedded previews and Shadow-generated proxies
can differ in upstream resizing, sharpening, tone, and color treatment, so their
metrics are not necessarily comparable even when the final analyzer revision is
the same. Their current Catalog revisions remain visible in the UI, but this
slice does not yet copy the separate technical-observation payload into the
feedback event.

## macOS development

The API baseline is Qt 6.11.1, matching the current stable development runtime.
Shadow follows the latest stable Qt minor instead of preserving compatibility
with an older baseline: upgrades advance only after the native build, QML lint,
desktop lifecycle smokes, and macOS/Windows visual checks pass. The current
Homebrew environment uses the smaller Qt 6.11 component set instead of the full
`qt` meta-package:

```sh
brew install qtdeclarative qttools
cargo xtask desktop-build
open build/desktop-dev/apps/desktop/Shadow.app
```

The development preset keeps assertions and debug-friendly native code. Use the
optimized preset for interactive photo editing and performance measurements:

```sh
cargo xtask desktop-release
open build/desktop-release/apps/desktop/Shadow.app
```

`shadow-desktop_qmllint` is generated by `qt_add_qml_module`. A headless startup check is available for CI and local diagnosis:

```sh
QT_QPA_PLATFORM=offscreen SHADOW_DESKTOP_SMOKE_TEST=1 \
  build/desktop-dev/apps/desktop/Shadow.app/Contents/MacOS/Shadow
```

`SHADOW_DESKTOP_SCAN_FOLDER=/absolute/folder` optionally starts one scan after launch. It is intended for local visual regression and does not bypass the folder picker in normal use.

Non-Release desktop builds also scan the ignored
`local-reference/sample-assets/raw/` fixture folder at startup when no explicit
`SHADOW_DESKTOP_SCAN_FOLDER` is supplied. The import is idempotent, so the
development Library automatically picks up newly added local DNG/RAW fixtures
without duplicating existing assets. Keeping this scope on the source fixtures
also prevents generated `raw-probe/` JPEG/PGM/PPM artifacts from entering the
Library. Release builds never embed or scan this repository-local path.

`SHADOW_DESKTOP_DATA_ROOT=/absolute/folder` overrides the local Catalog/cache directory for isolated smoke tests. Normal launches continue to use Qt's per-user application-data location.

Adding `SHADOW_DESKTOP_STREAMING_SCAN_SMOKE=1` proves that both the Review model and QML Grid become non-empty while `scanning` is still true, then requires `refreshing` to settle only after the terminal stable-prefix refresh. `SHADOW_DESKTOP_CANCEL_SCAN_SMOKE=1` requests cooperative cancellation after live progress begins and likewise waits for the final Library refresh before accepting a `cancelled` terminal snapshot. After a completed scan, launch the same isolated data root without `SHADOW_DESKTOP_SCAN_FOLDER` and add `SHADOW_DESKTOP_REOPEN_LIBRARY_SMOKE=1` to prove that the persisted Library appears without rescanning.

Adding `SHADOW_DESKTOP_OPEN_FIRST_EDIT=1` to a smoke run waits for the first scanned Review item, opens it through the real Precision controller, renders its processed linear-light RGB edit preview, validates all four 256-bin histogram sums and clipping bounds, and fails after 30 seconds if no generation-matched preview and analysis reach QML.

Adding `SHADOW_DESKTOP_REQUEST_BEFORE=1` to that edit smoke waits for a second, lazily requested neutral-import baseline and its independent analysis sidecar. This exercises the same warm decoded session without treating the baseline as unprocessed sensor data.

Adding `SHADOW_DESKTOP_GRADE_STACK_SMOKE=1` to the first-edit smoke runs a real
three-Grade-Node controller round trip: add, edit, duplicate, reorder, bypass,
render, save, close, reopen, and verify stable order, Grade Node and internal
Render Op IDs, parameters, and bypass state. Use a fresh
`SHADOW_DESKTOP_DATA_ROOT`; the smoke intentionally creates a named Recipe
version plus its atomic Library-wide commit in that isolated Catalog.

Adding `SHADOW_DESKTOP_FULL_DETAIL_SMOKE=1` to the first-edit smoke enters the
real level-zero detail path at the image center, prepares one bounded full-size
LibRaw reference-RGB session, renders the visible adaptive tile grid, assembles
one atomic RGB8 viewport presentation, and reads it back through the Qt image
provider. It does not save a Recipe, tile artifact, or Catalog fact.

Adding `SHADOW_DESKTOP_RECORD_FIRST_COMPARISON=1` to a smoke run with at least
two visuals prepares a real Compare presentation, requests both exact frames
through the image provider, confirms their receipts, and records a left-preferred
event. `SHADOW_DESKTOP_FORGET_RECORDED_COMPARISON=1` then appends its forget fact
before exit. Use a fresh `SHADOW_DESKTOP_DATA_ROOT` when inspecting the resulting
single event and fact in isolation.

Adding `SHADOW_DESKTOP_SET_FIRST_DECISION=1` to a smoke run waits for the first
Review row and records a real `Picked` decision while preserving its rating.
`SHADOW_DESKTOP_UNDO_FIRST_DECISION=1` then appends the inverse event before exit.
Use a fresh `SHADOW_DESKTOP_DATA_ROOT` to inspect the resulting two-event ledger
without changing a normal local Catalog.

The model fetches 96 metadata rows per page and requests another page near the end of the grid. Import publishes the first catalogued item immediately at the core boundary, then Qt polls at 150 ms and throttles live prefix reconciliation to at most every 400 ms with a 16-item stride. Pagination stays closed while paths are arriving. The terminal refresh reads stable pages from origin until it covers all keys that were already presented, so it neither drops a loaded tail nor resumes from a cursor whose preceding rows were not actually shown. An intermediate live-page failure cannot consume this terminal refresh; one explicit final attempt still runs. If that attempt fails, existing rows remain visible, pagination stays safely closed, and Add Folder or reopening Shadow can retry. Page membership is accepted before total/cursor/has-more metadata is committed. The keyed model update uses insert/move/remove/data-change signals rather than repeated model resets, preserving retained persistent indexes and avoiding unnecessary thumbnail reloads.

## Precision vertical slice

Double-clicking a Review item opens a real non-destructive Precision workspace:

```text
Qt sliders / named-version actions
  → EditController (debounce, generations, stale-result rejection)
  → shadow-desktop-bridge (Catalog-owned photo/source validation)
  → supported working Recipe + complete transient edit settings
  → ordered 1..16 layer typed render plan
  → per layer: Exposure → Contrast → [Tone Curve] → RGB Channel Gain → Saturation
  ├─ FIT / sub-100%: reusable 1200-edge processed linear-light RGB proxy (sRGB primaries)
  │    → pre-JPEG RGB/luma histogram + pre-clamp clipping sidecar
  │    → JPEG provider
  └─ 100%+: one immutable full-size u16 processed-linear RGB source (sRGB primaries) → display-encoded RGB8 tiles
       → one atomically published viewport image
```

Precision exposes an ordered stack of one through sixteen user-facing Grade Nodes. A Grade Node is one complete adjustment layer: its Light, Tone, and Color controls travel together. The left panel supports add, duplicate, delete, move, select, and enabled/bypassed operations; the final executable Grade Node cannot be deleted. The selected Grade Node's inspector exposes every current adjustment at once. Recipe v1 still lowers each Grade Node to a canonical four-Render-Op chain when its curve is absent and a five-Render-Op chain when the optional Tone Curve is present. Those Render Ops are execution details, not separate user nodes. Duplicate copies values, curve, and bypass state but receives a new Grade Node identity and five new Render Op identities. Reorder and bypass retain every existing identity and payload.

The controller treats the complete ordered stack—every stable identity, bypass flag, Basic parameter, and Tone Curve payload—as one edit-settings value. Structural commands and bypass toggles are discrete session-undo transitions; slider and curve gestures coalesce against the stable selected Grade Node ID. These undo/redo steps are deliberately separate from durable history. After a short idle debounce, every real edit writes an immutable Recipe snapshot and atomically advances only that photo's `working` ref. Closing Shadow waits for this autosave instead of asking the user to discard changes.

Creating a named version first validates stack-wide identity invariants, then stores an immutable Recipe v1 compatibility leaf inside the content-addressed Library tree. The per-photo compatibility Recipe commit and the Library-wide commit, `heads/main`, and both named-version refs publish in one SQLite transaction with mandatory compare-and-swap guards. A Library commit therefore names one comprehensive root that can include photo edits, shared Grade Node heads, masks, Styles, and output state; it is not a collection of unrelated per-slider commits. Autosave commits intentionally create no named ref and do not advance `heads/main`, so the Versions panel remains a concise list of human-created checkpoints. Object packs may be written before publication, but a stale CAS leaves them unreachable and rolls back both commits and every ref movement. Loading an older photo version creates only an in-memory draft and never moves either durable head. Editing that draft produces a new autosaved working branch; creating a named version from it advances from the latest Library root, so newer photo commits and unrelated Library state are not rewound.

Valid persisted curves contain 2 through 256 finite points with exact x endpoints at zero and one and strictly increasing x. They are loaded without clamping. Direct authoring is narrower by design: at most 32 points, y in `[0, 1]`, and an x gap of at least `1/4096` between neighbors; endpoint x positions are fixed. An extended-range, oversized, or too-tightly-spaced legacy curve remains preserved and visible but view-only. `Reset Curve` remains explicitly available to remove the selected Grade Node's curve Render Op without changing its other controls. Each Grade Node may own one piecewise-linear tone curve; this does not implement parameter curves, independent RGB channel curves, or the full Lightroom curve surface.

Precision also keeps a bounded, in-memory undo/redo history for the current edit session. All updates between a slider press/release or one curve-point drag are coalesced into one meaningful full-stack step; Grade Node add/duplicate/delete/reorder/bypass, point add/remove, curve reset, Grade Node reset, and revert are undoable transitions. Grade Node selection is transient UI state and does not make the Recipe dirty. Opening a photo or loading a saved version as a draft clears session history. Autosave preserves this session history; a named version establishes a new durable checkpoint and clears it. Standard Undo/Redo shortcuts use the platform mapping (`Cmd+Z` and `Cmd+Shift+Z` on macOS), with visible controls in the preview toolbar.

Each durable version row summarizes its parent-relative Recipe diff. Renderer-backed controls use readable labels such as `Exposure · Tone Curve · Saturation`; curve edits, additions, and resets share the stable `Tone Curve` change label, while topology and future adjustment types use semantic fallbacks without exposing internal parameter keys or commit identifiers. The current-version badge and exact parent count remain visible beside that summary.

RAW preparation is cached for up to two recent `(representation, source fingerprint, edge)` sessions. A slider, point-curve, structure, reorder, or Grade-Node-enabled update reruns the render plan and JPEG encoder; it does not reopen or decode the RAW. Undo and redo restore the complete working stack through the same generation-checked preview path. Continuous gestures use a leading-edge 16 ms throttle: they cannot postpone the first frame indefinitely. A render in flight does not disable controls; a newer revision is queued while the prior render finishes, and a completed same-photo intermediate frame may be presented without declaring the generation settled. Only the exact latest generation publishes histogram state or the current-status message. A deliberately bypassed selected Grade Node is different—the controls remain visible but read-only and dimmed so its preserved values stay inspectable. Preview buffers are bounded, rebuildable, and never become Catalog facts.

Every accepted warm-preview render now carries a transient analysis sidecar from the exact same Recipe execution and generation. The right inspector overlays 256-bin display-encoded sRGB R/G/B histograms and a fixed-point encoded Rec.709 luma outline computed from the uncompressed RGB8 proxy immediately before JPEG encoding. Shadow/highlight badges count pixels for which any edited processed-linear working-RGB channel is strictly below zero or above one before output clamping; exact zero and one are legal. Current and neutral Before have independent slots, and the panel remains dimmed as Updating or Stale until its generation matches the `Image.Ready` frame. This is complete-warm-proxy output analysis, not sensor-domain exposure, a full-resolution/viewport scope, or durable Catalog/Recipe/AI evidence. Nonlinear Tone Curve statistics remain an interactive proxy approximation for the same reason as the FIT image.

Before/After comparison uses two independently generation-checked preview slots. `After` is the current working stack; `Before` is the neutral import baseline produced by the same decoded processed linear-light RGB working proxy with one default enabled Grade Node and no Tone Curve. The backend enforces that neutral contract regardless of the caller's Grade Node count, order, bypass states, parameters, or curves. It is rendered only after the user first requests it and only after the latest current preview settles.

`FIT` and `100%` now have distinct meanings. FIT keeps the bounded warm proxy;
100% maps one processed photo pixel to one physical display pixel using Qt's
device-pixel ratio. Entering 100% lazily prepares a separate immutable full-size
16-bit processed-linear RGB reference buffer in sRGB primaries, capped at 512 MiB per retained source and cached
for only the active Catalog representation/fingerprint. The limit does not
claim to include LibRaw's transient decode allocations. A typical 45 MP
three-channel source retains about 260 MiB. It is not converted into one full-size float image: the backend
converts and executes the same complete Recipe only for the current level-zero
tile grid. Normal viewports use 512×512 tiles; viewports wider or taller than
4096 source pixels use 1024×1024 tiles so 5K/6K displays do not silently receive
a partial detail surface. Requests remain explicitly bounded to 8192 pixels per
axis. Tiles are tightly packed RGB8 rather than separately encoded
JPEGs; Qt receives one stitched viewport presentation, avoiding tile codec and
partial nonlinear-curve seams. Requests are rejected before a cold decode if
their worst-case grid exceeds 100 tiles; the assembled RGB presentation is
limited to 96 MiB and stays on the worker path. Qt's image provider retains that
same immutable byte storage instead of making another full viewport copy. The warm proxy remains underneath during cold
decode, pan, stale-result rejection, and errors.

Full detail has an independent `{photo, Recipe revision, viewport revision}`
acceptance contract at both controller and image-store boundaries. Changing a
photo or edit invalidates it immediately. Starting a pan hides the current
presentation; only the final viewport is queued after movement ends. The source
session is Recipe-independent and reusable across slider revisions, while the
RGB viewport is rebuildable memory state and never enters Catalog or durable
version history. A newer viewport or Recipe token stops the old worker between
tiles; generation checks still reject a result if cancellation races its final
tile. The first cold request still performs a complete LibRaw
demosaic because v1 deliberately does not depend on LibRaw crop semantics.

This is a full-resolution parity gate for the current pixel-local Basic nodes,
including nonlinear Tone Curve, but still uses LibRaw's camera-WB, processed
linear-light RGB reference output in sRGB primaries. It is not yet Shadow's final camera-domain color
pipeline, export renderer, ICC-managed display proof, mip pyramid, GPU backend,
or neighborhood-operation tile/halo system.

The current UI authors fixed, complete Grade Nodes rather than exposing arbitrary graph wiring.
Processed-RGB white balance now uses temperature/tint with a neutral-area picker; it remains
explicitly distinct from future Camera-domain RAW white balance. The application-wide LUT Library
can persist multiple source folders, recursively validate 3D `.cube` resources and expose stable
content identities in a dedicated manager. Valid resources are copied into an application-owned,
content-addressed store; each Grade Node can select one resource and an intensity directly from
the Precision inspector. The immutable Recipe retains the resource id, display title, managed
path, intensity, and a stable LUT render-op identity. Warm preview and full-detail rendering both
parse the exact managed `.cube`, apply trilinear interpolation in processed working RGB, and blend
the result by intensity; an unselected slot is an exact no-op. Only 3D `.cube` is supported, and
the file itself cannot declare whether it expects log, display-encoded, or another input space, so
the first contract deliberately treats it as a working-RGB LUT. Arbitrary Render Ops or
connections, shared Grade Node revisions, masks,
opacity/blend controls, clipping overlays, waveform/vectorscope, crop, full-resolution export,
and AI-authored stacks remain later vertical slices. Warm FIT preview and its analysis remain an
interactive approximation for nonlinear curves; 100% detail is the full-resolution quality gate
for the supported stack.
