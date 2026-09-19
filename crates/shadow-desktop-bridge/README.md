# shadow-desktop-bridge

`shadow-desktop-bridge` is the long-lived Rust application boundary consumed by the Qt desktop
shell. It owns coarse desktop services and the CXX-facing `DesktopSession`; it does not own QML
presentation, SQLite schema details, or native image algorithms.

## Responsibility index

| Change | Primary owner |
| --- | --- |
| Desktop CXX structs, methods, and session composition | [`src/lib.rs`](src/lib.rs) |
| Versioned Recipe document projection and verified bundled LUT storage | [`src/recipe_interchange.rs`](src/recipe_interchange.rs), [`src/recipe_lut_resources.rs`](src/recipe_lut_resources.rs) |
| Frozen color-only Grade Stack projection, explicit omissions, and cancellable LUT export | [`src/lut_export.rs`](src/lut_export.rs) |
| Checkpointed smart-category classification projection | [`src/session_smart_classification.rs`](src/session_smart_classification.rs) |
| Library service composition | [`src/library_service.rs`](src/library_service.rs) |
| Ordered Library photo paging, facets, and signed visual presentation | [`src/library_service/browse.rs`](src/library_service/browse.rs) |
| Provider-independent Library viewport aggregation | [`src/library_service/map_browse.rs`](src/library_service/map_browse.rs) |
| Coordinate-bound reverse-geocoding candidate/result bridge | [`src/library_service/place_resolution.rs`](src/library_service/place_resolution.rs) |
| Library filter, order, facet, and typed cursor wire contract | [`src/library_service/query_contract.rs`](src/library_service/query_contract.rs) |
| Hierarchical keyword taxonomy, assignment provenance, batch mutation, and CXX projection | [`src/library_service/keywords.rs`](src/library_service/keywords.rs) |
| Non-destructive removal of logical photos from active Library projections | [`src/library_service/lifecycle.rs`](src/library_service/lifecycle.rs) |
| Albums, memberships, and photo-affinity state | [`src/library_service/organization.rs`](src/library_service/organization.rs) |
| Source inventory removal, source-health/missing-location projections, and all-location-confirmed missing-photo reconciliation | [`src/library_service/source_health.rs`](src/library_service/source_health.rs) |
| Manual capture/GPS correction state and single-photo mutation | [`src/library_service/metadata.rs`](src/library_service/metadata.rs) |
| Batch capture-time, manual-coordinate, and GPX preview/apply lifecycles | [`src/library_service/metadata/capture_time_batch.rs`](src/library_service/metadata/capture_time_batch.rs), [`src/library_service/metadata/coordinate_batch.rs`](src/library_service/metadata/coordinate_batch.rs), [`src/library_service/metadata/gpx.rs`](src/library_service/metadata/gpx.rs) |
| Read-only reference photo/video-folder capture-time/GPS anchors for explicit location completion | [`src/location_reference_service.rs`](src/location_reference_service.rs), [`src/location_reference_service/video_metadata.rs`](src/location_reference_service/video_metadata.rs), [`src/session_location_reference.rs`](src/session_location_reference.rs) |
| Desktop-session Library, relink, scan, and durable-export CXX delegation | [`src/session_library.rs`](src/session_library.rs), [`src/session_scan.rs`](src/session_scan.rs), [`src/session_export.rs`](src/session_export.rs) |
| Exact single-file pipeline input admission into a caller-isolated temporary Catalog | [`src/session_pipeline.rs`](src/session_pipeline.rs) |
| Desktop-session Review and cache-maintenance CXX delegation | [`src/session_review.rs`](src/session_review.rs), [`src/session_cache_maintenance.rs`](src/session_cache_maintenance.rs) |
| Authorized anonymous-person analysis, durable local-summary projection, merge/undo/clear delegation, and embedding/coordinate-free CXX projection; empty endpoint input delegates to the shared Infer Runtime Consumer resolver | [`src/session_people_analysis.rs`](src/session_people_analysis.rs) |
| Independently clearable local People Store keyed to Catalog photo identities, with stable occurrence reconciliation, representative thumbnails, and durable manual merges; face embeddings are never persisted | [`src/people_library_store.rs`](src/people_library_store.rs) |
| Session-local people-analysis job identity, cooperative cancellation between provider calls, monotonic progress, terminal state, and bounded retirement | [`src/people_analysis_service.rs`](src/people_analysis_service.rs) |
| Desktop-session bounded SigLIP text-to-image ranking, smart-category batching, and vector-free CXX projection; explicit endpoint input remains the diagnostic override | [`src/session_semantic_search.rs`](src/session_semantic_search.rs), [`src/session_smart_classification.rs`](src/session_smart_classification.rs) |
| Desktop-session resumable Qwen image-understanding batches, rebuildable description/keyword proposals, and closed-set classification review with explicit acceptance or dismissal | [`src/session_image_understanding.rs`](src/session_image_understanding.rs) |
| Canonical wall-clock conversion and digest encoding | [`src/wall_clock.rs`](src/wall_clock.rs), [`src/digest_hex.rs`](src/digest_hex.rs) |
| Shared default Infer Runtime credential location across RAW denoise, subject masks, and image completion | [`src/infer_runtime_credentials.rs`](src/infer_runtime_credentials.rs) |
| Folder import lifecycle | [`src/scan_service.rs`](src/scan_service.rs) |
| Folder-owned source relinking from scan evidence, an unavailable Library card, or a configured source: verified attachment, replacement-root adoption, unresolved counts, and safe obsolete-source retirement | [`src/relink_service.rs`](src/relink_service.rs) |
| Replacement-folder planning: old-directory sibling/source-tree discovery, exact-identity matching, bounded legacy identity bootstrapping, ambiguity rejection, and pre-scan duplicate prevention | [`src/relink_service/folder_recovery.rs`](src/relink_service/folder_recovery.rs) |
| Review presentation, decisions, and comparison evidence | [`src/review_service.rs`](src/review_service.rs) |
| Exact selected-photo EXIF and technical inspection, independent from virtualized Review pages | [`src/photo_inspection_service.rs`](src/photo_inspection_service.rs), [`src/session_photo_inspection.rs`](src/session_photo_inspection.rs) |
| Authenticated remote-Mac Library mirror, client-local curation, verified original cache residency, and local Catalog edit admission without losing remote origin | [`src/remote_library_service.rs`](src/remote_library_service.rs), [`src/session_remote_library.rs`](src/session_remote_library.rs) |
| This Mac's managed Library-server lifecycle, embedded-preview-first Provider routing, shared-root scanning, rebuildable development-schema recovery, cache reset, and CXX projection | [`src/library_server_service.rs`](src/library_server_service.rs), [`src/library_server_service/provider_host.rs`](src/library_server_service/provider_host.rs), [`src/library_server_host.rs`](src/library_server_host.rs), [`src/session_library_server.rs`](src/session_library_server.rs) |
| Photo source admission, Provider Host inventory, quarantine, optics discovery, and raster delivery | [`src/session_photo_source.rs`](src/session_photo_source.rs), [`src/photo_provider.rs`](src/photo_provider.rs), [`src/isolated_proxy.rs`](src/isolated_proxy.rs), [`src/isolated_proxy/provider_inventory.rs`](src/isolated_proxy/provider_inventory.rs) |
| Typed Infer Runtime SAM 2.1 probability-mask consumption, transient person detection and face parsing from one prepared in-memory JPEG, local-only provenance admission, and proposal staging; Shadow does not install or host the models | [`src/subject_mask_runtime.rs`](src/subject_mask_runtime.rs) |
| Transient per-photo person thumbnails, exact face-parsing ontology, multi-region label composition, and bounded session payloads | [`src/subject_mask_people.rs`](src/subject_mask_people.rs) |
| Bounded input reuse, discovered-person and parsed-label caches, proposal preview, registration, apply, and discard authority | [`src/subject_mask_service.rs`](src/subject_mask_service.rs) |
| Edit-session subject-mask input preparation, point refinement, people/detail selection, and apply orchestration | [`src/session_subject_mask.rs`](src/session_subject_mask.rs) |
| Destination-safe semantic Recipe import planning, bounded per-session item lifecycles, target-identity validation, and CXX projection | [`src/recipe_import_plan.rs`](src/recipe_import_plan.rs), [`src/recipe_import_service.rs`](src/recipe_import_service.rs), [`src/session_recipe_import.rs`](src/session_recipe_import.rs) |
| Infer Runtime RAW foundation execution through typed lease/SCM_RIGHTS handles, cache-before-runtime alias resolution, cancellation, independent `.shadowrawf` verification/publication, and unavailable-feature status without local-model fallback | [`src/raw_foundation_runtime.rs`](src/raw_foundation_runtime.rs), [`src/raw_foundation_runtime/infer_materialization.rs`](src/raw_foundation_runtime/infer_materialization.rs), [`src/raw_foundation_runtime/infer_materialization/alias_store.rs`](src/raw_foundation_runtime/infer_materialization/alias_store.rs), [`src/isolated_proxy/raw_frame_staging.rs`](src/isolated_proxy/raw_frame_staging.rs) |
| Cancellable RAW foundation job progress and terminal descriptor ownership | [`src/raw_foundation_service.rs`](src/raw_foundation_service.rs) |
| Session-ready and restart-resolved `.shadowrawf` association, re-verification, exact zero-strength bypass, strength-independent verified-artifact identity, and bounded native render transfer | [`src/raw_foundation_render_source.rs`](src/raw_foundation_render_source.rs) |
| Desktop-session RAW foundation model probing and opaque job delegation | [`src/session_raw_foundation.rs`](src/session_raw_foundation.rs) |
| Bounded persistent grid-proxy identity from the complete RAW plan and optional isolated-helper graph | [`src/photo_provider/grid_proxy_identity.rs`](src/photo_provider/grid_proxy_identity.rs) |
| Preview identity, cancellation, session-local reuse, exact optional RAW-foundation source identity, bounded strength rebinding over one retained original/AI basis, and isolated-helper RawFrame staging for manual RAW white balance when the public decoder cannot open the locally materialized source | [`src/preview_cache_identity.rs`](src/preview_cache_identity.rs), [`src/preview_render_registry.rs`](src/preview_render_registry.rs), [`src/session_preview_store.rs`](src/session_preview_store.rs), [`src/edit_preview/warm_session_cache.rs`](src/edit_preview/warm_session_cache.rs) |
| 1:1 detail tile geometry and prepared-source reuse keyed by the optional RAW foundation plus its render-only strength; manual RAW white balance retains the same staged provider-neutral RawFrame route as preview/export | [`src/detail_viewport.rs`](src/detail_viewport.rs), [`src/detail_tile_cache.rs`](src/detail_tile_cache.rs), [`src/session_edit_render.rs`](src/session_edit_render.rs), [`src/export_service.rs`](src/export_service.rs) |
| Interactive RGB8 preview transaction, settled JPEG/analysis policy, terminal linearization, durable-publication call, stable cross-language payload lifetime, and descriptor projection | [`src/edit_preview/service.rs`](src/edit_preview/service.rs), [`src/edit_preview/owned_response.rs`](src/edit_preview/owned_response.rs), [`src/edit_preview/response.rs`](src/edit_preview/response.rs) |
| Desktop-session full-detail viewport, AI RAW-foundation routing, and prepared-source lifecycle | [`src/session_edit_render.rs`](src/session_edit_render.rs) |
| Settled Recipe-preview identity, blob storage, and Catalog publication | [`src/edit_preview/recipe_preview_store.rs`](src/edit_preview/recipe_preview_store.rs) |
| Before-preview composition projection: retain geometry and optics, discard grading and RAW development edits, without inheriting the working Recipe | [`src/recipe_v1/render_request.rs`](src/recipe_v1/render_request.rs) |
| Working drafts, named versions, checkout, and edit-reference publication | [`src/session_edit_history.rs`](src/session_edit_history.rs) |
| Photo Variant lifecycle, active-head projection, and stale-editor isolation | [`src/session_photo_variants.rs`](src/session_photo_variants.rs), [`src/session_edit_history.rs`](src/session_edit_history.rs) |
| Bounded per-photo Recipe history, Library-wide commit/ref paging, reference decoration, and semantic diff projection | [`src/history_service.rs`](src/history_service.rs), [`src/session_history.rs`](src/session_history.rs) |
| Recipe v1 draft model and complete Foundation/Grade Stack FFI translation, including the independent photo-local AI RAW Denoise singleton, path-free model intent, persisted denoise and per-Grade-Node strength, historical v1 wire compatibility, validation, stable identity, snapshot codec and layout, verified managed-raster resolution, one immutable source-development contract shared by preview/detail/export, render-plan compilation, and version summaries | [`src/recipe_v1.rs`](src/recipe_v1.rs), [`src/recipe_v1/draft.rs`](src/recipe_v1/draft.rs), [`src/recipe_v1/ffi_adapter.rs`](src/recipe_v1/ffi_adapter.rs), [`src/recipe_v1/validation.rs`](src/recipe_v1/validation.rs), [`src/recipe_v1/identity.rs`](src/recipe_v1/identity.rs), [`src/recipe_v1/snapshot_encode.rs`](src/recipe_v1/snapshot_encode.rs), [`src/recipe_v1/snapshot_decode.rs`](src/recipe_v1/snapshot_decode.rs), [`src/recipe_v1/snapshot_layout.rs`](src/recipe_v1/snapshot_layout.rs), [`src/recipe_v1/managed_raster_resolution.rs`](src/recipe_v1/managed_raster_resolution.rs), [`src/recipe_v1/foundation_development.rs`](src/recipe_v1/foundation_development.rs), [`src/recipe_v1/render_request.rs`](src/recipe_v1/render_request.rs), [`src/recipe_v1/compiler.rs`](src/recipe_v1/compiler.rs), [`src/edit_version_diff.rs`](src/edit_version_diff.rs) |
| Strict flat Liquify DTO decoding and bounded node construction | [`src/recipe_v1/ffi_adapter/liquify.rs`](src/recipe_v1/ffi_adapter/liquify.rs) |
| Condition-mask executability and opaque managed-raster gates between persistent Recipe values, the flat Qt DTO, verified application storage, and native render plans | [`src/recipe_v1/snapshot_decode.rs`](src/recipe_v1/snapshot_decode.rs), [`src/recipe_v1/ffi_adapter.rs`](src/recipe_v1/ffi_adapter.rs), [`src/recipe_v1/managed_raster_resolution.rs`](src/recipe_v1/managed_raster_resolution.rs), [`src/recipe_v1/compiler.rs`](src/recipe_v1/compiler.rs) |
| Shared Grade Node library, application, and desktop-session orchestration | [`src/shared_grade_library.rs`](src/shared_grade_library.rs), [`src/shared_grade_application.rs`](src/shared_grade_application.rs), [`src/session_shared_grade.rs`](src/session_shared_grade.rs) |
| Recipe-exact raster export execution (including fail-closed AI RAW-foundation routing and provider-neutral RawFrame staging when the public decoder cannot develop the source), durable queueing, and source-stage RAW DNG publication that preserves the staged CFA mosaic and source calibration without applying the Recipe; isolated display JPEGs are never substituted for RAW export | [`src/export_service.rs`](src/export_service.rs), [`src/export_queue_service.rs`](src/export_queue_service.rs), [`src/raw_dng_export.rs`](src/raw_dng_export.rs), [`src/isolated_proxy/raw_frame_staging.rs`](src/isolated_proxy/raw_frame_staging.rs) |
| Cache ownership and explicit maintenance | [`src/cache_maintenance_service.rs`](src/cache_maintenance_service.rs) |
| Cross-responsibility facade contracts and responsibility-owned fixtures | [`src/tests/mod.rs`](src/tests/mod.rs), [`src/tests/fixtures/mod.rs`](src/tests/fixtures/mod.rs) |

Add behavior to the module that owns its lifecycle and failure policy. Change `lib.rs` only when
the CXX contract, session composition, or a narrow delegation must change. A new independent
desktop workflow should begin in a responsibility-named service and be wired through the facade
after its contract is stable.

The interactive response is an ownership transaction, not a pixel-vector return. On a native
Metal warm session, `owned_response.rs` retains the opaque bridge owner and `response.rs` projects
only the validated storage kind, texture/device handles, row stride, pixel format, resource
identity, and optional paired mask view. Host materialization remains an explicit fallback method.
Settled output continues to publish encoded JPEG plus analysis; the native presentation descriptor
is transient and never enters Catalog or cache identity. The desktop's explicit
`PresentationCommit` policy uses the same settled renderer and identities, but completes the blob
and Catalog transaction before returning so the Precision-to-Library boundary cannot observe a
saved Recipe with an older visual. It remains worker-thread work and is not an interactive render
mode.

Recipe v1 keeps existing single Oklab-lightness and zero-minimum-chroma Oklch-hue masks on their
byte-stable, GPU-executable representation. The domain can also persist bounded `all`/`any`/`not`
condition expressions, chroma-qualified predicates, and a scale-explicit local-detail predicate.
Those richer values currently fail before Qt projection and before render-plan execution; they are
not flattened, silently omitted, or advertised as active UI features. The next execution slice must
extend the Qt/native mask protocol and prove CPU/Metal parity before removing either gate.

The Recipe draft and flat Qt DTO expose the optional, photo-private Liquify structural node as one
enabled flag plus an ordered tagged Push/Reconstruct vector. Empty plus disabled is the canonical
absent node; every non-empty vector materializes exactly one validated `PhotoLiquifyNode`, and
Reconstruct is accepted only after prior Push deformation. Points retain original-image normalized
coordinates and pressure; radius, strength, hardness, and bypass round-trip exactly. Snapshot
encoding uses that editable projection directly, so clearing Liquify removes it even when a base
Recipe contained one, while unrelated Canvas and Grade Node edits preserve the decoded value.

The persistent soft-mask math is closed in `shadow-domain`'s condition-mask `reference` owner.
`all`, `any`, and `not` mean exact minimum, maximum, and `1 - x`; scalar ranges retain the quintic
smootherstep feather. Hue retains the current relative-chroma confidence curve and relative
half-width feather, while a nonzero absolute minimum-chroma gate has its own cubic feather. Local
detail uses a complete-render, edge-clamped `(2r + 1)²` Oklab-lightness box, fixed residual
normalization, and rational full-render scale with round-half-up radius conversion; tile dimensions
do not alter its footprint.

## Test layout

Small private-invariant tests remain in their production service. The crate-local `src/tests/`
tree exercises contracts that need private access across the facade and is indexed by product
responsibility rather than by numbered test parts. Public behavior that does not need crate-private
access belongs in the package-level `tests/` directory. Each suite imports its production contract
and shared fixture owner directly; `src/tests/mod.rs` supplies no implicit test prelude. Grade Stack,
edit-session, feedback-evidence, and Review-comparison fixtures are indexed in
[`src/tests/fixtures/mod.rs`](src/tests/fixtures/mod.rs), with no generic support bucket.
Perceptual Color DTO validation lives in
[`src/tests/perceptual_color_contract.rs`](src/tests/perceptual_color_contract.rs), while Oklab
Color Warper DTO, persistence, and render-plan behavior lives in
[`src/tests/oklab_color_warper_contract.rs`](src/tests/oklab_color_warper_contract.rs).
[`src/tests/adjustment_contract.rs`](src/tests/adjustment_contract.rs) retains cross-family
Fine Edit round trips plus the independent basic, curve, LUT, and non-color validation contracts.

Run focused validation with build output outside the shared source worktree:

```sh
sh scripts/local_shared_workspace_guard.sh
CARGO_TARGET_DIR=/absolute/task-private/path cargo test -p shadow-desktop-bridge --lib
```
