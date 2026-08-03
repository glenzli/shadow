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

QML never opens SQLite, calls LibRaw, or interprets blob paths. The global local Library loads its existing first page at startup; Add Folder starts a separate import job and no longer clears already visible photos. The first page owns startup priority on the serialized Catalog boundary: aggregate count, facets, albums, keywords, source health, and shared Grade Nodes begin only after that page has been projected, so secondary navigation cannot delay visible photos. The Rust bridge returns bounded Review metadata pages using a stable path/representation cursor and exposes a generation-bound progress snapshot for Qt to poll. While import is changing sort order, each live first-page snapshot is reconciled as a prefix: matching rows move or update, new rows insert, and every already loaded key outside that prefix remains in its existing tail. No pagination cursor is exposed in this phase. At terminal state Qt pages again from the stable origin until the rebuilt sorted prefix contains every still-present loaded representation, then atomically publishes that exact boundary and re-enables pagination. Compressed visuals are not stored in the Qt model: a forced-asynchronous `QQuickImageProvider` requests a verified cache blob only when Qt needs that image and decodes only the requested display size. [`src/review_visual_request.hpp`](src/review_visual_request.hpp) owns the image-URL protocol: signed immutable grid requests may finish while the Library advances generations, whereas decoded-frame-receipt comparison requests remain strictly current-generation-bound.

## Desktop source index

Application startup is split from environment-driven automation:

- [`src/main.cpp`](src/main.cpp) owns process startup, isolated RAW-helper policy, local Catalog
  recovery, service composition, QML loading, and the application run loop.
- [`qml/Main.qml`](qml/Main.qml) owns application-window composition, workspace routing, theme
  projection, and the stable application-shell entry points used by child workspaces.
- [`qml/MainTitleBar.qml`](qml/MainTitleBar.qml) owns title-bar geometry, native window dragging,
  workspace navigation, edit save/undo state, settings entry, and the History Drawer trigger. It
  preserves the `Main` translation context; [`qml/HistoryDrawer.qml`](qml/HistoryDrawer.qml) owns
  the per-photo durable Recipe timeline, named-version creation and non-destructive checkout, plus
  the read-only Library commit/ref timeline.
- [`qml/MainStatusBar.qml`](qml/MainStatusBar.qml) composes the responsive bottom status surface
  and workspace status projection. [`qml/MainLibraryFilterBar.qml`](qml/MainLibraryFilterBar.qml)
  owns Library filter mutations and status-row alignment,
  [`qml/MainLibrarySortMenu.qml`](qml/MainLibrarySortMenu.qml) owns the anchored Shadow-styled
  capture-date/file-name order menu,
  [`qml/MainPrecisionProxyStatus.qml`](qml/MainPrecisionProxyStatus.qml) owns read-only proxy
  state presentation. Every child uses the stable `Main` translation context explicitly.
- [`qml/ApplicationSettingsDialog.qml`](qml/ApplicationSettingsDialog.qml) is the single modal
  settings shell and section router. Its General, Library, AI, Storage, and Map panes remain
  separate QML owners so the shell does not accumulate domain behavior. The former compact menu
  and standalone cache-maintenance window no longer form alternate settings paths.
  [`qml/ShadowCheckBox.qml`](qml/ShadowCheckBox.qml) and
  [`qml/ShadowSwitch.qml`](qml/ShadowSwitch.qml) own the compact checkbox and toggle presentation
  used throughout the packaged desktop module; feature panes retain only their domain semantics.
  Together with `Theme.qml` and the remaining `Shadow*` primitives they form the desktop's internal
  control library inside the packaged `Shadow.App` QML module. Keep feature-specific state and
  workflows outside these controls. A separate `Shadow.Controls` module is deferred until its
  packaging benefit outweighs the import and focused-test migration across existing consumers.
  [`qml/MetadataFieldSelectorRow.qml`](qml/MetadataFieldSelectorRow.qml) owns one metadata field's
  row interaction and consumes `ShadowCheckBox`; [`qml/MetadataWindow.qml`](qml/MetadataWindow.qml)
  keeps only field grouping and preference orchestration.
- [`src/ui_preferences.*`](src/ui_preferences.hpp) owns appearance, language, Library thumbnail,
  and EXIF-field presentation preferences. [`src/ai_preferences.*`](src/ai_preferences.hpp) owns
  admission policy for new local AI work plus the default strength of newly authored RAW-denoise
  nodes; model discovery and verification remain with the model runtimes.
  [`src/personal_profile.*`](src/personal_profile.hpp) is deliberately separate from application
  settings: it owns the device-local nickname, normalized avatar, and a bounded set of
  provider-independent living-place rules with optional inclusive month ranges.
  [`qml/PersonalProfileDialog.qml`](qml/PersonalProfileDialog.qml) edits that context from the
  title-bar avatar without adding an account or upload path;
  [`qml/PersonalLivingPlacesEditor.qml`](qml/PersonalLivingPlacesEditor.qml) owns the multi-place
  period interaction rather than growing the dialog into another state-machine hub.
  [`src/personal_location_search.*`](src/personal_location_search.hpp) independently owns bounded,
  asynchronous search over the packaged offline city index, including rapid-query coalescing and
  stale-result rejection. [`qml/PersonalLocationSearchField.qml`](qml/PersonalLocationSearchField.qml)
  combines that manual search with optional Library-derived shortcuts; only an explicitly selected
  canonical locality identity reaches the profile owner.
  [`src/cache_preferences.*`](src/cache_preferences.hpp) owns the persistent soft disk-cache target
  and permission for automatic safe reclamation. [`src/cache_maintenance_controller.*`](src/cache_maintenance_controller.hpp)
  may enforce that target only through the Catalog-proven unused-preview sweep: live, unknown,
  recently protected, and AI RAW foundation data may keep actual use above the requested target.
- [`src/map_provider_preferences.*`](src/map_provider_preferences.hpp) owns optional external
  map-service permissions, the derived Library basemap readiness/style, and the native-only Google
  credential lifecycle. With no permitted service it publishes an explicit `none` provider instead
  of silently selecting an unavailable basemap.
  [`src/secure_secret_store.*`](src/secure_secret_store.hpp) is the narrow platform credential
  boundary: macOS stores the key as a device-local generic password in Keychain, isolated smoke
  sessions use volatile memory, and unsupported platforms fail closed without a plaintext
  fallback. [`qml/MapProviderSettingsPane.qml`](qml/MapProviderSettingsPane.qml) may save or remove
  a key and edit non-secret permissions inside the application settings shell, but it has no
  key-read property. [`qml/MapProviderSettingsDialog.qml`](qml/MapProviderSettingsDialog.qml) is a
  thin compatibility wrapper for focused component loading rather than a second policy owner.
- [`src/map/google_map_tiles_service.*`](src/map/google_map_tiles_service.hpp) owns the opt-in
  Google Map Tiles session, visible-only request queue, bounded policy-aware memory cache,
  `ETag` revalidation, backoff, cancellation, and viewport copyright lifecycle. It never installs
  a disk cache or starts before both a stored key and explicit tile permission are present.
  [`src/map/google_map_tiles_protocol.*`](src/map/google_map_tiles_protocol.hpp) owns the wire,
  error, and HTTP cache contracts; [`src/map/google_map_tile_geometry.*`](src/map/google_map_tile_geometry.hpp)
  owns Web Mercator visible-tile projection; and
  [`src/map/google_map_tile_layer.*`](src/map/google_map_tile_layer.hpp) paints those decoded
  tiles without taking gesture or photo-marker ownership.
- [`src/geonames_city_index.*`](src/geonames_city_index.hpp) owns the bounded, latitude-sorted
  offline country/administrative-area/city data contract, nearest-city query, and bounded
  token-based name search ranked by fit and population. The compact index
  is reproducibly derived by [`scripts/prepare_geonames_city_index.py`](../../scripts/prepare_geonames_city_index.py)
  from GeoNames `cities500`, country, and first-level administrative exports; it is loaded on first
  use off the UI thread and retained for later coordinates. [`src/geonames_library_reverse_geocoder.*`](src/geonames_library_reverse_geocoder.hpp)
  projects those matches into the provider-neutral place result, while
  [`src/library_reverse_geocoder_router.*`](src/library_reverse_geocoder_router.hpp) keeps offline
  city lookup as the default and chooses Google only after explicit user authorization. An online
  failure falls back to the local city index; [`src/default_library_reverse_geocoder.*`](src/default_library_reverse_geocoder.hpp)
  is the narrow production composition boundary. [`src/google_library_reverse_geocoder.*`](src/google_library_reverse_geocoder.hpp)
  separately owns the authorized Google Geocoding API request, bounded response parsing,
  cancellation, and safe diagnostics. It reuses the Keychain-backed Maps Platform key and never
  starts merely because a key exists. No Google user sign-in is involved; the key's Cloud project
  must enable billing and the Geocoding API.
  [`src/review_library_place_resolution_coordinator.*`](src/review_library_place_resolution_coordinator.hpp)
  starts only after the first Library page is visible, keeps Catalog work off the UI thread,
  serializes provider calls, publishes retryable progress/failure state, and records only
  coordinate-still-current results. Structured places feed country/city facets and the selected
  photo's Location presentation without overwriting camera or user-authored metadata.
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
- [`src/desktop_smoke/scenario_setup.cpp`](src/desktop_smoke/scenario_setup.cpp) owns first-photo,
  first-comparison, and first-decision readiness wiring.
- [`src/desktop_smoke/application_lifecycle.cpp`](src/desktop_smoke/application_lifecycle.cpp)
  owns normal close, dirty autosave close, and live language-switch acceptance.
- [`src/desktop_smoke/library_lifecycle.cpp`](src/desktop_smoke/library_lifecycle.cpp) owns import
  cancellation, progressive-grid visibility, and persisted-Library reopen acceptance, while
  [`src/desktop_smoke/review_mutations.cpp`](src/desktop_smoke/review_mutations.cpp) owns Review
  decision/comparison completion and undo/forget outcomes.
- [`src/desktop_smoke/edit_preview_session.cpp`](src/desktop_smoke/edit_preview_session.cpp)
  owns the first-photo Precision acceptance lifecycle: Review-row readiness, current preview and
  analysis, optional Before, optional complete level-zero viewport readback, deadline,
  diagnostics, and the unique process terminal.
- [`src/desktop_smoke/grade_stack_persistence.cpp`](src/desktop_smoke/grade_stack_persistence.cpp)
  owns the Grade Stack acceptance lifecycle from preview readiness through durable save,
  close/reopen verification, deadline, diagnostics, and process exit.

`desktop_backend.hpp` is the stable Qt/backend facade index. Its DTO contracts are owned by
[`src/backend/review_types.hpp`](src/backend/review_types.hpp),
[`src/backend/library_types.hpp`](src/backend/library_types.hpp),
[`src/backend/edit_types.hpp`](src/backend/edit_types.hpp), and
[`src/backend/cache_types.hpp`](src/backend/cache_types.hpp); follow the domain header before
changing a wire shape.

Its implementation follows the same navigation:

- [`src/desktop_backend.cpp`](src/desktop_backend.cpp) owns session composition, folder scan, and
  exact selected-photo inspection.
- [`src/desktop_backend_library.cpp`](src/desktop_backend_library.cpp) owns Library queries,
  facets, albums, source-health evidence, relinking, mutable Library organization, non-destructive
  capture/GPS corrections, batch capture-time preview/apply, and GPX preview/apply projection.
- [`src/desktop_backend_review.cpp`](src/desktop_backend_review.cpp) owns Review visuals,
  comparison receipts, feedback, and explicit decision mutation.
- [`src/desktop_backend_edit.cpp`](src/desktop_backend_edit.cpp) owns Precision state, shared
  Grade Nodes, preview/detail rendering, and durable edit transitions.
- [`src/desktop_backend_cache.cpp`](src/desktop_backend_cache.cpp) owns export-service access and
  conservative cache inventory/maintenance.
- [`src/desktop_backend_history.cpp`](src/desktop_backend_history.cpp) and
  [`src/backend/history_projection.*`](src/backend/history_projection.hpp) own the bounded CXX/Qt
  projection for per-photo Recipe history, Library-wide commits, refs, keyset cursors, and semantic
  diff counts. [`src/history_coordinator.*`](src/history_coordinator.hpp) owns lazy asynchronous
  loading, pagination, stale-photo rejection, and worker lifetime; [`src/history_model.*`](src/history_model.hpp)
  owns only the two read-only QML list projections.

`EditController` is the stable QObject/QML facade, with implementation grouped by responsibility:

- [`src/edit_controller.cpp`](src/edit_controller.cpp) owns the stable facade, session
  composition, property projection, and localization.
- [`src/edit_history_controller.cpp`](src/edit_history_controller.cpp) owns gesture coalescing,
  undo/redo, reset/revert, Grade Stack synchronization, and dirty/autosave transitions for the
  current editing session. [`src/edit_history_restore_projection.*`](src/edit_history_restore_projection.hpp)
  classifies restored node-list and local-mask changes and resolves selection after an inserted
  node is undone, keeping those UI notification invariants independently testable.
- [`src/edit_adjustment_controller.cpp`](src/edit_adjustment_controller.cpp) owns Grade Node
  adjustment presentation and mutation, LUT, Color Mixer/Warper, grading, and Selective Color.
- [`src/edit_adjustment_reset_controller.cpp`](src/edit_adjustment_reset_controller.cpp) owns
  atomic section-level neutralization so every panel-header reset is one undoable history entry.
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
  presentation, selection, visibility/bypass state, collection actions, sharing, and node-level
  resets. Foundation, Grade, and fixed photo-node rows use the same eye affordance for this
  non-destructive visibility meaning; the AI panel's result-generation checkbox remains separate.
- [`src/edit_processing_stack_controller.cpp`](src/edit_processing_stack_controller.cpp) owns
  explicit add/remove and bypass transitions for optional fixed-order photo nodes. AI RAW Denoise
  and Canvas are absent from a new stack until added; they remain single-use, photo-private, and
  non-reorderable. Model execution stays in the RAW Foundation controller and authored framing
  stays in the geometry controller.
- [`src/edit_mask_assignment_controller.cpp`](src/edit_mask_assignment_controller.cpp) owns the
  atomic choice between attaching a new mask to the selected empty node and creating, masking,
  inserting, and selecting one new node. QML never chains those state mutations.
- [`src/edit_local_mask_controller.cpp`](src/edit_local_mask_controller.cpp) owns local-mask
  presentation, in-session clipboard semantics, the enumerable scalar-parameter contract,
  geometry/condition validation, and brush strokes. Ordinary photo-local masks are not named or
  persisted as a separate reusable asset library.
- [`src/edit_mask_coverage_controller.cpp`](src/edit_mask_coverage_controller.cpp) owns the
  selected-node coverage request lifecycle: tool, photo, node, and mask invalidation; monotonic
  selection identity; exact preview pairing; and transient provider publication. Renderer-owned
  coverage samples and their generation contract live in
  [`src/edit_mask_coverage_contract.hpp`](src/edit_mask_coverage_contract.hpp).
- [`src/edit_foundation_controller.cpp`](src/edit_foundation_controller.cpp) owns the singleton,
  photo-local RAW Foundation white-balance authoring lifecycle, exact camera-neutral validation,
  history keys, and coalesced prepared-source preview scheduling. RAW temperature/tint changes the
  camera-domain development plan, so a drag is debounced instead of queueing repeated
  decode/demosaic work. It does not reuse the selected Grade Node's fast relative RGB
  white-balance controls.
- [`src/edit_raw_foundation_controller.*`](src/edit_raw_foundation_controller.hpp) owns the
  non-blocking AI RAW Denoise model probe, job polling, cancellation, stale-photo rejection,
  terminal retirement, source-noise advisory projection, and the fixed photo-local node's
  undoable bypass/strength transitions. The pure staged-Bayer estimator lives in
  `crates/shadow-desktop-bridge/src/raw_foundation_noise_assessment.rs`; it cancels planar scene
  gradients, reports confidence, and never admits or executes the model.
  [`src/edit_raw_foundation_state.*`](src/edit_raw_foundation_state.hpp) is its Qt-free generation
  state machine. Materialized artifacts remain rebuildable Rust-owned cache state: bypassing the
  node never deletes or serializes an artifact path, and strength changes never rerun the model.
  [`providers/rawnind-foundation/`](providers/rawnind-foundation/README.md) owns the reproducible
  self-contained provider build and optional desktop-bundle copy contract. Public model weights
  remain side-loaded in the versioned application-data model directory; a missing provider or
  model is an explicit unavailable state and never selects a fallback pixel route.
- [`src/edit_optics_controller.cpp`](src/edit_optics_controller.cpp) owns optical-correction state,
  automatic and manual profiles, residual controls, validation, history, and preview scheduling.
- [`src/edit_retouch_controller.cpp`](src/edit_retouch_controller.cpp) owns photo-level repair and
  clone picker state, continuous strokes, legacy spots, automatic Heal donors, and source-offset
  editing for both Heal and Clone.
- [`src/edit_liquify_controller.cpp`](src/edit_liquify_controller.cpp) owns the photo-private
  singleton Liquify projection, node bypass, Push/Reconstruct brush mode, and one-gesture/one-
  history boundary. Push prefers the local display mesh, then falls back to the same provisional
  authoritative lifecycle as Reconstruct when a live texture is unavailable or the prior Push is
  still awaiting replacement. Provisional Recipe state becomes durable only on release.
  [`src/edit_liquify_coordinates.cpp`](src/edit_liquify_coordinates.cpp) owns the exact post-
  Canvas-to-original coordinate inversion. `EditController` remains only their stable QObject/QML
  facade rather than absorbing the gesture semantics.
- [`src/edit_tone_curve_controller.cpp`](src/edit_tone_curve_controller.cpp) owns Tone Curve
  presentation, point normalization and editing, gesture integration, history, and preview timing.
- [`src/edit_persistence_coordinator.cpp`](src/edit_persistence_coordinator.cpp) owns photo
  open/close, autosave, version operations, and durable state transitions. A named Version
  requested during a non-blocking autosave is queued behind that exact snapshot and keeps explicit
  state interaction locked; it must never be accepted by the UI and then silently discarded.
- [`src/edit_render_coordinator.cpp`](src/edit_render_coordinator.cpp) owns current and neutral
  preview scheduling, cancellation, diagnostics, and presentation. During interaction it
  publishes the bridge's shared immutable frame owner; settled and neutral frames retain their
  encoded proxy contract for analysis and durable publication.
- [`src/backend/edit_preview_frame.hpp`](src/backend/edit_preview_frame.hpp) is the small read-only
  RGB8/paired-R8 owner contract. [`src/backend/rust_owned_edit_preview_frame.cpp`](src/backend/rust_owned_edit_preview_frame.cpp)
  is its only Rust-Box adapter, so presentation tests do not depend on generated bridge types.
- [`src/edit_preview_store.cpp`](src/edit_preview_store.cpp) owns generation-guarded immutable
  preview snapshots. [`src/edit_preview_presentation_context.*`](src/edit_preview_presentation_context.hpp)
  atomically publishes the root scene graph's window, graphics API, device, and epoch from render-
  thread lifecycle signals. [`src/edit_preview_presentation_registry.*`](src/edit_preview_presentation_registry.hpp)
  is the explicit application-composition owner for those two services: `Main` passes it through
  required QML properties to every live preview item. It has no process-global lookup, can be
  cleared or reconfigured, and existing items fail closed when its composition disappears. Each
  configuration has a monotonic runtime revision so a same-generation replacement cannot reuse a
  texture created for the prior composition.
- [`src/edit_preview_texture_item.*`](src/edit_preview_texture_item.hpp) owns only live interactive
  RGB presentation. Its custom QSG texture node resolves an immutable frame on the GUI thread,
  validates the scene-graph epoch/window/API/device and imports the owned Metal texture on the
  render thread without calling `QQuickTextureFactory::image()`, materializing RGB, or uploading
  through the CPU. The texture's child guard retains the native frame after the temporary factory
  is destroyed. Host storage, software rendering, stale epochs, and invalid native descriptors use
  the named materialize/upload fallback. A QSG node cache hit requires the same source, frame
  owner, presentation binding, and registry revision; a changed identity destroys the stale
  texture before publishing readiness for its replacement. A monotonic source-binding revision
  also prevents a hidden node from being reused after a suspend/resume roundtrip. Item resource
  release and window scene-graph invalidation synchronously or atomically invalidate that identity,
  revoke presented readiness, and retain the live owner plus settled fallback for reconstruction.
  The queued readiness receipt carries that complete identity as well, so a same-generation
  replacement or destroyed node cannot be acknowledged by an older render callback. Both
  comparison items track the current source, but explicit live
  admission is mutually exclusive: entering dual comparison suspends the main item without
  discarding its last settled fallback, and leaving dual comparison waits for a fresh main-item
  import above that fallback. This dedicated item is
  necessary because Qt Quick
  `Image` may inspect a texture factory through `image()` while resolving color space, which would
  force a full-frame readback before native import.
  [`src/edit_preview_liquify_mesh.*`](src/edit_preview_liquify_mesh.hpp) owns the bounded
  display-only Scene Graph grid for one active Push gesture. Pointer samples deform vertex
  positions while reusing the settled texture; the outer ring stays pinned, cancellation restores
  the quad, and the next authoritative preview generation retires a committed transient mesh.
  Reconstruct deliberately does not use this push-only approximation. A pending local mesh never
  blocks a following gesture or a Push/Reconstruct mode switch.
- [`src/edit_preview_provider.*`](src/edit_preview_provider.hpp) retains settled JPEG, scope/R8,
  full-detail, and explicit image-readback responsibilities. The last settled `Image` remains
  underneath the live item during interaction, so fallback and generation transitions do not
  expose an empty canvas. [`src/edit_mask_coverage_store.cpp`](src/edit_mask_coverage_store.cpp)
  publishes paired Alpha8
  coverage from that exact same owner only when preview, recipe, node, and selection generations
  agree.
- [`src/edit_detail_render_controller.cpp`](src/edit_detail_render_controller.cpp) owns
  full-resolution viewport admission, cancellation, tile validation and publication, idle warmup,
  memory/readiness state, and Recipe-change invalidation.
- [`src/edit_analysis_controller.cpp`](src/edit_analysis_controller.cpp) owns histogram and
  display-scope validation/projection, Point Color reference freezing, analysis refresh,
  publication, failure, and clearing.
- [`src/edit_stroke_input.*`](src/edit_stroke_input.hpp) validates the bounded normalized-point
  transport shared by direct brush gestures. QML owns transient sampling, while Local Mask and
  Retouch controllers each commit one complete gesture to their own recipe domain.

Add a new edit workflow to its semantic owner and wire only its stable QML contract through
`edit_controller.hpp`; do not rebuild a monolithic controller implementation.

Precision presentation follows the same responsibility tree:

- [`qml/EditHistogram.qml`](qml/EditHistogram.qml) is the analysis presentation index and owns
  stable public analysis aliases plus data/canvas/summary composition.
- [`qml/EditScopeData.qml`](qml/EditScopeData.qml) is the single defensive projection of backend
  analysis maps; [`qml/EditScopeCanvas.qml`](qml/EditScopeCanvas.qml) owns every histogram,
  waveform, RGB-parade, vectorscope, and skin-reference drawing algorithm.
- [`qml/EditScopeToolbar.qml`](qml/EditScopeToolbar.qml) owns analysis mode, vectorscope filter,
  freshness, and proxy-identity controls; [`qml/EditScopeClipSummary.qml`](qml/EditScopeClipSummary.qml)
  owns the complete pre-clamp shadow/highlight summary and tooltip formatting.
- [`qml/PrecisionCaptureMetadata.qml`](qml/PrecisionCaptureMetadata.qml) owns capture identity and
  setting formatting instead of making the Inspector composition root interpret camera fields.
- [`qml/PrecisionCanvas.qml`](qml/PrecisionCanvas.qml) owns the preview viewport, zoom/detail
  transport, tool/comparison surface composition, and their stable workspace-facing state.
- [`qml/PrecisionDetailLoupe.qml`](qml/PrecisionDetailLoupe.qml) presents one non-persistent
  focus/manual picture-in-picture viewport from the existing full-detail tile transport; camera
  AF metadata selects its initial center. [`qml/PrecisionDetailLoupeState.qml`](qml/PrecisionDetailLoupeState.qml)
  owns crop/orientation mapping, pointer follow/pin state, bounded detail requests, and the
  image-center fallback while the Canvas remains the composition boundary.
- [`qml/PrecisionCanvasStatusOverlays.qml`](qml/PrecisionCanvasStatusOverlays.qml) owns only
  comparison/detail/loading/error HUD presentation above that viewport. It preserves the last
  usable frame during work, reveals explanatory preview/detail status only after a short delay,
  and deliberately retains the `PrecisionWorkspace` translation context. The detail loupe uses
  the same delayed-status rule without creating another render or task-state owner.
- [`qml/PrecisionRetouchOverlay.qml`](qml/PrecisionRetouchOverlay.qml) is the retouch-overlay
  composition index. Continuous swept-disc painting lives in
  [`qml/PrecisionRetouchStrokeCoverage.qml`](qml/PrecisionRetouchStrokeCoverage.qml), its
  clone-source gesture in
  [`qml/PrecisionRetouchStrokeHandle.qml`](qml/PrecisionRetouchStrokeHandle.qml), and legacy
  point-repair target/source interaction in
  [`qml/PrecisionRetouchSpotHandle.qml`](qml/PrecisionRetouchSpotHandle.qml). The overlay and
  [`qml/PrecisionRetouchTools.qml`](qml/PrecisionRetouchTools.qml) share one transient
  stroke-or-spot selection through the workspace composition boundary.
  [`qml/PrecisionRetouchRegionPicker.qml`](qml/PrecisionRetouchRegionPicker.qml) owns compact
  collection navigation, while
  [`qml/PrecisionRetouchRegionInspector.qml`](qml/PrecisionRetouchRegionInspector.qml) owns the
  selected region's size, feather, mode, and removal gestures. Any number of authored regions
  therefore feeds one inspector rather than one repeated control tree per region.
- [`qml/PrecisionActiveStrokeCoverage.qml`](qml/PrecisionActiveStrokeCoverage.qml) owns only the
  incremental swept-area feedback for the pointer gesture currently in flight. Mask and Retouch
  input keep that feedback independent of preview generation churn, then perform one history and
  preview mutation when the pointer is released. Persistent coverage remains with the feature-
  specific overlays.
- [`qml/PrecisionLiquifyOverlay.qml`](qml/PrecisionLiquifyOverlay.qml) owns Liquify pointer
  sampling, admission, and a high-contrast radius/hardness brush cursor. The live deformation is
  the gesture feedback, so Liquify never paints a coverage trail over the photograph. Preview
  readiness admits a new gesture but a brief readiness transition cannot cancel an already
  captured gesture. Push uses the local mesh when possible and otherwise streams through the same
  authoritative interactive rendering path as Reconstruct; release remains the single history/
  persistence boundary.
  [`qml/PrecisionLiquifyTools.qml`](qml/PrecisionLiquifyTools.qml) owns Push/Reconstruct selection,
  next-gesture radius, strength, hardness, undo, and whole-node removal controls.
- [`qml/PrecisionCropOverlay.qml`](qml/PrecisionCropOverlay.qml) retains its direct manipulation
  and explicit full-surface cursor while a replacement preview frame is rendering; the last
  presented frame remains the valid geometry surface during that transition.
- [`qml/PrecisionCanvasToolbar.qml`](qml/PrecisionCanvasToolbar.qml) presents the current-photo,
  clipping, comparison, and zoom commands while emitting intent back to the viewport owner.
- [`qml/PrecisionGradeNodePane.qml`](qml/PrecisionGradeNodePane.qml) owns Grade Node navigation,
  ordering, enablement, and collection actions; [`qml/PrecisionGradeNodeMenus.qml`](qml/PrecisionGradeNodeMenus.qml)
  owns sharing and node collection popup lifecycles.
- [`qml/PrecisionMaskCreateMenu.qml`](qml/PrecisionMaskCreateMenu.qml) owns mask-kind and
  current-node/new-node destination choice. The global tool defaults to a new node, a node-row
  entry defaults to that node, and an existing mask is edited rather than silently replaced.
  Geometry masks and Oklab-lightness/Oklch-hue condition masks use the same atomic transaction.
- [`qml/PrecisionLocalMaskTools.qml`](qml/PrecisionLocalMaskTools.qml) owns only the selected
  node mask's semantic geometry/range parameters, inversion, removal, and in-session copy/paste
  controls; QML never interprets the compact condition-mask transport slots.
- [`qml/PrecisionMaskCoverageOverlay.qml`](qml/PrecisionMaskCoverageOverlay.qml) presents the
  renderer's exact selected-node R8 coverage with the theme mask tint and rejects a texture whose
  paired preview identity is no longer visible. Its view-only `O` toggle hides the tint without
  discarding that exact coverage or changing the mask. [`qml/PrecisionLocalMaskOverlay.qml`](qml/PrecisionLocalMaskOverlay.qml)
  retains direct-manipulation handles plus only a temporary continuous brush capsule while exact
  native coverage is unavailable.
- [`qml/PrecisionComparisonSurface.qml`](qml/PrecisionComparisonSurface.qml) owns the complete
  visual comparison transaction inside that viewport: original-frame receipt, whole/wipe/dual
  layouts, divider input, and BEFORE/AFTER labels.
- [`qml/PrecisionInspector.qml`](qml/PrecisionInspector.qml) owns inspector composition, tool
  routing, analysis presentation, the icon-only keep-and-exit action, and the stable Adjust/Looks
  surface.
- [`qml/PrecisionResetAllDialog.qml`](qml/PrecisionResetAllDialog.qml) owns the explicit
  destructive reset confirmation; the Inspector requests it and the editor remains the sole
  Recipe mutation and undo-history authority.
- [`qml/PrecisionLutSection.qml`](qml/PrecisionLutSection.qml) owns the complete managed-LUT
  browser: recursive directory projection, preview-provider identities, browser expansion,
  selection and clear actions, and the LUT-intensity gesture. An empty library contributes no
  synthetic explanation row; management remains an explicit adjacent action.
- [`qml/PrecisionPointColorSection.qml`](qml/PrecisionPointColorSection.qml) owns Skin Check
  pending/locked scope transitions, tone-coherence admission, the bounded undoable hue nudge, and
  all six Point Color parameter gestures.
  [`qml/PrecisionPointColorSampleBar.qml`](qml/PrecisionPointColorSampleBar.qml) owns the sampled
  range collection, picker admission action, selection, removal, and visible sampling guidance.
  The Inspector supplies the analysis surface but does not reopen either interaction lifecycle.
- [`qml/PrecisionCanvasPickerInput.qml`](qml/PrecisionCanvasPickerInput.qml) owns point-color and
  white-balance sampling plus repair spot/stroke gesture lifecycles without expanding the canvas
  composition surface.
- [`qml/PrecisionRawDenoiseAdjustments.qml`](qml/PrecisionRawDenoiseAdjustments.qml) presents the
  optional fixed AI RAW Denoise node: one concise source-noise recommendation, explicit Generate,
  materialize/retry/cancel state, and the cached-result strength gesture. The node-row eye and
  panel-header reset are reversible bypasses; the recommendation never auto-generates or applies
  the model, and the component contains no Foundation or JPEG terminology.
- [`qml/PrecisionFoundationAdjustments.qml`](qml/PrecisionFoundationAdjustments.qml) presents
  photo-local RAW white balance separately from the selected Grade Node's relative RGB white
  balance, then owns Light, Presence, foundational Color and Color Balance, plus the perceptual
  lightness Curve. These sections share one editor, parameter-gesture contract, and uniform
  panel-header reset affordance.
- [`qml/PrecisionColorMixer.qml`](qml/PrecisionColorMixer.qml) owns Color Mixer modes, hue-band
  controls, and their curve editors while keeping the inspector as a composition boundary.
- [`qml/PrecisionSelectiveColor.qml`](qml/PrecisionSelectiveColor.qml) owns selective-color
  target selection and CMYK adjustment presentation.
- [`qml/PrecisionTechnicalTools.qml`](qml/PrecisionTechnicalTools.qml) is the ordered composition
  index for technical adjustments. [`qml/PrecisionDetailSection.qml`](qml/PrecisionDetailSection.qml)
  owns sharpening and noise reduction,
  [`qml/PrecisionOpticsSection.qml`](qml/PrecisionOpticsSection.qml) owns profile matching and
  residual optical correction, and
  [`qml/PrecisionEffectsSection.qml`](qml/PrecisionEffectsSection.qml) owns finishing grain and
  post-crop vignette.

Review presentation keeps the workspace focused on selection and orchestration:

- [`qml/ReviewGallerySurface.qml`](qml/ReviewGallerySurface.qml) owns grid and single-photo
  presentation, incremental paging, comparison, empty/busy states, and the sole selected-photo
  decision-toolbar placement. [`qml/ReviewDecisionToolbar.qml`](qml/ReviewDecisionToolbar.qml)
  owns that floating pick/reject/rating/Like/color interaction contract across grid and single-photo
  presentation; the application status bar does not duplicate it.
- [`qml/ReviewGalleryToolbar.qml`](qml/ReviewGalleryToolbar.qml) owns gallery layout and batch
  command presentation while emitting external popup/navigation intents.
- [`qml/ReviewLibrarySidebar.qml`](qml/ReviewLibrarySidebar.qml) is the Library-side navigation
  index. [`qml/ReviewSystemCollections.qml`](qml/ReviewSystemCollections.qml) owns built-in
  collection selection, [`qml/ReviewAlbumList.qml`](qml/ReviewAlbumList.qml) owns album loading
  and commands, [`qml/ReviewImportProgressCard.qml`](qml/ReviewImportProgressCard.qml) owns one
  import/refresh receipt, and
  [`qml/ReviewComparisonEvidence.qml`](qml/ReviewComparisonEvidence.qml) owns session evidence
  summary and undo.
- [`qml/ReviewPhotoCard.qml`](qml/ReviewPhotoCard.qml) and
  [`qml/ReviewSinglePreview.qml`](qml/ReviewSinglePreview.qml) own grid-card and filmstrip
  geometry. They share [`qml/ReviewPhotoAffinity.qml`](qml/ReviewPhotoAffinity.qml) for Like/star
  evidence and [`qml/ShadowRoundedImage.qml`](qml/ShadowRoundedImage.qml) for true rounded image
  clipping, so the two browsing modes keep one visual contract without sharing interaction state.
  A grid card whose original is currently unreachable keeps its cached visual and presents an
  explicit missing badge; [`qml/LibraryMissingPhotoDialogs.qml`](qml/LibraryMissingPhotoDialogs.qml)
  owns the stable relink picker and non-destructive Library-removal confirmation after the
  virtualized card has released its context menu.
- [`qml/ReviewPhotoInspector.qml`](qml/ReviewPhotoInspector.qml) is the selected-photo scrolling
  index. [`qml/ReviewPhotoSummary.qml`](qml/ReviewPhotoSummary.qml) owns visual identity,
  [`qml/ReviewExifSection.qml`](qml/ReviewExifSection.qml) owns configurable metadata and retry,
  and [`qml/ReviewComparisonSlots.qml`](qml/ReviewComparisonSlots.qml) owns comparison admission,
  assignment, clearing, and entry.

Library management uses the same page-composition boundary:

- [`qml/LibrarySourceHealthPane.qml`](qml/LibrarySourceHealthPane.qml) owns source-scan evidence,
  non-destructive scan-root removal, missing-location paging, and exact-content relink
  confirmation. Removing a root hides photos available only through that root while preserving
  their photo, edit, Catalog-location, and source-file records; adding the root again restores them.
- [`qml/LibraryImportPane.qml`](qml/LibraryImportPane.qml) owns catalog count, folder admission,
  and observable import activity.

`ExportController` remains the stable QObject/QML facade, while the durable workflow is split by
admission, background execution, preset persistence, and backend publication:

- [`qml/ExportDialog.qml`](qml/ExportDialog.qml) owns modal export lifecycle, destination
  admission, progress, failures, and completion.
- [`qml/ExportSettingsPane.qml`](qml/ExportSettingsPane.qml) owns the editable option draft and
  exact backend projection; [`qml/ExportPresetMenus.qml`](qml/ExportPresetMenus.qml) owns preset
  naming and removal transactions.

- [`src/export_controller.cpp`](src/export_controller.cpp) owns selection-to-destination planning,
  task-center presentation, cancellation requests, and the stable QML facade. Its localized status
  projection retranslates in place when the application language changes.
- [`src/export_task_runner.cpp`](src/export_task_runner.cpp) owns the durable background drain:
  recovery, queue claims, cancellation, item execution, progress receipts, and terminal results.
- [`src/export_preset_store.cpp`](src/export_preset_store.cpp) owns preset identity, normalization,
  settings persistence, and runtime retranslation of built-in names while preserving user names.
- [`src/backend/export_settings_codec.cpp`](src/backend/export_settings_codec.cpp) owns the export
  field names, defaults, clamps, validation, preset projection, and immutable settings JSON shared
  by the controller and executor.
- [`src/backend/export_backend.cpp`](src/backend/export_backend.cpp) owns queue recovery and claims,
  exact Recipe rendering, watermarking and encoding, write-conflict handling, atomic publication,
  terminal completion, cancellation, and progress on the application's single Rust session.

`DesktopBackend` composes that export component with the shared session but does not forward its
workflow operations. [`tests/backend_export_contract_test.cpp`](tests/backend_export_contract_test.cpp)
links the production component and verifies its settings schema plus an empty real durable queue;
[`tests/export_preset_store_test.cpp`](tests/export_preset_store_test.cpp) verifies preset
normalization, persistence, stable built-in identities, runtime retranslation, and preservation of
user-authored names.

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
  window. [`qml/LibraryMetadataEditor.qml`](qml/LibraryMetadataEditor.qml) edits effective capture
  time and coordinates without writing the source file, while
  [`qml/LibraryCaptureTimeBatchDialog.qml`](qml/LibraryCaptureTimeBatchDialog.qml) previews and
  explicitly applies a shared clock shift or restoration to the current camera times, and
  [`qml/LibraryGpxImportDialog.qml`](qml/LibraryGpxImportDialog.qml) owns GPX selection,
  clock-offset settings, match summary, and explicit confirmation. Selection ownership remains in
  `ReviewSelectionState`.
- [`src/review_controller.cpp`](src/review_controller.cpp) is the stable QML-facing composition,
  property projection, and localization index. Request admission and routing are partitioned into
  [`src/review_controller_inspection.cpp`](src/review_controller_inspection.cpp),
  [`src/review_controller_comparison.cpp`](src/review_controller_comparison.cpp),
  [`src/review_controller_decisions.cpp`](src/review_controller_decisions.cpp),
  [`src/review_controller_library_query.cpp`](src/review_controller_library_query.cpp), and
  [`src/review_controller_library_management.cpp`](src/review_controller_library_management.cpp).
  [`src/review_controller_library_metadata.cpp`](src/review_controller_library_metadata.cpp)
  routes only manual metadata, batch capture-time, and GPX commands.
  [`src/review_controller_backend_operations.*`](src/review_controller_backend_operations.hpp)
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
- [`src/review_library_metadata_coordinator.cpp`](src/review_library_metadata_coordinator.cpp)
  keeps metadata reads, manual corrections, batch capture-time preview/apply, GPX parsing/preview,
  and confirmed batch application off the GUI thread. Decoder EXIF remains the immutable
  observation; the Catalog materializes an indexed effective projection, so rescans preserve user
  corrections and Library sort/facets use the corrected time and location.
- [`src/review_source_health_coordinator.cpp`](src/review_source_health_coordinator.cpp) owns the
  complete Library source-health review lifecycle: serialized health refreshes, scan-scoped
  missing-location paging, stale-page rejection, asynchronous source removal, exact user-selected
  relink workers from either scan evidence or a current unavailable grid location, non-destructive
  logical-photo removal, localized status, and destruction wait. Folder scanning only requests a
  health refresh at its terminal boundary; it does not share this state machine. The responsibility-named
  [`tests/review_source_health_coordinator/`](tests/review_source_health_coordinator/) suite covers
  refresh coalescing and projection, review switching/closing and keyset continuation, plus relink
  admission, receipts, errors, and lifetime.
  [`review_source_health_backend_contract_test.cpp`](tests/review_source_health_backend_contract_test.cpp)
  additionally runs two completed scans through the real desktop session, pages the resulting
  missing-location evidence, and proves that a wrong complete-file identity cannot relink it.
- [`src/review_library_query_coordinator.cpp`](src/review_library_query_coordinator.cpp) carries
  Library order with the filter generation and validates an order-typed continuation cursor.
  Future Group By is a separate query dimension: capture month, camera, and lens headers must be
  produced by the indexed Catalog query with a stable group cursor and an explicit within-group
  order, never by regrouping only the currently loaded page in QML.
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
- [`src/review_travel_collection_coordinator.*`](src/review_travel_collection_coordinator.hpp)
  independently derives the private Travel navigation projection from all configured living-place
  periods. A photo in one of those cities is ordinary life only when its capture month falls in
  that rule; other resolved places are grouped by country then destination, while unresolved
  locations and unknown dates in a time-bounded home are never guessed. It does not mutate photo
  metadata. [`qml/ReviewTravelCollections.qml`](qml/ReviewTravelCollections.qml) applies the exact
  generated Travel predicate when the user selects that hierarchy.
- [`src/review_library_keyword_coordinator.*`](src/review_library_keyword_coordinator.hpp) owns
  hierarchical taxonomy refresh, selected-photo assignment projection, serialized batch
  mutations, stale-selection rejection, localized outcomes, and destruction wait.
  [`qml/LibraryKeywordPanel.qml`](qml/LibraryKeywordPanel.qml) owns the shared hierarchy,
  assignment, and include-all/exclude-any filter interaction;
  [`qml/LibraryKeywordDialogs.qml`](qml/LibraryKeywordDialogs.qml) owns taxonomy mutation
  confirmation; and [`qml/LibraryKeywordPopup.qml`](qml/LibraryKeywordPopup.qml) is the bounded
  Review entry surface. The Library management view composes the same panel so organization and
  retrieval cannot drift into separate keyword semantics.
- [`qml/LibraryMapView.qml`](qml/LibraryMapView.qml) owns the on-demand map composition, gestures,
  coordinates, location placement, and photo-cluster interaction. One transparent Qt Location
  item-overlay map remains the only interaction/coordinate owner; Shadow's Google raster layer is
  the optional basemap beneath the same markers.
  [`qml/LibraryMapProviderOverlay.qml`](qml/LibraryMapProviderOverlay.qml) separately owns provider
  readiness guidance, visible-photo/busy projection, Google attribution, and localized provider
  errors. The local Catalog, not the tile service,
  applies the current Library filter and aggregates effective GPS coordinates through
  [`src/review_library_map_coordinator.cpp`](src/review_library_map_coordinator.cpp); viewport
  requests are bounded, coalesced, stale-safe, and never perform geocoding. Shadow intentionally
  embeds no provider API key. A user-supplied key plus explicit Google 2D tile permission activates
  the session-based basemap while the
  Library map is visible. Google tiles remain memory-only and obey response cache directives;
  dynamic viewport copyright is shown beside a distinct `Google Maps` attribution. Country,
  administrative-area, and nearest-city enrichment is local by default and never depends on map
  loading. Google precision lookup and place search remain separate opt-in contracts; provider
  results do not alter the basemap.
  [`qml/LibraryMapLocationPlacementState.qml`](qml/LibraryMapLocationPlacementState.qml) separately
  owns one selected photo's placement identity, pending coordinate, explicit confirmation, and
  retryable failure lifecycle. It delegates persistence to the existing metadata coordinator, so
  clicking the map never mutates Catalog state until the user confirms.
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
- [`qml/LibraryAlbumDialogs.qml`](qml/LibraryAlbumDialogs.qml) is the stable album-dialog router.
  [`qml/LibraryAlbumCreateDialog.qml`](qml/LibraryAlbumCreateDialog.qml) owns manual/condition
  creation and form state; [`qml/LibraryAlbumMembershipDialog.qml`](qml/LibraryAlbumMembershipDialog.qml)
  owns selected-photo admission and Manual Album choice;
  [`qml/LibraryAlbumManageDialog.qml`](qml/LibraryAlbumManageDialog.qml) owns rename and delete
  escalation; and [`qml/LibraryAlbumDeleteDialog.qml`](qml/LibraryAlbumDeleteDialog.qml) owns the
  explicit destructive confirmation. The album coordinator remains the authoritative owner of
  data, selection, and mutations behind the stable controller facade.

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

Offline country/administrative-area/city lookup uses a prepared GeoNames index. Download
`cities500.zip`, `countryInfo.txt`, and `admin1CodesASCII.txt` from the official
[`export/dump`](https://download.geonames.org/export/dump/) directory into one external source
folder, then prepare and bundle the deterministic index:

```sh
python3 scripts/prepare_geonames_city_index.py \
  --source-dir /absolute/geonames-source \
  --output /absolute/shadow-geonames-cities-v1.tsv \
  --dataset-version geonames-YYYY-MM-DD
cmake --preset desktop-dev \
  -DSHADOW_GEONAMES_CITY_INDEX_PATH=/absolute/shadow-geonames-cities-v1.tsv
```

The index header includes a SHA-256 identity over all three exact source files. Preparation never
contacts the network, and the application never uploads coordinates unless the user separately
allows Google precision lookup. Canonical `run_debug.sh` promotion requires both the index and its
GeoNames attribution notice, so the normal developer entry cannot silently regress to an empty
location provider.

The development preset keeps assertions and debug-friendly native code. Use the
optimized preset for interactive photo editing and performance measurements:

```sh
cargo xtask desktop-release
open build/desktop-release/apps/desktop/Shadow.app
```

The daily workflow smoke normally builds and uses the external `desktop-release` preset. To
validate an already-built task-private bundle without touching the shared default build directory,
set both `SHADOW_DAILY_USE_SKIP_DESKTOP_BUILD=1` and the absolute external
`SHADOW_DAILY_USE_DESKTOP_EXECUTABLE=/path/to/Shadow.app/Contents/MacOS/Shadow`. Relative paths and
paths inside the source tree are rejected.

`shadow-desktop_qmllint` is generated by `qt_add_qml_module`. A headless startup check is available for CI and local diagnosis:

```sh
QT_QPA_PLATFORM=offscreen SHADOW_DESKTOP_SMOKE_TEST=1 \
  build/desktop-dev/apps/desktop/Shadow.app/Contents/MacOS/Shadow
```

`SHADOW_DESKTOP_SCAN_FOLDER=/absolute/folder` optionally starts one scan after launch. It is intended for local visual regression and does not bypass the folder picker in normal use.

Non-Release desktop builds can scan the ignored
`local-reference/sample-assets/raw/` fixture folder by setting
`SHADOW_DESKTOP_AUTO_SCAN_SAMPLES=1` when no explicit
`SHADOW_DESKTOP_SCAN_FOLDER` is supplied. This is deliberately opt-in so a
normal debug launch cannot silently restore a Library folder the user removed.
The import is idempotent and remains limited to source fixtures, preventing
generated `raw-probe/` JPEG/PGM/PPM artifacts from entering the Library.
Release builds never embed or scan this repository-local path.

`SHADOW_DESKTOP_DATA_ROOT=/absolute/folder` overrides the local Catalog/cache directory for isolated smoke tests. Normal launches continue to use Qt's per-user application-data location.

Adding `SHADOW_DESKTOP_STREAMING_SCAN_SMOKE=1` proves that both the Review model and QML Grid become non-empty while `scanning` is still true, then requires `refreshing` to settle only after the terminal stable-prefix refresh. `SHADOW_DESKTOP_CANCEL_SCAN_SMOKE=1` requests cooperative cancellation after live progress begins and likewise waits for the final Library refresh before accepting a `cancelled` terminal snapshot. After a completed scan, launch the same isolated data root without `SHADOW_DESKTOP_SCAN_FOLDER` and add `SHADOW_DESKTOP_REOPEN_LIBRARY_SMOKE=1` to prove that the persisted Library appears without rescanning.

Adding `SHADOW_DESKTOP_OPEN_FIRST_EDIT=1` to a smoke run waits for the first scanned Review item, opens it through the real Precision controller, renders its processed linear-light RGB edit preview, validates all four 256-bin histogram sums and clipping bounds, and fails after 30 seconds if no generation-matched preview and analysis reach QML.

Adding `SHADOW_DESKTOP_METAL_PREVIEW_SMOKE=1` to that edit smoke performs two real slider
transactions around a root scene-graph invalidation/recreation. Both live generations must import
owned Metal textures, retain their frame owners after factory destruction, advance to the recreated
epoch, and record exactly one import for the one active surface in each epoch, with zero RGB
materializations, CPU uploads, or fallback reasons before the gesture ends. The phase reset
distinguishes that legal post-recreation import from an illegal hidden same-epoch duplicate.
`SHADOW_DESKTOP_SOFTWARE_PREVIEW_SMOKE=1` is the corresponding software-adaptation contract:
Qt retains its CPU scene graph across `releaseResources()`, so the smoke re-shows the window and
requires exactly one named materialize/upload fallback for the active surface in each phase, with
no native import.

Adding `SHADOW_DESKTOP_RAPID_PREVIEW_SMOKE=1` runs 96 rapid exposure samples through the same edit
session and requires the final settled preview pixels and histogram to belong to the last requested
generation. Set `SHADOW_DESKTOP_EDIT_SOURCE_PATH=/absolute/source/path` to select the exact scanned
Review item used by an edit smoke instead of relying on Library order.

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

The controller treats the complete ordered stack—every stable identity, bypass flag, Basic parameter, Tone Curve payload, the independent AI RAW Denoise node, and Foundation intent—as one edit-settings value. The Qt/CXX projection preserves the path-free AI RAW denoise enabled/model/strength tuple separately from RAW white balance and optics; cache locations and materialization state remain runtime-only. Structural commands and bypass toggles are discrete session-undo transitions; sliders and curves coalesce into gestures, including the photo-local denoise-strength gesture. These undo/redo steps are deliberately separate from durable history. After a short idle debounce, every real edit writes an immutable Recipe snapshot and atomically advances only that photo's `working` ref. Closing Shadow waits for this autosave instead of asking the user to discard changes.

Creating a named version first validates stack-wide identity invariants, then stores an immutable Recipe v1 compatibility leaf inside the content-addressed Library tree. The per-photo compatibility Recipe commit and the Library-wide commit, `heads/main`, and both named-version refs publish in one SQLite transaction with mandatory compare-and-swap guards. A Library commit therefore names one comprehensive root that can include photo edits, shared Grade Node heads, masks, Styles, and output state; it is not a collection of unrelated per-slider commits. Autosave commits intentionally create no named ref and do not advance `heads/main`, so the Versions panel remains a concise list of human-created checkpoints. Object packs may be written before publication, but a stale CAS leaves them unreachable and rolls back both commits and every ref movement. Loading an older photo version creates only an in-memory draft and never moves either durable head. Editing that draft produces a new autosaved working branch; creating a named version from it advances from the latest Library root, so newer photo commits and unrelated Library state are not rewound.

Valid persisted curves contain 2 through 256 finite points with exact x endpoints at zero and one and strictly increasing x. They are loaded without clamping. Direct authoring is narrower by design: at most 32 points, y in `[0, 1]`, and an x gap of at least `1/4096` between neighbors; endpoint x positions are fixed. An extended-range, oversized, or too-tightly-spaced legacy curve remains preserved and visible but view-only. `Reset Curve` remains explicitly available to remove the selected Grade Node's curve Render Op without changing its other controls. Each Grade Node may own one piecewise-linear tone curve; this does not implement parameter curves, independent RGB channel curves, or the full Lightroom curve surface.

Precision also keeps a bounded, in-memory undo/redo history for the current edit session. All updates between a slider press/release or one curve-point drag are coalesced into one meaningful full-stack step; Grade Node add/duplicate/delete/reorder/bypass, point add/remove, curve reset, Grade Node reset, and revert are undoable transitions. Grade Node selection is transient UI state and does not make the Recipe dirty. Opening a photo or loading a saved version as a draft clears session history. Autosave preserves this session history; a named version establishes a new durable checkpoint and clears it. Standard Undo/Redo shortcuts use the platform mapping (`Cmd+Z` and `Cmd+Shift+Z` on macOS), with visible controls in the preview toolbar.

Each durable version row summarizes its parent-relative Recipe diff. Renderer-backed controls use readable labels such as `Exposure · Tone Curve · Saturation`; curve edits, additions, and resets share the stable `Tone Curve` change label, while topology and future adjustment types use semantic fallbacks without exposing internal parameter keys or commit identifiers. The current-version badge and exact parent count remain visible beside that summary.

RAW preparation is cached for up to two recent `(representation, source fingerprint, edge)` sessions. A slider, point-curve, structure, reorder, or Grade-Node-visible update reruns the render plan and JPEG encoder; it does not reopen or decode the RAW. For a bounded AI RAW preview, strength also rebinds the retained original/full-strength-AI Camera RGB bases in memory; it does not reread `.shadowrawf`, reopen the source, rerun the model, or mix the full-resolution artifact. Once AI RAW denoise is active, a settled overview no longer speculatively prepares the hundreds-of-MiB 1:1 source; only an explicit detail viewport request pays that cost. Undo and redo restore the complete working stack through the same generation-checked preview path. Continuous gestures use a leading-edge 16 ms throttle: they cannot postpone the first frame indefinitely. A render in flight does not disable controls; a newer revision is queued while the prior render finishes, and a completed same-photo intermediate frame may be presented without declaring the generation settled. Only the exact latest generation publishes histogram state or the current-status message. A deliberately hidden selected Grade Node is different—the controls remain visible but read-only and dimmed so its preserved values stay inspectable. Preview buffers are bounded, rebuildable, and never become Catalog facts. Shared sliders reset to their declared default on double-click; adjustment sliders preserve one undoable gesture, while amount controls such as AI strength fill from the logical minimum rather than painting backward from their reset value.

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
session is Recipe-independent and reusable across ordinary slider revisions, while the
RGB viewport is rebuildable memory state and never enters Catalog or durable
version history. A newer viewport or Recipe token stops the old worker between
tiles; generation checks still reject a result if cancellation races its final
tile. The first cold request still performs a complete LibRaw
demosaic because v1 deliberately does not depend on LibRaw crop semantics. AI
foundation detail shares the same verified artifact, Recipe compiler, optics,
tile renderer, and display path as FIT, but prepares its own full-resolution
source because the bounded FIT basis cannot supply 1:1 pixels. Its cache key
therefore includes strength and retains one mixed raster rather than doubling
hundreds of MiB. Non-AI sources may use idle warmup, but an enabled AI source
stays lazy; an explicit foreground request enters the same full-detail cache
gate without making overview editing pay that cost.

This is a full-resolution parity gate for the current pixel-local Basic nodes,
including nonlinear Tone Curve, but still uses LibRaw's camera-WB, processed
linear-light RGB reference output in sRGB primaries. It is not yet Shadow's final camera-domain color
pipeline, export renderer, ICC-managed display proof, mip pyramid, GPU backend,
or neighborhood-operation tile/halo system.

The current UI authors fixed, complete Grade Nodes rather than exposing arbitrary graph wiring.
Processed-RGB white balance uses the fast adjustment/GPU path with temperature/tint and a
neutral-area picker. Photo-local RAW white balance is explicitly separate: it authors absolute
camera-domain development and currently rebuilds the prepared RAW source after a coalesced drag.
The application-wide LUT Library
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
