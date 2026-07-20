# Shadow desktop

The first macOS Review slice is a native Qt Quick application backed by the existing Rust and C++ core:

```text
FolderDialog / QML Review grid
  → ReviewController (Qt UI thread + QtConcurrent job)
  → shadow-desktop-bridge (long-lived CXX session)
  → shadow-core scan / decode workers
  → shadow-catalog single writer
  → embedded preview or generated proxy cache
  → bounded display-luma observation worker → Catalog v9 summary

two signed exact-artifact handles / explicit outcome
  → compare-only request tickets → verified cache bytes
  → Qt decoded RGBA frame receipt → ReviewController evidence write
  → Catalog v9 append-only Global feedback / forget fact

Pick / Reject / 0–5 rating command
  → full-state expected-head CAS → immutable human decision event
  → forward-only current projection → append-only inverse-event undo
```

QML never opens SQLite, calls LibRaw, or interprets blob paths. The Rust bridge returns bounded Review metadata pages using a stable path/representation cursor. Compressed visuals are not stored in the Qt model: a forced-asynchronous `QQuickImageProvider` requests a verified cache blob only when Qt needs that image and decodes only the requested display size. Every image URL carries the current model generation, so a late result from a previous folder is discarded.

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
screen pixels. Legacy feedback remains readable with an explicitly absent visual
field. Catalog v8 was a marker-only payload-contract migration and never guesses
provenance from today's cache; current schema v9 retains that rule while adding
the separate manual decision ledger.

Available display-luma observations may appear alongside each photo as parallel
technical facts. The UI does not subtract them, name a winner, or use them to
justify the human outcome. Camera-embedded previews and Shadow-generated proxies
can differ in upstream resizing, sharpening, tone, and color treatment, so their
metrics are not necessarily comparable even when the final analyzer revision is
the same. Their current Catalog revisions remain visible in the UI, but this
slice does not yet copy the separate technical-observation payload into the
feedback event.

## macOS development

The API baseline remains Qt 6.8. The current Homebrew development environment uses the smaller Qt 6.11 component set instead of the full `qt` meta-package:

```sh
brew install qtdeclarative
cargo xtask desktop-build
open build/desktop-dev/apps/desktop/Shadow.app
```

`shadow-desktop_qmllint` is generated by `qt_add_qml_module`. A headless startup check is available for CI and local diagnosis:

```sh
QT_QPA_PLATFORM=offscreen SHADOW_DESKTOP_SMOKE_TEST=1 \
  build/desktop-dev/apps/desktop/Shadow.app/Contents/MacOS/Shadow
```

`SHADOW_DESKTOP_SCAN_FOLDER=/absolute/folder` optionally starts one scan after launch. It is intended for local visual regression and does not bypass the folder picker in normal use.

`SHADOW_DESKTOP_DATA_ROOT=/absolute/folder` overrides the local Catalog/cache directory for isolated smoke tests. Normal launches continue to use Qt's per-user application-data location.

Adding `SHADOW_DESKTOP_OPEN_FIRST_EDIT=1` to a smoke run waits for the first scanned Review item, opens it through the real Precision controller, renders its scene-linear edit preview, and fails after 30 seconds if no preview reaches QML.

Adding `SHADOW_DESKTOP_REQUEST_BEFORE=1` to that edit smoke waits for a second, lazily requested neutral-import baseline. This exercises the same warm decoded session without treating the baseline as unprocessed sensor data.

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

The model currently fetches 96 metadata rows per page and requests another page near the end of the grid. Scanning still completes before the first Catalog page is shown; streaming import progress and first-screen priority are separate follow-up work.

## Precision vertical slice

Double-clicking a Review item opens a real non-destructive Precision workspace:

```text
Qt sliders / named-version actions
  → EditController (debounce, generations, stale-result rejection)
  → shadow-desktop-bridge (Catalog-owned photo/source validation)
  → reusable 1600-edge scene-linear sRGB working proxy
  → supported working Recipe + complete transient edit settings
  → dependency-ordered typed render plan
  → Exposure → Contrast → [Tone Curve] → RGB Channel Gain → Saturation
  → asynchronously decoded JPEG image provider
```

Precision exposes four fixed slider groups plus a global point-curve editor over one `PHOTO`-scope `Basic Adjustments` layer. It is deliberately not a free-form Node Lab. The layer header also exposes its persisted enabled/bypassed state. Bypassing it makes the renderer skip the whole layer while keeping every slider value, optional Tone Curve point, and layer/node identity intact; the visible controls become read-only and dimmed until the layer is re-enabled. The persisted Recipe is a canonical linear chain with four nodes when the curve is neutral/absent and five when the optional Tone Curve is present. Adding the first adjustment point inserts that fifth node; `Reset Curve` removes it. Editing an existing curve preserves its node identity, and consecutive saves retain all other supported layer/node identities so structural diffs remain meaningful.

The controller treats the layer-enabled flag, Basic parameters, and complete Tone Curve payload as one edit-settings value. Preview requests, dirty comparison, session history, version save, reopen, and checkout therefore cannot silently combine layer state, sliders, or curve points from different revisions. Toggling bypass is an undoable edit and appears in durable parent-relative version diffs. Saving creates a new immutable Recipe commit while atomically moving the `working` ref and adding a named-version ref. Each preview is bound to the immutable base commit that supplied its settings, and each save compare-and-swaps the expected `working` head; a stale task fails instead of combining or overwriting two versions. Checking out an older commit preserves newer commits; the next save branches from the checked-out version.

Valid persisted curves contain 2 through 256 finite points with exact x endpoints at zero and one and strictly increasing x. They are loaded without clamping. Direct authoring is narrower by design: at most 32 points, y in `[0, 1]`, and an x gap of at least `1/4096` between neighbors; endpoint x positions are fixed. An extended-range, oversized, or too-tightly-spaced legacy curve remains preserved and visible but view-only. `Reset Curve` remains explicitly available to remove the curve node and return to the four-node Recipe without changing the sliders. This editor is one global piecewise-linear point curve in the tone stage. It does not implement parameter curves, independent RGB channel curves, a histogram, or the full Lightroom curve surface.

Precision also keeps a bounded, in-memory undo/redo history for the current edit session. All updates between a slider press/release or one curve-point drag are coalesced into one meaningful full-settings step; point add/remove, curve reset, slider reset, and revert are also undoable transitions. Opening or checking out a photo clears the session history. A successful save makes the complete saved settings the new dirty baseline and clears session undo/redo, because durable Recipe versions—not transient UI steps—own history across saves. Standard Undo/Redo shortcuts use the platform mapping (`Cmd+Z` and `Cmd+Shift+Z` on macOS), with visible controls in the preview toolbar.

Each durable version row summarizes its parent-relative Recipe diff. Renderer-backed controls use readable labels such as `Exposure · Tone Curve · Saturation`; curve edits, additions, and resets share the stable `Tone Curve` change label, while topology and future adjustment types use semantic fallbacks without exposing internal parameter keys or commit identifiers. The current-version badge and exact parent count remain visible beside that summary.

RAW preparation is cached for up to two recent `(representation, source fingerprint, edge)` sessions. A slider, point-curve, or layer-enabled update reruns the render plan and JPEG encoder; it does not reopen or decode the RAW. Undo and redo restore the complete working settings through the same generation-checked preview path. A render in flight does not disable controls: a newer revision is queued while the prior render finishes, and stale output is rejected. A deliberately bypassed layer is different—the controls remain visible but read-only and dimmed so their preserved values stay inspectable. Preview buffers are bounded, rebuildable, and never become Catalog facts.

Before/After comparison uses two independently generation-checked preview slots. `After` is the current working edit; `Before` is the neutral import baseline produced by the same decoded scene-linear working proxy with the Basic Adjustments layer enabled, default basic-adjustment parameters, and no Tone Curve. The backend enforces that neutral enabled contract regardless of the caller's working layer state, slider values, or curve. It is rendered only after the user first requests it and only after the latest current preview settles.

The current UI authors the enabled state, four global slider groups, and global Tone Curve of one fixed Basic Adjustments layer. Multiple independently authored layers, reordering, arbitrary nodes, Camera-domain white balance, parameter/per-channel curves, histogram overlays, masks, crop, full-resolution tiles, export, and shared scopes remain later vertical slices; the UI does not present placeholders for them. The warm proxy remains an interactive approximation for this nonlinear curve, so full-resolution parity still needs an explicit quality gate.
