# Shadow desktop

The first macOS Review slice is a native Qt Quick application backed by the existing Rust and C++ core:

```text
startup Catalog page / Add Folder / QML Review grid
  → ReviewController (Qt facade + cross-workflow admission)
  → ReviewImportCoordinator / ReviewLibraryQueryCoordinator
  → shadow-desktop-bridge (long-lived CXX session)
  → shadow-core controlled scan / cancellable decode workers
  → shadow-catalog single writer
  → embedded preview or generated proxy cache
  → bounded display-luma observation worker → Catalog v1 summary

two signed exact-artifact handles / explicit outcome
  → compare-only request tickets → verified cache bytes
  → Qt decoded RGBA frame receipt → ReviewComparisonCoordinator evidence write
  → Catalog v1 append-only Global feedback / forget fact

Pick / Reject / 0–5 rating command
  → full-state expected-head CAS → immutable human decision event
  → forward-only current projection → append-only inverse-event undo
```

QML never opens SQLite, calls LibRaw, or interprets blob paths. The global local Library loads its existing first page at startup; Add Folder starts a separate import job and no longer clears already visible photos. The Rust bridge returns bounded Review metadata pages using a stable path/representation cursor and exposes a generation-bound progress snapshot for Qt to poll. While import is changing sort order, each live first-page snapshot is reconciled as a prefix: matching rows move or update, new rows insert, and every already loaded key outside that prefix remains in its existing tail. No pagination cursor is exposed in this phase. At terminal state Qt pages again from the stable origin until the rebuilt sorted prefix contains every still-present loaded representation, then atomically publishes that exact boundary and re-enables pagination. Compressed visuals are not stored in the Qt model: a forced-asynchronous `QQuickImageProvider` requests a verified cache blob only when Qt needs that image and decodes only the requested display size. [`src/review_visual_request.hpp`](src/review_visual_request.hpp) owns the image-URL protocol: signed immutable grid requests may finish while the Library advances generations, whereas decoded-frame-receipt comparison requests remain strictly current-generation-bound.

## Desktop source index

Application startup is split from environment-driven automation:

- [`src/main.cpp`](src/main.cpp) owns process startup, isolated RAW-helper policy, local Catalog
  recovery, service composition, QML loading, and the application run loop.
- [`qml/Main.qml`](qml/Main.qml) owns application-window composition, workspace routing, theme
  projection, and the stable application-shell entry points used by child workspaces.
- [`qml/MainTitleBar.qml`](qml/MainTitleBar.qml) owns title-bar geometry, native window dragging,
  workspace navigation, edit save/undo state, settings entry, and the catalog-history popup as one
  application-shell interaction surface. It preserves the `Main` translation context.
- [`qml/MainStatusBar.qml`](qml/MainStatusBar.qml) owns the responsive bottom status and action
  surface: Library filters, current-selection decisions, progress, Precision proxy state, and
  workspace status projection. It preserves the `Main` translation context.
- [`qml/AutosaveFailureRecovery.qml`](qml/AutosaveFailureRecovery.qml) owns native-close
  interception plus the complete failed-save choice: retry, keep editing, discard only the
  in-memory draft and continue a queued photo open, or explicitly quit without saving.
- [`src/folder_scan_backend.cpp`](src/folder_scan_backend.cpp) owns folder-import admission,
  begin/scan/progress/cancel projection, cooperative cancellation, and terminal reporting while
  `DesktopBackend` preserves the stable compatibility methods. Its focused
  [`folder_scan_backend_contract_test.cpp`](tests/folder_scan_backend_contract_test.cpp) exercises
  cancellation and completion through the real desktop session.
- [`src/backend/edit_settings_projection.cpp`](src/backend/edit_settings_projection.cpp) owns the
  complete bidirectional Qt/CXX Grade Stack and edit-history wire mapping: optics, Grade Node
  identities and controls, masks, repair spots and strokes, geometry, versions, and all bounded
  vector-shape checks. `DesktopBackend` workflows consume this single projection instead of
  carrying their own field interpretation. Its
  [`backend_edit_settings_projection_test.cpp`](tests/backend_edit_settings_projection_test.cpp)
  verifies full-stack lossless round trips plus malformed vector rejection.
- [`src/desktop_smoke_harness.cpp`](src/desktop_smoke_harness.cpp) owns documented
  `SHADOW_DESKTOP_*` flag selection and shared scenario dispatch.
- [`src/desktop_smoke/edit_preview_session.cpp`](src/desktop_smoke/edit_preview_session.cpp)
  owns the first-photo Precision acceptance lifecycle: Review-row readiness, current preview and
  analysis, optional Before, optional complete level-zero viewport readback, deadline,
  diagnostics, and the unique process terminal.
- [`src/desktop_smoke/grade_stack_persistence.cpp`](src/desktop_smoke/grade_stack_persistence.cpp)
  owns the Grade Stack acceptance lifecycle from preview readiness through durable save,
  close/reopen verification, deadline, diagnostics, and process exit.

`EditController` is the stable QObject/QML facade, with implementation grouped by responsibility:

- [`src/edit_controller.cpp`](src/edit_controller.cpp) owns the stable facade, session
  composition, Grade Stack synchronization, and cross-workflow edit history.
- [`src/edit_adjustment_controller.cpp`](src/edit_adjustment_controller.cpp) owns Grade Node
  adjustment presentation and mutation, LUT, Color Mixer/Warper, grading, and Selective Color.
- [`src/edit_fine_parameter_registry.*`](src/edit_fine_parameter_registry.hpp) is the single
  inventory for scalar fine-adjustment keys, backend fields, writable bounds, and validation
  labels. Paired defringe endpoints remain read-only here and Point Color ranges remain owned by
  their selected-range model.
- [`src/edit_point_color_controller.cpp`](src/edit_point_color_controller.cpp) owns the complete
  preview color-sampling lifecycle: Point Color selection/scope/picker state, bounded sample
  creation, white-balance picker projection, localized failures, and the selected-range model.
- [`src/edit_point_color_model.hpp`](src/edit_point_color_model.hpp) owns the canonical
  primary-plus-additional Point Color representation shared by adjustment and stack synchronization.
- [`src/edit_geometry_controller.cpp`](src/edit_geometry_controller.cpp) owns crop-tool state,
  crop bounds and aspect ratios, straighten, rotation, flips, and geometry reset.
- [`src/edit_grade_node_controller.cpp`](src/edit_grade_node_controller.cpp) owns Grade Node list
  presentation, selection, enablement, collection actions, sharing, and node-level resets.
- [`src/edit_local_mask_controller.cpp`](src/edit_local_mask_controller.cpp) owns local-mask
  presentation, asset persistence, clipboard semantics, geometry validation, and brush strokes.
- [`src/edit_optics_controller.cpp`](src/edit_optics_controller.cpp) owns optical-correction state,
  automatic and manual profiles, residual controls, validation, history, and preview scheduling.
- [`src/edit_retouch_controller.cpp`](src/edit_retouch_controller.cpp) owns photo-level repair and
  clone picker state, continuous strokes, legacy spots, and source-offset editing.
- [`src/edit_tone_curve_controller.cpp`](src/edit_tone_curve_controller.cpp) owns Tone Curve
  presentation, point normalization and editing, gesture integration, history, and preview timing.
- [`src/edit_persistence_coordinator.cpp`](src/edit_persistence_coordinator.cpp) owns photo
  open/close, autosave, version operations, and durable state transitions.
- [`src/edit_render_coordinator.cpp`](src/edit_render_coordinator.cpp) owns current and neutral
  preview scheduling, cancellation, diagnostics, and presentation.
- [`src/edit_detail_render_controller.cpp`](src/edit_detail_render_controller.cpp) owns
  full-resolution viewport admission, cancellation, tile validation and publication, idle warmup,
  memory/readiness state, and Recipe-change invalidation.
- [`src/edit_analysis_controller.cpp`](src/edit_analysis_controller.cpp) owns histogram and
  display-scope validation/projection, Point Color reference freezing, analysis refresh,
  publication, failure, and clearing.

Add a new edit workflow to its semantic owner and wire only its stable QML contract through
`edit_controller.hpp`; do not rebuild a monolithic controller implementation.

Precision presentation follows the same responsibility tree:

- [`qml/EditHistogram.qml`](qml/EditHistogram.qml) owns analysis-mode controls, status,
  generation-aware labels, clipping badges, and the surrounding layout.
- [`qml/EditScopeData.qml`](qml/EditScopeData.qml) is the single defensive projection of backend
  analysis maps; [`qml/EditScopeCanvas.qml`](qml/EditScopeCanvas.qml) owns every histogram,
  waveform, RGB-parade, vectorscope, and skin-reference drawing algorithm.
- [`qml/PrecisionCanvas.qml`](qml/PrecisionCanvas.qml) owns the preview viewport, zoom/detail
  transport, overlays, and their stable workspace-facing state.
- [`qml/PrecisionCanvasToolbar.qml`](qml/PrecisionCanvasToolbar.qml) presents the current-photo,
  clipping, comparison, and zoom commands while emitting intent back to the viewport owner.
- [`qml/PrecisionComparisonSurface.qml`](qml/PrecisionComparisonSurface.qml) owns the complete
  visual comparison transaction inside that viewport: original-frame receipt, whole/wipe/dual
  layouts, divider input, and BEFORE/AFTER labels.
- [`qml/PrecisionInspector.qml`](qml/PrecisionInspector.qml) owns inspector composition, tool
  routing, analysis presentation, and the stable Adjust/Looks surface.
- [`qml/PrecisionLutSection.qml`](qml/PrecisionLutSection.qml) owns the complete managed-LUT
  browser: recursive directory projection, preview-provider identities, browser expansion,
  selection and clear actions, and the LUT-intensity gesture. An empty library contributes no
  synthetic explanation row; management remains an explicit adjacent action.
- [`qml/PrecisionPointColorSection.qml`](qml/PrecisionPointColorSection.qml) owns Point Color
  sampling and selection, Skin Check pending/locked scope transitions, tone-coherence admission,
  the bounded undoable hue nudge, and all six parameter gestures. The Inspector supplies the
  analysis surface but does not reopen that interaction lifecycle.
- [`qml/PrecisionCanvasPickerInput.qml`](qml/PrecisionCanvasPickerInput.qml) owns point-color and
  white-balance sampling plus repair spot/stroke gesture lifecycles without expanding the canvas
  composition surface.
- [`qml/PrecisionFoundationAdjustments.qml`](qml/PrecisionFoundationAdjustments.qml) owns White
  Balance, Light, Presence, foundational Color and Color Balance, plus the perceptual lightness
  Curve. These sections share one editor and parameter-gesture contract.
- [`qml/PrecisionColorMixer.qml`](qml/PrecisionColorMixer.qml) owns Color Mixer modes, hue-band
  controls, and their curve editors while keeping the inspector as a composition boundary.
- [`qml/PrecisionSelectiveColor.qml`](qml/PrecisionSelectiveColor.qml) owns selective-color
  target selection and CMYK adjustment presentation.

`ExportController` remains the stable QObject/QML facade, while the durable transaction has one
backend owner:

- [`src/export_controller.cpp`](src/export_controller.cpp) owns selection-to-destination planning,
  task-center presentation, cancellation requests, and preset persistence.
- [`src/backend/export_settings_codec.cpp`](src/backend/export_settings_codec.cpp) owns the export
  field names, defaults, clamps, validation, preset projection, and immutable settings JSON shared
  by the controller and executor.
- [`src/backend/export_backend.cpp`](src/backend/export_backend.cpp) owns queue recovery and claims,
  exact Recipe rendering, watermarking and encoding, write-conflict handling, atomic publication,
  terminal completion, cancellation, and progress on the application's single Rust session.

`DesktopBackend` composes that export component with the shared session but does not forward its
workflow operations. [`tests/backend_export_contract_test.cpp`](tests/backend_export_contract_test.cpp)
links the production component and verifies its settings schema plus an empty real durable queue.

Review presentation keeps the workspace as the composition and compatibility surface:

- [`qml/ReviewWorkspace.qml`](qml/ReviewWorkspace.qml) owns Review composition, selection
  compatibility routing, gallery presentation, and the stable triggers consumed by its toolbars
  and delegates.
- [`qml/ReviewLibrarySidebar.qml`](qml/ReviewLibrarySidebar.qml) owns the complete left Library
  navigation surface: system collections, album selection/management entry, import progress, and
  session comparison evidence. It receives only the workspace contract and album-dialog owner;
  explicit `ReviewWorkspace` translation context preserves the existing localized catalog while
  the component gains independent layout ownership.
- [`qml/ReviewSharedGradePicker.qml`](qml/ReviewSharedGradePicker.qml) owns the shared Grade Node
  selection popup, including refresh, bounded Overlay placement, application, and closure. Toolbar
  and context-menu callers supply only the requested presentation point.
- [`qml/ReviewMetadataPresentation.qml`](qml/ReviewMetadataPresentation.qml) owns locale-aware
  EXIF/RAW value formatting and the grouped metadata-field projection consumed by the metadata
  window. Selection ownership remains in `ReviewSelectionState`.
- [`src/review_controller.cpp`](src/review_controller.cpp) is the stable QML-facing composition
  index and request router. [`src/review_controller_backend_operations.*`](src/review_controller_backend_operations.hpp)
  owns every backend-to-coordinator operation adapter, while
  [`src/review_controller_connections.cpp`](src/review_controller_connections.cpp) owns the
  complete coordinator signal, invalidation, and status-routing topology.
- [`src/review_import_coordinator.cpp`](src/review_import_coordinator.cpp) owns one complete folder
  import after cross-workflow admission: scan identity, blocking worker lifetime, monotonic progress
  polling, cooperative cancellation, live-Library refresh pacing, terminal outcome, localized
  status, and destruction wait. `ReviewController` decides only whether another workflow permits
  the import and performs the requested page/source-health refreshes. Its
  [`tests/review_import_coordinator/`](tests/review_import_coordinator/) contracts cover progress
  projection, refresh pacing, cancellation, terminal failure, diagnostics, and lifetime.
- [`qml/ReviewSelectionState.qml`](qml/ReviewSelectionState.qml) owns identity-keyed multi-selection,
  the off-screen-safe Shift anchor, and the primary presentation snapshot. Detailed EXIF and
  technical facts come from an independent exact `{photo, representation}` request, so delegate
  recycling and Library pagination cannot replace the selected representation.
- [`qml/ReviewComparisonState.qml`](qml/ReviewComparisonState.qml) owns frozen left/right evidence
  snapshots, duplicate-photo rejection, prepared presentation tickets and sources, visual/backend
  readiness, local status, submission, cancellation, and terminal cleanup. Comparison surfaces
  navigate through this owner instead of reopening the lifecycle in `ReviewWorkspace`.
- [`src/review_comparison_coordinator.cpp`](src/review_comparison_coordinator.cpp) owns the complete
  Compare lifecycle after cross-workflow admission: exact presentation preparation, decoded-frame
  verification, cancellation, serialized record/forget workers, receipt validation, session-local
  undoability, terminal status, and destruction wait. `ReviewController` preserves the public Qt
  properties, methods, and signals while routing only the stable boundary. Its
  [`tests/review_comparison_coordinator/`](tests/review_comparison_coordinator/) suite keeps
  presentation, evidence, receipt-validation, and failure/lifetime contracts independently
  navigable behind one registered runner.
- [`src/review_decision_coordinator.cpp`](src/review_decision_coordinator.cpp) owns the complete
  append-only flag/rating mutation lifecycle after cross-workflow admission: current-state
  resolution, serialized backend writes, authoritative refresh after failure, receipt validation,
  causal session-local undo, localized status, and destruction wait. `ReviewController` projects
  accepted states into the Library model and arbitrates its shared decision/status channel. The
  responsibility-named
  [`tests/review_decision_coordinator/`](tests/review_decision_coordinator/) suite keeps admission
  and projection, failure and undo, and lifetime contracts independently navigable.
- [`src/review_photo_inspection_coordinator.cpp`](src/review_photo_inspection_coordinator.cpp) owns
  the complete asynchronous selected-photo lifecycle: exact request coalescing, terminal failure,
  explicit retry, and presentation. Its
  [`review_photo_inspection_session.hpp`](src/review_photo_inspection_session.hpp) child owns only
  independent request generation and stale-completion rejection; neither contract observes the
  Library page generation. Focused coordinator and session tests cover rapid reselection, clear,
  failure/retry, and same-identity refresh.
- [`src/review_source_health_coordinator.cpp`](src/review_source_health_coordinator.cpp) owns the
  complete Library source-health review lifecycle: serialized health refreshes, scan-scoped
  missing-location paging, stale-page rejection, exact user-selected relink workers, localized
  status, and destruction wait. Folder scanning only requests a health refresh at its terminal
  boundary; it does not share this state machine. The responsibility-named
  [`tests/review_source_health_coordinator/`](tests/review_source_health_coordinator/) suite covers
  refresh coalescing and projection, review switching/closing and keyset continuation, plus relink
  admission, receipts, errors, and lifetime.
  [`review_source_health_backend_contract_test.cpp`](tests/review_source_health_backend_contract_test.cpp)
  additionally runs two completed scans through the real desktop session, pages the resulting
  missing-location evidence, and proves that a wrong complete-file identity cannot relink it.
- [`src/review_library_album_coordinator.cpp`](src/review_library_album_coordinator.cpp) owns the
  complete Library album lifecycle: authoritative album snapshots and selection, serialized
  refresh/CRUD/membership workers, refresh coalescing, deleted-selection invalidation, localized
  terminal status, and destruction wait. `ReviewController` supplies the current smart-album
  filter and reacts only to selection or selected-membership query invalidation. Its
  [`tests/review_library_album_coordinator/`](tests/review_library_album_coordinator/) contracts
  cover projection, smart-query freezing, deduplicated membership, coalescing, failure, and
  lifetime.
- [`src/review_library_facet_coordinator.cpp`](src/review_library_facet_coordinator.cpp) owns the
  generation-bound Capture Month, Camera, and Lens facet projection. It fetches all three bounded
  dimensions from one immutable filter snapshot, rejects stale generations, coalesces refreshes
  onto the latest input, publishes localized failures, and waits for its worker at destruction.
  Its [`tests/review_library_facet_coordinator/`](tests/review_library_facet_coordinator/)
  contracts cover the shared filter/bound, projection, stale replacement, failure, and lifetime.
- [`src/review_library_organization_coordinator.cpp`](src/review_library_organization_coordinator.cpp)
  owns complete per-photo Like and color-label mutation: cross-workflow admission, current-state
  synthesis, serialized persistence, receipt identity validation, authoritative model projection,
  localized status, and destruction wait. The controller only routes the stable QML calls and
  reacts to a successful projection by updating public signals and any active server filter. Its
  [`tests/review_library_organization_coordinator/`](tests/review_library_organization_coordinator/)
  contracts cover normalization, coupled-state preservation, no-op admission, projection,
  diagnostics, invalid receipts, and lifetime.
- [`src/review_library_query_coordinator.cpp`](src/review_library_query_coordinator.cpp) owns the
  generation-bound Library page and count projection: one immutable active filter, keyset cursor,
  reset debounce/coalescing, page/count workers, stale-result rejection, cursor validation,
  snapshot/prefix/append reconciliation, query status, and destruction wait. `ReviewController`
  supplies filter snapshots and cross-workflow state while the coordinator emits only facet,
  decision, status, and work-state boundaries. Its
  [`tests/review_library_query_coordinator/`](tests/review_library_query_coordinator/) contracts
  cover projection, pagination, reset coalescing, stale completion, failures, and lifetime.
- [`src/review_model.cpp`](src/review_model.cpp) owns the photo-keyed Qt row projection, stable QML
  roles, visual-generation URLs, and reset/prefix/append reconciliation. Its single test runner
  routes to responsibility-named contracts under
  [`tests/review_model/`](tests/review_model/) for roles, mutable Library/decision state, visual
  generations, item-field identity, and snapshot membership; executable test bodies do not live
  in the runner or production model.
- [`src/review_shared_grade_coordinator.cpp`](src/review_shared_grade_coordinator.cpp) owns the
  authoritative shared Grade Node snapshot and batch-link boundary: QML target normalization,
  duplicate-photo rejection, complete mutation receipts, localized status, and visible-Library
  invalidation only after an actual update. `ReviewController` retains only the stable public Qt
  routing. Its
  [`tests/review_shared_grade_coordinator/`](tests/review_shared_grade_coordinator/) contracts
  cover snapshot projection, normalized apply calls, partial receipts, admission, and failures.
- [`src/photo_inspection_projection.cpp`](src/photo_inspection_projection.cpp) is the sole
  production mapping from the complete Rust FFI inspection DTO to the desktop DTO.
  [`tests/backend_photo_inspection_contract_test.cpp`](tests/backend_photo_inspection_contract_test.cpp)
  exercises that mapping with distinct sentinel values for every identity, presence flag, unit,
  metadata field, and technical metric, then retains an absent exact-pair test through the real
  Catalog/FFI/backend path.
- [`qml/LibraryAlbumDialogs.qml`](qml/LibraryAlbumDialogs.qml) owns the complete create, membership,
  rename, and delete dialog lifecycle plus temporary form state. The album coordinator remains the
  authoritative owner of data, selection, and mutations behind the stable controller facade.

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

Every `desktop-build`, `desktop-check`, and `desktop-release` first runs the exact translation
contract. It re-extracts production messages from `qml/` and `src/`, requires the Simplified
Chinese catalog to have exactly one finished, non-empty entry for every message and no stale
entries, verifies placeholder multiplicity, and compiles the result with `lrelease`. Run
`cargo xtask desktop-i18n-check` directly when changing UI text or translations.

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
render, save, close, reopen, and verify stable Grade Node order and identities,
parameters, and bypass state. Use a fresh
`SHADOW_DESKTOP_DATA_ROOT`; the smoke intentionally creates a named Recipe
version plus its atomic Library-wide commit in that isolated Catalog.

Adding `SHADOW_DESKTOP_FULL_DETAIL_SMOKE=1` to the first-edit smoke enters the
real level-zero detail path at the image center, prepares one bounded full-size
LibRaw reference-RGB session, renders the visible adaptive tile grid, assembles
one atomic RGB8 viewport presentation, and reads the complete generation-bound
grid back through the Qt image provider as display-sRGB. When
`SHADOW_DESKTOP_REQUEST_BEFORE=1` is also present, the same runner accepts
Current, Before, and full detail in sequence. It does not save a Recipe, tile
artifact, or Catalog fact.

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
